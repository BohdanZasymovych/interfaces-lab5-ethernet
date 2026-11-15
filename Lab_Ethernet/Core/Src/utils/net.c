#include "utils/net.h"

#include "dhcp.h"
#include "wizchip_conf.h"
#include <string.h>

#define NET_UID_BASE_ADDR 0x1FFF7A10u  // 96-bit unique device ID of the STM32F4
#define NET_UID_SIZE      12u

static net_info_t net_info = {.state = NET_ST_NO_CHIP, .mode = NET_MODE_NONE};

static uint8_t net_dhcp_buf[NET_DHCP_BUF_SIZE];  // DHCP message buffer, used by the ioLibrary between calls
static uint32_t net_dhcp_start_ms;               // HAL_GetTick() when the current DHCP attempt started

static const uint8_t net_static_ip[4] = NET_STATIC_IP;
static const uint8_t net_static_mask[4] = NET_STATIC_MASK;
static const uint8_t net_static_gw[4] = NET_STATIC_GW;

/*
 * MAC from the chip unique ID: first byte 0x02 (locally administered, unicast),
 * the rest from an FNV-1a hash of the 12 UID bytes. Stable for a given board.
 */
static void net_make_mac(uint8_t mac[6]) {
    const uint8_t* uid = (const uint8_t*) NET_UID_BASE_ADDR;
    uint32_t hash = 2166136261u;
    uint8_t xor_all = 0;

    for (uint32_t i = 0; i < NET_UID_SIZE; i++) {
        hash = (hash ^ uid[i]) * 16777619u;
        xor_all ^= uid[i];
    }

    mac[0] = 0x02;
    mac[1] = xor_all;
    mac[2] = (uint8_t) (hash >> 24);
    mac[3] = (uint8_t) (hash >> 16);
    mac[4] = (uint8_t) (hash >> 8);
    mac[5] = (uint8_t) hash;
}

static bool net_link_up(void) {
    uint8_t link = PHY_LINK_OFF;
    if (ctlwizchip(CW_GET_PHYLINK, &link) != 0) return false;
    return link == PHY_LINK_ON;
}

// Write the config to the W5500 and publish it in net_info
static void net_apply(const uint8_t ip[4], const uint8_t mask[4], const uint8_t gw[4], net_mode_t mode, uint32_t now) {
    wiz_NetInfo ni = {.dhcp = (mode == NET_MODE_DHCP) ? NETINFO_DHCP : NETINFO_STATIC};
    memcpy(ni.mac, net_info.mac, sizeof(ni.mac));
    memcpy(ni.ip, ip, sizeof(ni.ip));
    memcpy(ni.sn, mask, sizeof(ni.sn));
    memcpy(ni.gw, gw, sizeof(ni.gw));
    wizchip_setnetinfo(&ni);

    memcpy(net_info.ip, ip, sizeof(net_info.ip));
    memcpy(net_info.mask, mask, sizeof(net_info.mask));
    memcpy(net_info.gw, gw, sizeof(net_info.gw));
    net_info.lease_s = (mode == NET_MODE_DHCP) ? getDHCPLeasetime() : 0;
    net_info.mode = mode;
    net_info.state = NET_ST_UP;
    net_info.up_since_ms = now;
    net_info.config_version++;
}

static void net_apply_dhcp_lease(uint32_t now) {
    uint8_t ip[4], mask[4], gw[4];
    getIPfromDHCP(ip);
    getSNfromDHCP(mask);
    getGWfromDHCP(gw);
    net_apply(ip, mask, gw, NET_MODE_DHCP, now);
}

static void net_apply_static(uint32_t now) {
    DHCP_stop();
    net_apply(net_static_ip, net_static_mask, net_static_gw, NET_MODE_STATIC, now);
}

// Remove the IP config from the chip and from net_info (MAC stays)
static void net_clear_ip(void) {
    static const uint8_t zero[4] = {0, 0, 0, 0};
    setSIPR((uint8_t*) zero);
    setSUBR((uint8_t*) zero);
    setGAR((uint8_t*) zero);

    memset(net_info.ip, 0, sizeof(net_info.ip));
    memset(net_info.mask, 0, sizeof(net_info.mask));
    memset(net_info.gw, 0, sizeof(net_info.gw));
    net_info.lease_s = 0;
    net_info.mode = NET_MODE_NONE;
    net_info.up_since_ms = 0;
}

