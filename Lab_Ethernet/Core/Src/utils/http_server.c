#include "utils/http_server.h"
#include "socket.h"
#include "utils/net.h"
#include "utils/status.h"
#include "utils/web_page.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * Call structure:
 *
 *   HTTP_Process
 *    ├─ http_close_all                 network down / IP config changed
 *    └─ http_process_socket  (x4)      per-socket state machine
 *        ├─ http_disconnect            CLOSE_WAIT
 *        └─ http_handle_request        data arrived: recv, parse request line
 *            ├─ http_route             pick the response
 *            │   └─ http_send_response status line + headers + body
 *            │       └─ http_send_all  send() until everything is out
 *            └─ http_disconnect
 */

#define HTTP_HEADER_MAX_LEN   256u  // Status line + headers of one response
#define HTTP_METHOD_MAX_LEN   8u    // "GET", "POST", "OPTIONS", ...
#define HTTP_WS_FRAME_MAX_LEN 128u

_Static_assert(HTTP_SOCKET_FIRST + HTTP_SOCKET_COUNT <= NET_DHCP_SOCKET, "HTTP sockets overlap the DHCP socket");

static uint32_t http_request_count = 0;  // Requests answered since start-up
static uint32_t http_started_ms = 0;     // HAL_GetTick() when the sockets were opened, 0 = not running
static uint32_t http_config_version;     // Last NET_GetInfo()->config_version seen

// Shared buffers: a request is read and answered within one call, so one set is enough for all sockets
static char http_rx_buf[HTTP_REQ_BUF_SIZE];
static char http_json_buf[STATUS_JSON_MAX_LEN];
static uint8_t http_cbor_buf[STATUS_CBOR_MAX_LEN];
static char http_tx_buf[HTTP_HEADER_MAX_LEN + STATUS_JSON_MAX_LEN];  // headers + a small body in one send()
static uint8_t http_ws_frame_buf[HTTP_WS_FRAME_MAX_LEN];

typedef struct {
    uint32_t connected_ms;  // HAL_GetTick() when the client was first seen in ESTABLISHED, 0 = no client
    uint32_t closing_ms;    // HAL_GetTick() when the socket started closing, 0 = not closing
    uint32_t next_ws_ms;
    bool websocket;
} http_socket_t;

static http_socket_t http_sockets[HTTP_SOCKET_COUNT];

static bool http_send_all(uint8_t sn, const uint8_t* data, uint32_t len);

// 0 means "not set" in http_socket_t, so never store 0 as a time
static uint32_t http_timestamp(uint32_t now) {
    return (now != 0) ? now : 1u;
}

static void http_reset_socket_state(uint8_t sn) {
    http_socket_t* s = &http_sockets[sn - HTTP_SOCKET_FIRST];
    s->connected_ms = 0;
    s->closing_ms = 0;
    s->next_ws_ms = 0;
    s->websocket = false;
}

static uint32_t http_sha1_rotate(uint32_t value, uint8_t bits) {
    return (value << bits) | (value >> (32u - bits));
}

