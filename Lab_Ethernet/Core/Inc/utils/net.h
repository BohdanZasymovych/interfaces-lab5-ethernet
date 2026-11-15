#ifndef NET_H
#define NET_H

/*
 * Network configuration: MAC, link, DHCP with fallback to a static IP.
 *
 * A non-blocking state machine driven from the main loop:
 *
 *   NET_ST_NO_CHIP   W5500 did not answer, nothing else runs
 *   NET_ST_NO_LINK   waiting for the Ethernet cable / PHY link
 *   NET_ST_DHCP      DHCP running, gives up after NET_DHCP_TIMEOUT_MS
 *   NET_ST_UP        IP applied (mode says DHCP or STATIC)
 *
 *   power-up -> NO_LINK -> (link up) -> DHCP -> lease   -> UP, mode DHCP
 *                                           -> timeout -> UP, mode STATIC
 *   UP -> (link lost) -> NO_LINK, then DHCP again on re-plug
 *
 * Notes:
 *   - The MAC is written before DHCP starts: the DHCP client sends it.
 *   - DHCP_init() of the ioLibrary zeroes the IP and gateway registers, so
 *     DHCP is never retried in the background while the static IP is in use.
 *     It runs once per link-up.
 *   - This module knows nothing about HTTP. Other modules watch
 *     net_info_t.config_version to notice that the IP config changed.
 *
 * Socket use: NET_DHCP_SOCKET is reserved for DHCP; HTTP uses the others.
 */

#include <stdbool.h>
#include <stdint.h>

#define NET_DHCP_SOCKET     7u      // W5500 socket used by the DHCP client
#define NET_DHCP_TIMEOUT_MS 10000u  // Time to wait for a DHCP lease before using the static config
#define NET_DHCP_BUF_SIZE   548u    // DHCP message buffer (RIP_MSG_SIZE in the ioLibrary's dhcp.h)

// Static fallback, used when no DHCP server answers
#define NET_STATIC_IP   {192, 168, 4, 50}
#define NET_STATIC_MASK {255, 255, 255, 0}
#define NET_STATIC_GW   {192, 168, 4, 1}

typedef enum {
    NET_ST_NO_CHIP,  // W5500_Port_Init() failed
    NET_ST_NO_LINK,  // chip ready, no Ethernet link
    NET_ST_DHCP,     // link up, waiting for a DHCP lease
    NET_ST_UP,       // IP configured, sockets can be used
} net_state_t;

typedef enum {
    NET_MODE_NONE,    // no IP applied yet
    NET_MODE_DHCP,    // IP from a DHCP lease
    NET_MODE_STATIC,  // static fallback (NET_STATIC_*)
} net_mode_t;

typedef struct {
    net_state_t state;
    net_mode_t mode;
    bool link_up;    // PHY link, from ctlwizchip(CW_GET_PHYLINK, ...)
    uint8_t mac[6];  // derived from the STM32 unique ID, see NET_Init()
    uint8_t ip[4];   // all zero while not NET_ST_UP
    uint8_t mask[4];
    uint8_t gw[4];
    uint32_t lease_s;         // DHCP lease time in seconds, 0 for static
    uint32_t up_since_ms;     // HAL_GetTick() when the current IP config was applied
    uint32_t config_version;  // incremented every time an IP config is applied
} net_info_t;

/**
 * Set up the network part of the W5500. Call once, after W5500_Port_Init().
 *
 * Steps:
 *   1. If W5500_Port_Init() failed -> state NET_ST_NO_CHIP and return.
 *   2. wizchip_init(): split the 16 KB TX / 16 KB RX buffers between the
 *      8 sockets (2 KB each is fine).
 *   3. Build the MAC from the 96-bit unique ID at 0x1FFF7A10:
 *        mac[0] = 0x02 (locally administered, unicast), mac[1..5] from a
 *        hash of the UID. The same board always gets the same MAC.
 *   4. setSHAR(mac).
 *   5. state = NET_ST_NO_LINK.
 *
 * @param chip_ok  Result of W5500_Port_Init().
 */
void NET_Init(bool chip_ok);

/**
 * Advance the state machine. Call on every main-loop pass. Never blocks.
 *
 *   NO_LINK: read the PHY link; on link up -> DHCP_init(NET_DHCP_SOCKET, buf),
 *            remember the start time, state = NET_ST_DHCP.
 *   DHCP:    DHCP_run();
 *            DHCP_IP_LEASED -> read IP/mask/GW from DHCP, apply, mode DHCP;
 *            DHCP_FAILED or NET_DHCP_TIMEOUT_MS passed -> DHCP_stop(),
 *            apply NET_STATIC_*, mode STATIC.
 *   UP:      in DHCP mode keep calling DHCP_run() (lease renewal, handle
 *            DHCP_IP_CHANGED); if the link drops -> DHCP_stop() if needed,
 *            clear the IP, state = NET_ST_NO_LINK.
 *
 * "Apply" = wizchip_setnetinfo(), fill net_info_t, set up_since_ms and
 * increment config_version.
 *
 * @param now  HAL_GetTick() of the current main-loop pass.
 */
void NET_Process(uint32_t now);

/**
 * Call once per second (from the 1 s block in the main loop).
 * Forwards to DHCP_time_handler(), which the ioLibrary DHCP client uses for
 * its own timeouts and for lease renewal.
 */
void NET_Tick1s(void);

/**
 * @return true when an IP config is applied (NET_ST_UP) and sockets can be opened.
 */
bool NET_IsUp(void);

/**
 * Current network state for the display, the NET CLI command and the
 * optional "net" object in /status. Read-only, always valid.
 */
const net_info_t* NET_GetInfo(void);

/**
 * Human-readable names for the display and the CLI.
 *
 * @return e.g. "NO W5500", "NO LINK", "DHCP...", "UP" / "NONE", "DHCP", "STATIC".
 */
const char* NET_StateName(net_state_t state);
const char* NET_ModeName(net_mode_t mode);

#endif
