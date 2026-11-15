#include "utils/mdns.h"

#include "socket.h"
#include "utils/net.h"
#include <stdbool.h>
#include <string.h>

#define MDNS_RX_SIZE             256u
#define MDNS_TX_SIZE             128u
#define MDNS_ANNOUNCE_COUNT      3u
#define MDNS_ANNOUNCE_PERIOD_MS  1000u
#define MDNS_TTL_S               120u

_Static_assert(MDNS_SOCKET < NET_DHCP_SOCKET, "mDNS socket overlaps DHCP socket");

static uint8_t mdns_rx[MDNS_RX_SIZE];
static uint8_t mdns_tx[MDNS_TX_SIZE];
static uint32_t mdns_config_version;
static uint32_t mdns_next_announce_ms;
static uint8_t mdns_announcements_left;

static const uint8_t mdns_multicast_ip[4] = MDNS_MULTICAST_IP;

static bool mdns_time_reached(uint32_t now, uint32_t deadline) {
    return (int32_t) (now - deadline) >= 0;
}

static void mdns_reset_announce(uint32_t now) {
    mdns_announcements_left = MDNS_ANNOUNCE_COUNT;
    mdns_next_announce_ms = now;
}

static void mdns_close_socket(void) {
    if (getSn_SR(MDNS_SOCKET) != SOCK_CLOSED) close(MDNS_SOCKET);
}

static bool mdns_write_name(uint8_t* dst, uint16_t capacity, uint16_t* pos) {
    static const uint8_t name[] = {
            6, 'b', 'p', '-', '4', '1', '1',
            5, 'l', 'o', 'c', 'a', 'l',
            0,
    };

    if ((uint16_t) (*pos + sizeof(name)) > capacity) return false;
    memcpy(dst + *pos, name, sizeof(name));
    *pos += sizeof(name);
    return true;
}

static bool mdns_label_equals(const uint8_t* packet, uint16_t packet_len, uint16_t* pos, const char* label) {
    uint8_t label_len;
    size_t expected_len = strlen(label);

    if (*pos >= packet_len) return false;
    label_len = packet[(*pos)++];
    if (label_len != expected_len || (uint16_t) (*pos + label_len) > packet_len) return false;

    for (uint8_t i = 0; i < label_len; i++) {
        uint8_t actual = packet[*pos + i];
        uint8_t expected = (uint8_t) label[i];
        if (actual >= 'A' && actual <= 'Z') actual = (uint8_t) (actual + ('a' - 'A'));
        if (actual != expected) return false;
    }

    *pos += label_len;
    return true;
}

static bool mdns_parse_question(const uint8_t* packet, uint16_t packet_len, uint16_t* question_end) {
    uint16_t pos = 12;

    if (packet_len < 12 || packet[4] == 0 || packet[5] != 1) return false;
    if (!mdns_label_equals(packet, packet_len, &pos, "bp-411")) return false;
    if (!mdns_label_equals(packet, packet_len, &pos, "local")) return false;
    if (pos >= packet_len || packet[pos++] != 0) return false;
    if ((uint16_t) (pos + 4) > packet_len) return false;

    uint16_t qtype = ((uint16_t) packet[pos] << 8) | packet[pos + 1];
    uint16_t qclass = ((uint16_t) packet[pos + 2] << 8) | packet[pos + 3];
    if (qtype != 1 && qtype != 255) return false;  // A or ANY
    if ((qclass & 0x7FFFu) != 1) return false;     // IN, with QU bit ignored

    *question_end = (uint16_t) (pos + 4);
    return true;
}

static uint16_t mdns_build_response(uint8_t* response, uint16_t capacity, const uint8_t ip[4]) {
    uint16_t pos = 0;

    if (capacity < 12) return 0;
    response[pos++] = 0;
    response[pos++] = 0;
    response[pos++] = 0x84;  // response, authoritative answer
    response[pos++] = 0x00;
    response[pos++] = 0;
    response[pos++] = 0;     // no question section in the response
    response[pos++] = 0;
    response[pos++] = 1;     // one answer
    response[pos++] = 0;
    response[pos++] = 0;
    response[pos++] = 0;
    response[pos++] = 0;

    if (!mdns_write_name(response, capacity, &pos)) return 0;
    if ((uint16_t) (pos + 14) > capacity) return 0;

    response[pos++] = 0;
    response[pos++] = 1;     // A
    response[pos++] = 0x80;  // cache flush + IN
    response[pos++] = 1;
    response[pos++] = (uint8_t) (MDNS_TTL_S >> 24);
    response[pos++] = (uint8_t) (MDNS_TTL_S >> 16);
    response[pos++] = (uint8_t) (MDNS_TTL_S >> 8);
    response[pos++] = (uint8_t) MDNS_TTL_S;
    response[pos++] = 0;
    response[pos++] = 4;
    memcpy(response + pos, ip, 4);
    pos += 4;
    return pos;
}

static void mdns_send_announcement(void) {
    const net_info_t* net = NET_GetInfo();
    uint16_t length = mdns_build_response(mdns_tx, sizeof(mdns_tx), net->ip);
    if (length != 0) sendto(MDNS_SOCKET, mdns_tx, length, (uint8_t*) mdns_multicast_ip, MDNS_PORT);
}

void MDNS_Init(void) {
    memset(mdns_rx, 0, sizeof(mdns_rx));
    memset(mdns_tx, 0, sizeof(mdns_tx));
    mdns_config_version = 0;
    mdns_next_announce_ms = 0;
    mdns_announcements_left = 0;
}

void MDNS_Process(uint32_t now) {
    if (!NET_IsUp()) {
        if (getSn_SR(MDNS_SOCKET) != SOCK_CLOSED) mdns_close_socket();
        mdns_config_version = 0;
        mdns_announcements_left = 0;
        return;
    }

    const net_info_t* net = NET_GetInfo();
    if (net->config_version != mdns_config_version) {
        mdns_close_socket();
        mdns_config_version = net->config_version;
        mdns_reset_announce(now);
    }

    if (getSn_SR(MDNS_SOCKET) == SOCK_CLOSED) {
        socket(MDNS_SOCKET, Sn_MR_UDP, MDNS_PORT, SF_IO_NONBLOCK);
        return;
    }

    if (getSn_SR(MDNS_SOCKET) != SOCK_UDP) return;

    if (mdns_announcements_left != 0 && mdns_time_reached(now, mdns_next_announce_ms)) {
        mdns_send_announcement();
        mdns_announcements_left--;
        mdns_next_announce_ms = now + MDNS_ANNOUNCE_PERIOD_MS;
    }

    if (getSn_RX_RSR(MDNS_SOCKET) != 0) {
        uint8_t source_ip[4];
        uint16_t source_port = 0;
        int32_t received = recvfrom(MDNS_SOCKET, mdns_rx, sizeof(mdns_rx), source_ip, &source_port);
        if (received > 0) {
            uint16_t question_end;
            if (mdns_parse_question(mdns_rx, (uint16_t) received, &question_end)) {
                (void) question_end;
                uint16_t response_length = mdns_build_response(mdns_tx, sizeof(mdns_tx), net->ip);
                if (response_length != 0) {
                    sendto(MDNS_SOCKET, mdns_tx, response_length, (uint8_t*) mdns_multicast_ip, MDNS_PORT);
                }
            }
        }
    }
}
