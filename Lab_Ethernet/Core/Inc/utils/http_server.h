#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

/*
 * Minimal HTTP/1.1 server on TCP port 80, on top of the W5500 sockets.
 *
 * Routes:
 *   GET /        -> the static HTML page (text/html). It polls /status once
 *                   per second and updates only the value text nodes.
 *   GET /status  -> Status_FormatJson() (application/json), target < 10 ms.
 *   anything else -> 404 Not Found.  Non-GET methods -> 405 Method Not Allowed.
 *
 * Every response is sent with:
 *   Content-Type, Content-Length, Connection: close, Cache-Control: no-store
 *   and, for /status, Access-Control-Allow-Origin: *.
 * The connection is closed after one response (no keep-alive), which keeps
 * the state per socket trivial.
 *
 * Each socket in HTTP_SOCKET_FIRST .. HTTP_SOCKET_FIRST + HTTP_SOCKET_COUNT - 1
 * listens on port 80 on its own, so a browser can open a few connections at
 * the same time. Per-socket state machine, polled with getSn_SR():
 *
 *   SOCK_CLOSED       -> socket(sn, Sn_MR_TCP, HTTP_PORT, 0)
 *   SOCK_INIT         -> listen(sn)
 *   SOCK_LISTEN       -> nothing, wait for a client
 *   SOCK_ESTABLISHED  -> if getSn_RX_RSR(sn) > 0: recv the request, answer, disconnect(sn)
 *   SOCK_CLOSE_WAIT   -> disconnect(sn)
 *
 * Coupling with net.h:
 *   - While !NET_IsUp(), keep all HTTP sockets closed.
 *   - When NET_GetInfo()->config_version changes (new DHCP IP or switch to
 *     static), close all HTTP sockets so they reopen with the new config.
 */

#include <stdint.h>

#define HTTP_PORT          80u
#define HTTP_SOCKET_FIRST  0u     // First W5500 socket used for HTTP
#define HTTP_SOCKET_COUNT  4u     // Sockets 0..3; socket 7 belongs to DHCP (NET_DHCP_SOCKET)
#define HTTP_REQ_BUF_SIZE  512u   // Only the request line is needed; the rest of the request is discarded
#define HTTP_TX_CHUNK_SIZE 1024u  // Large bodies (the HTML page) are sent in chunks of at most this size

#define HTTP_IDLE_TIMEOUT_MS  2000u  // Connected client that sends no request is dropped after this
#define HTTP_CLOSE_TIMEOUT_MS 2000u  // Socket stuck in a closing state (peer gone) is force-closed after this

/**
 * Reset the server state. Call once at start-up, after NET_Init().
 * Does not open sockets; HTTP_Process() does that once the network is up.
 */
void HTTP_Init(void);

/**
 * Run the per-socket state machines once. Call on every main-loop pass.
 * Must not block: only handles data that has already arrived.
 *
 * Request handling (in SOCK_ESTABLISHED):
 *   1. recv() up to HTTP_REQ_BUF_SIZE bytes; parse "METHOD PATH HTTP/x.y".
 *      Ignore a query string ("/status?t=123" is /status).
 *   2. Route to the page, /status or 404/405.
 *   3. send() headers, then the body. send() can return less than requested
 *      when the TX buffer is full; loop until everything is sent.
 *   4. disconnect().
 *   5. Count the request (HTTP_GetRequestCount()).
 *
 * @param now  HAL_GetTick() of the current main-loop pass (for /status ages and uptime).
 */
void HTTP_Process(uint32_t now);

/**
 * @return seconds since the HTTP sockets were last opened on the current IP
 *         config, or 0 while the network is down. For the NET CLI command.
 */
uint32_t HTTP_GetUptimeS(uint32_t now);

/**
 * @return number of requests answered since start-up (any status code).
 */
uint32_t HTTP_GetRequestCount(void);

#endif