static void net_start_dhcp(uint32_t now) {
    DHCP_init(NET_DHCP_SOCKET, net_dhcp_buf);  // also zeroes SIPR and GAR in the chip
    net_dhcp_start_ms = now;
    net_info.state = NET_ST_DHCP;
}

/*
 * DHCP callbacks. The ioLibrary defaults write the IP into the chip on their own,
 * and the update/conflict ones even soft-reset the W5500 (losing MAC and buffer
 * setup). Replace them with no-ops: NET_Process() applies the config itself from
 * the DHCP_run() return value. On a conflict the library declines the address and
 * starts discovery again by itself.
 */
static void net_dhcp_on_assign(void) {
}
static void net_dhcp_on_update(void) {
}
static void net_dhcp_on_conflict(void) {
}

void NET_Init(bool chip_ok) {
    memset(&net_info, 0, sizeof(net_info));
    net_info.state = NET_ST_NO_CHIP;
    net_info.mode = NET_MODE_NONE;
    if (!chip_ok) return;

    uint8_t txsize[_WIZCHIP_SOCK_NUM_] = {2, 2, 2, 2, 2, 2, 2, 2};  // KB per socket, 16 KB total
    uint8_t rxsize[_WIZCHIP_SOCK_NUM_] = {2, 2, 2, 2, 2, 2, 2, 2};
    if (wizchip_init(txsize, rxsize) != 0) return;

    net_make_mac(net_info.mac);
    setSHAR(net_info.mac);

    reg_dhcp_cbfunc(net_dhcp_on_assign, net_dhcp_on_update, net_dhcp_on_conflict);

    net_info.state = NET_ST_NO_LINK;
}

void NET_Process(uint32_t now) {
    if (net_info.state == NET_ST_NO_CHIP) return;

    net_info.link_up = net_link_up();

    // Link lost in any state: drop the config, start over when the cable is back
    if (!net_info.link_up && net_info.state != NET_ST_NO_LINK) {
        if (net_info.state == NET_ST_DHCP || net_info.mode == NET_MODE_DHCP) DHCP_stop();
        net_clear_ip();
        net_info.state = NET_ST_NO_LINK;
        return;
    }

    switch (net_info.state) {
        case NET_ST_NO_LINK:
            if (net_info.link_up) net_start_dhcp(now);
            break;

        case NET_ST_DHCP:
            switch (DHCP_run()) {
                case DHCP_IP_LEASED:
                case DHCP_IP_ASSIGN:
                case DHCP_IP_CHANGED: net_apply_dhcp_lease(now); break;
                case DHCP_FAILED: net_apply_static(now); break;
                default:
                    if (now - net_dhcp_start_ms >= NET_DHCP_TIMEOUT_MS) net_apply_static(now);
                    break;
            }
            break;

        case NET_ST_UP:
            if (net_info.mode != NET_MODE_DHCP) break;  // static: nothing to maintain

            switch (DHCP_run()) {  // lease renewal happens inside
                case DHCP_IP_CHANGED: net_apply_dhcp_lease(now); break;
                case DHCP_FAILED:  // renewal failed: server gone, run the whole DHCP -> static cycle again
                    DHCP_stop();
                    net_clear_ip();
                    net_start_dhcp(now);
                    break;
                default: break;
            }
            break;

        case NET_ST_NO_CHIP:
        default: break;
    }
}

void NET_Tick1s(void) {
    if (net_info.state == NET_ST_DHCP || net_info.mode == NET_MODE_DHCP) DHCP_time_handler();
}

bool NET_IsUp(void) {
    return net_info.state == NET_ST_UP;
}

const net_info_t* NET_GetInfo(void) {
    return &net_info;
}

const char* NET_StateName(net_state_t state) {
    switch (state) {
        case NET_ST_NO_CHIP: return "NO W5500";
        case NET_ST_NO_LINK: return "NO LINK";
        case NET_ST_DHCP: return "DHCP...";
        case NET_ST_UP: return "UP";
        default: return "?";
    }
}

const char* NET_ModeName(net_mode_t mode) {
    switch (mode) {
        case NET_MODE_NONE: return "NONE";
        case NET_MODE_DHCP: return "DHCP";
        case NET_MODE_STATIC: return "STATIC";
        default: return "?";
    }
}