static void http_sha1(const uint8_t* input, size_t length, uint8_t digest[20]) {
    uint8_t block[64];
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    uint64_t bit_length = (uint64_t) length * 8u;
    size_t padded = ((length + 9u + 63u) / 64u) * 64u;

    for (size_t offset = 0; offset < padded; offset += 64u) {
        memset(block, 0, sizeof(block));
        for (size_t i = 0; i < 64u; i++) {
            size_t source = offset + i;
            if (source < length)
                block[i] = input[source];
            else if (source == length)
                block[i] = 0x80u;
        }
        if (offset + 64u == padded) {
            for (uint8_t i = 0; i < 8u; i++) block[56u + i] = (uint8_t) (bit_length >> (56u - 8u * i));
        }

        uint32_t w[80];
        for (uint8_t i = 0; i < 16u; i++) {
            w[i] = ((uint32_t) block[4u * i] << 24) | ((uint32_t) block[4u * i + 1u] << 16) |
                   ((uint32_t) block[4u * i + 2u] << 8) | block[4u * i + 3u];
        }
        for (uint8_t i = 16u; i < 80u; i++) w[i] = http_sha1_rotate(w[i - 3u] ^ w[i - 8u] ^ w[i - 14u] ^ w[i - 16u], 1);

        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (uint8_t i = 0; i < 80u; i++) {
            uint32_t f, k;
            if (i < 20u) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999u;
            } else if (i < 40u) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60u) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            uint32_t temp = http_sha1_rotate(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = http_sha1_rotate(b, 30);
            b = a;
            a = temp;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    for (uint8_t i = 0; i < 5u; i++)
        for (uint8_t j = 0; j < 4u; j++) digest[4u * i + j] = (uint8_t) (h[i] >> (24u - 8u * j));
}

static size_t http_base64(const uint8_t* input, size_t length, char* output, size_t capacity) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t pos = 0;
    for (size_t i = 0; i < length; i += 3u) {
        uint32_t value = (uint32_t) input[i] << 16;
        if (i + 1u < length) value |= (uint32_t) input[i + 1u] << 8;
        if (i + 2u < length) value |= input[i + 2u];
        if (pos + 4u >= capacity) return 0;
        output[pos++] = alphabet[(value >> 18) & 63u];
        output[pos++] = alphabet[(value >> 12) & 63u];
        output[pos++] = i + 1u < length ? alphabet[(value >> 6) & 63u] : '=';
        output[pos++] = i + 2u < length ? alphabet[value & 63u] : '=';
    }
    output[pos] = '\0';
    return pos;
}

