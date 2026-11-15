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

#define HTTP_HEADER_MAX_LEN 256u  // Status line + headers of one response
#define HTTP_METHOD_MAX_LEN 8u    // "GET", "POST", "OPTIONS", ...

_Static_assert(HTTP_SOCKET_FIRST + HTTP_SOCKET_COUNT <= NET_DHCP_SOCKET, "HTTP sockets overlap the DHCP socket");

static uint32_t http_request_count = 0;  // Requests answered since start-up
static uint32_t http_started_ms = 0;     // HAL_GetTick() when the sockets were opened, 0 = not running
static uint32_t http_config_version;     // Last NET_GetInfo()->config_version seen

// Shared buffers: a request is read and answered within one call, so one set is enough for all sockets
static char http_rx_buf[HTTP_REQ_BUF_SIZE];
static char http_json_buf[STATUS_JSON_MAX_LEN];
static char http_tx_buf[HTTP_HEADER_MAX_LEN + STATUS_JSON_MAX_LEN];  // headers + a small body in one send()

typedef struct {
    uint32_t connected_ms;  // HAL_GetTick() when the client was first seen in ESTABLISHED, 0 = no client
    uint32_t closing_ms;    // HAL_GetTick() when the socket started closing, 0 = not closing
} http_socket_t;

static http_socket_t http_sockets[HTTP_SOCKET_COUNT];

// 0 means "not set" in http_socket_t, so never store 0 as a time
static uint32_t http_timestamp(uint32_t now) {
    return (now != 0) ? now : 1u;
}

static void http_reset_socket_state(uint8_t sn) {
    http_socket_t* s = &http_sockets[sn - HTTP_SOCKET_FIRST];
    s->connected_ms = 0;
    s->closing_ms = 0;
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
        if (n <= 0) return false;  // < 0: error / closed; 0 (SOCK_BUSY) can't happen in blocking mode
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
static bool http_send_response(uint8_t sn, int code, const char* content_type, const char* body, uint32_t body_len,
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
    const char* method = NULL;
    const char* path = NULL;
    char* eol = strstr(http_rx_buf, "\r\n");

    if (eol == NULL) {  // request line incomplete or longer than the buffer
        sent = http_send_text(sn, 400, "Bad Request\n", NULL);
    } else {
        *eol = '\0';
        if (http_parse_request_line(http_rx_buf, &method, &path)) {
            sent = http_route(sn, method, path, now);
        } else {
            sent = http_send_text(sn, 400, "Bad Request\n", NULL);
        }
    }

    http_request_count++;

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
            break;

        case SOCK_INIT:
            listen(sn);  // on failure the library closes the socket, reopened next pass
            break;

        case SOCK_LISTEN:
        case SOCK_SYNRECV: break;  // waiting for a client / handshake in progress

        case SOCK_ESTABLISHED:
            if (s->closing_ms != 0) break;  // disconnect already issued, state not updated yet
            if (s->connected_ms == 0) s->connected_ms = http_timestamp(now);

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
