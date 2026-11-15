#ifndef MDNS_H
#define MDNS_H

#include <stdint.h>

#define MDNS_SOCKET       6u
#define MDNS_PORT         5353u
#define MDNS_HOSTNAME     "bp-411"
#define MDNS_FULL_NAME    "bp-411.local"
#define MDNS_MULTICAST_IP {224, 0, 0, 251}

/**
 * Reset the mDNS responder state. Call once after NET_Init().
 */
void MDNS_Init(void);

/**
 * Poll the UDP mDNS socket. Call on every main-loop pass after NET_Process().
 * Answers IPv4 A/ANY queries for MDNS_FULL_NAME and announces the current IP
 * after the network comes up or changes.
 */
void MDNS_Process(uint32_t now);

#endif