static bool http_ws_handshake(uint8_t sn, uint32_t now) {
    const char* marker = "Sec-WebSocket-Key:";
    char* key = strstr(http_rx_buf, marker);
    if (key == NULL) return false;
    key += strlen(marker);
    while (*key == ' ') key++;
    char* end = strstr(key, "\r\n");
    if (end == NULL || (size_t) (end - key) >= 64u) return false;

    char source[128];
    size_t key_len = (size_t) (end - key);
    memcpy(source, key, key_len);
    memcpy(source + key_len, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", 36u);
    uint8_t digest[20];
    char accept[32];
    http_sha1((const uint8_t*) source, key_len + 36u, digest);
    if (http_base64(digest, sizeof(digest), accept, sizeof(accept)) == 0) return false;

    int length = snprintf(http_tx_buf, sizeof(http_tx_buf),
                          "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                          "Sec-WebSocket-Accept: %s\r\n\r\n",
                          accept);
    if (length <= 0 || !http_send_all(sn, (const uint8_t*) http_tx_buf, (uint32_t) length)) return false;
    http_sockets[sn - HTTP_SOCKET_FIRST].websocket = true;
    http_sockets[sn - HTTP_SOCKET_FIRST].next_ws_ms = now;
    return true;
}

static bool http_ws_send_status(uint8_t sn, uint32_t now) {
    size_t length = Status_FormatJson(http_json_buf, sizeof(http_json_buf), now);
    if (length == 0 || length > 65535u) return false;
    http_ws_frame_buf[0] = 0x81u;
    if (length <= 125u) {
        http_ws_frame_buf[1] = (uint8_t) length;
        if (!http_send_all(sn, http_ws_frame_buf, 2u)) return false;
    } else {
        http_ws_frame_buf[1] = 126u;
        http_ws_frame_buf[2] = (uint8_t) (length >> 8);
        http_ws_frame_buf[3] = (uint8_t) length;
        if (!http_send_all(sn, http_ws_frame_buf, 4u)) return false;
    }
    return http_send_all(sn, (const uint8_t*) http_json_buf, (uint32_t) length);
}

static bool http_ws_process_input(uint8_t sn) {
    uint8_t frame[HTTP_WS_FRAME_MAX_LEN];
    int32_t length = recv(sn, frame, sizeof(frame));
    if (length <= 0) return false;
    if ((frame[0] & 0x0Fu) == 0x8u) return false;
    if ((frame[0] & 0x0Fu) == 0x9u && length >= 2) {
        uint8_t pong[HTTP_WS_FRAME_MAX_LEN];
        uint8_t payload = frame[1] & 0x7Fu;
        if (payload > sizeof(pong) - 2u || length < (int32_t) (2u + payload)) return true;
        pong[0] = 0x8Au;
        pong[1] = payload;
        memcpy(pong + 2, frame + 2, payload);
        return http_send_all(sn, pong, (uint32_t) payload + 2u);
    }
    return true;
}

static void http_close_all(void) {
    for (uint8_t sn = HTTP_SOCKET_FIRST; sn < HTTP_SOCKET_FIRST + HTTP_SOCKET_COUNT; ++sn) {
        close(sn);
        http_reset_socket_state(sn);
    }
}

/*
 * Polite close (FIN) without blocking: in the default blocking mode the library's
 * disconnect() waits until the peer finishes the close or the TCP timeout fires.
 * In non-blocking mode it only issues the command; http_process_socket() then
 * waits for SOCK_CLOSED. socket() resets the IO mode when the socket is reopened.
 */
static void http_disconnect(uint8_t sn, uint32_t now) {
    uint8_t io_mode = SOCK_IO_NONBLOCK;
    ctlsocket(sn, CS_SET_IOMODE, &io_mode);
    disconnect(sn);
    http_sockets[sn - HTTP_SOCKET_FIRST].closing_ms = http_timestamp(now);
}

static const char* http_reason(int code) {
    switch (code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        default: return "";
    }
}

/*
 * send() until all `len` bytes are queued. The library's blocking send() takes at
 * most the socket TX buffer size (2 KB) and may take less than offered.
 * Returns false if the socket died (the library has closed it then).
 */
static bool http_send_all(uint8_t sn, const uint8_t* data, uint32_t len) {
    uint32_t off = 0;
    while (off < len) {
        uint32_t chunk = len - off;
        if (chunk > HTTP_TX_CHUNK_SIZE) chunk = HTTP_TX_CHUNK_SIZE;

        int32_t n = send(sn, (uint8_t*) data + off, (uint16_t) chunk);
        if (n == SOCK_BUSY) continue;  // W5500 is still completing the previous SEND command
        if (n <= 0) return false;      // socket error / closed
        off += (uint32_t) n;
    }
    return true;
}

/*
 * Status line + headers + body. `extra_headers` is NULL or complete header lines
 * ending in "\r\n" (CORS for /status, Allow for 405).
 * Small responses (header + body fit in http_tx_buf) go out in one send(), so
 * usually in one TCP segment; larger bodies (the page) follow the headers in chunks.
 */
static bool http_send_response(uint8_t sn, int code, const char* content_type, const void* body, uint32_t body_len,
                               const char* extra_headers) {
    int hlen = snprintf(http_tx_buf, HTTP_HEADER_MAX_LEN,
                        "HTTP/1.1 %d %s\r\n"
                        "Content-Type: %s\r\n"
                        "Content-Length: %lu\r\n"
                        "Cache-Control: no-store\r\n"
                        "%s"
                        "Connection: close\r\n"
                        "\r\n",
                        code, http_reason(code), content_type, (unsigned long) body_len,
                        extra_headers ? extra_headers : "");
    if (hlen < 0 || (uint32_t) hlen >= HTTP_HEADER_MAX_LEN) return false;

    if ((uint32_t) hlen + body_len <= sizeof(http_tx_buf)) {
        memcpy(http_tx_buf + hlen, body, body_len);
        return http_send_all(sn, (const uint8_t*) http_tx_buf, (uint32_t) hlen + body_len);
    }

    if (!http_send_all(sn, (const uint8_t*) http_tx_buf, (uint32_t) hlen)) return false;
    return http_send_all(sn, (const uint8_t*) body, body_len);
}

static bool http_send_text(uint8_t sn, int code, const char* text, const char* extra_headers) {
    return http_send_response(sn, code, "text/plain; charset=utf-8", text, (uint32_t) strlen(text), extra_headers);
}

// Choose and send the response for one parsed request
static bool http_route(uint8_t sn, const char* method, const char* path, uint32_t now) {
    if (strcmp(method, "GET") != 0) { return http_send_text(sn, 405, "Method Not Allowed\n", "Allow: GET\r\n"); }

    if (strcmp(path, "/") == 0) {
        return http_send_response(sn, 200, "text/html; charset=utf-8", WEB_INDEX_HTML, (uint32_t) WEB_INDEX_HTML_LEN,
                                  NULL);
    }

    if (strcmp(path, "/status") == 0) {
        size_t len = Status_FormatJson(http_json_buf, sizeof(http_json_buf), now);
        if (len == 0) return http_send_text(sn, 500, "Status does not fit the buffer\n", NULL);
        return http_send_response(sn, 200, "application/json", http_json_buf, (uint32_t) len,
                                  "Access-Control-Allow-Origin: *\r\n");
    }

    if (strcmp(path, "/status.cbor") == 0) {
        size_t len = Status_FormatCbor(http_cbor_buf, sizeof(http_cbor_buf), now);
        if (len == 0) return http_send_text(sn, 500, "Status does not fit the buffer\n", NULL);
        return http_send_response(sn, 200, "application/cbor", http_cbor_buf, (uint32_t) len,
                                  "Access-Control-Allow-Origin: *\r\n");
    }

    return http_send_text(sn, 404, "Not Found\n", NULL);
}

/*
 * Split "METHOD PATH[?query] VERSION" in place. `line` must be the '\0'-terminated
 * first line without "\r\n". The version is optional and ignored.
 * Returns false if the line has no method/path.
 */
static bool http_parse_request_line(char* line, const char** method, const char** path) {
    char* sp = strchr(line, ' ');
    if (sp == NULL || sp == line || (size_t) (sp - line) >= HTTP_METHOD_MAX_LEN) return false;
    *sp = '\0';
    *method = line;

    char* p = sp + 1;
    if (*p != '/') return false;
    char* end = strchr(p, ' ');
    if (end != NULL) *end = '\0';
    char* query = strchr(p, '?');
    if (query != NULL) *query = '\0';
    *path = p;
    return true;
}

/*
 * Read the request waiting on `sn`, answer it and start the disconnect.
 * Only the request line is used; headers are read and dropped.
 */
static void http_handle_request(uint8_t sn, uint32_t now) {
    int32_t n = recv(sn, (uint8_t*) http_rx_buf, HTTP_REQ_BUF_SIZE - 1);
    if (n <= 0) {  // socket closed under us; the library has closed it
        http_reset_socket_state(sn);
        return;
    }
    http_rx_buf[n] = '\0';

    // Drop whatever else already arrived (long browser headers): closing with unread
    // data in the RX buffer can make the peer see a reset instead of the response
    while (getSn_RX_RSR(sn) > 0) {
        static uint8_t discard[64];
        if (recv(sn, discard, sizeof(discard)) <= 0) break;
    }

    bool sent;
    bool upgraded = false;
    const char* method = NULL;
    const char* path = NULL;
    char* eol = strstr(http_rx_buf, "\r\n");

    if (eol == NULL) {  // request line incomplete or longer than the buffer
        sent = http_send_text(sn, 400, "Bad Request\n", NULL);
    } else if (strncmp(http_rx_buf, "GET /ws ", 8u) == 0) {
        // Keep the original request intact: the handshake needs headers after the request line.
        sent = http_ws_handshake(sn, now);
        upgraded = sent;
    } else {
        *eol = '\0';
        if (http_parse_request_line(http_rx_buf, &method, &path)) {
            sent = http_route(sn, method, path, now);
        } else {
            sent = http_send_text(sn, 400, "Bad Request\n", NULL);
        }
    }

    http_request_count++;

    if (sent && upgraded) { return; }
    if (sent) {
        http_disconnect(sn, now);
    } else {
        close(sn);  // send failed: the connection is broken, no point in a polite close
        http_reset_socket_state(sn);
    }
}

/*
 * One step of the state machine of HTTP socket `sn`. Called for every HTTP socket on
 * every main-loop pass while the network is up. Never blocks.
 *
 *   CLOSED -> socket() -> INIT -> listen() -> LISTEN -> (client) -> ESTABLISHED
 *   ESTABLISHED + data -> http_handle_request() -> closing states -> CLOSED
 */
static void http_process_socket(uint8_t sn, uint32_t now) {
    http_socket_t* s = &http_sockets[sn - HTTP_SOCKET_FIRST];

    switch (getSn_SR(sn)) {
        case SOCK_CLOSED:
            http_reset_socket_state(sn);
            socket(sn, Sn_MR_TCP, HTTP_PORT, SF_TCP_NODELAY);  // on failure stays CLOSED, retried next pass
            {
                uint8_t io_mode = SOCK_IO_BLOCK;
                ctlsocket(sn, CS_SET_IOMODE, &io_mode);
            }
            break;

        case SOCK_INIT:
            listen(sn);  // on failure the library closes the socket, reopened next pass
            break;

        case SOCK_LISTEN:
        case SOCK_SYNRECV: break;  // waiting for a client / handshake in progress

        case SOCK_ESTABLISHED:
            if (s->closing_ms != 0) break;  // disconnect already issued, state not updated yet
            if (s->connected_ms == 0) s->connected_ms = http_timestamp(now);

            if (s->websocket) {
                if (getSn_RX_RSR(sn) > 0 && !http_ws_process_input(sn)) {
                    close(sn);
                    http_reset_socket_state(sn);
                    break;
                }
                if (now - s->next_ws_ms >= 1000u) {
                    if (!http_ws_send_status(sn, now)) {
                        close(sn);
                        http_reset_socket_state(sn);
                    } else {
                        s->next_ws_ms = now;
                    }
                }
                break;
            }

            if (getSn_RX_RSR(sn) > 0) {
                http_handle_request(sn, now);
            } else if (now - s->connected_ms > HTTP_IDLE_TIMEOUT_MS) {
                close(sn);  // connected but silent: free the socket for real clients
                http_reset_socket_state(sn);
            }
            break;

        case SOCK_CLOSE_WAIT:  // client closed its side; a request may still be in the buffer
            if (s->closing_ms != 0) break;
            if (getSn_RX_RSR(sn) > 0) {
                http_handle_request(sn, now);
            } else {
                http_disconnect(sn, now);
            }
            break;

        case SOCK_FIN_WAIT:
        case SOCK_CLOSING:
        case SOCK_TIME_WAIT:
        case SOCK_LAST_ACK:  // closing, the chip finishes the FIN exchange
            if (s->closing_ms == 0) s->closing_ms = http_timestamp(now);
            if (now - s->closing_ms > HTTP_CLOSE_TIMEOUT_MS) {
                close(sn);  // peer gone, don't wait for the TCP timeout
                http_reset_socket_state(sn);
            }
            break;

        default:  // not a TCP state (UDP, MACRAW, ...): should never happen on these sockets
            close(sn);
            http_reset_socket_state(sn);
            break;
    }
}

void HTTP_Init(void) {
    memset(http_sockets, 0, sizeof(http_sockets));
    http_request_count = 0;
    http_started_ms = 0;
    http_config_version = 0;
}

void HTTP_Process(uint32_t now) {
    if (!NET_IsUp()) {
        // Only touch the chip if the server ran: with no W5500, close() would spin forever
        if (http_started_ms != 0) {
            http_close_all();
            http_started_ms = 0;
        }
        return;
    }

    uint32_t version = NET_GetInfo()->config_version;
    if (version != http_config_version) {  // first IP after boot, new DHCP address or switch to static
        http_close_all();
        http_config_version = version;
        http_started_ms = http_timestamp(now);
    }

    for (uint8_t sn = HTTP_SOCKET_FIRST; sn < HTTP_SOCKET_FIRST + HTTP_SOCKET_COUNT; ++sn) {
        http_process_socket(sn, now);
    }
}

uint32_t HTTP_GetUptimeS(uint32_t now) {
    if (http_started_ms == 0) return 0;
    return (now - http_started_ms) / 1000u;
}

uint32_t HTTP_GetRequestCount(void) {
    return http_request_count;
}
