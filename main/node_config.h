/*
 * node_config.h — Stage 3 node-role and static forwarding configuration.
 *
 * ONE codebase, THREE firmware builds.
 * Enable exactly ONE of the NODE_ROLE_* defines below.
 * Zero or multiple enabled = compile error.
 *
 * USB1 = NODE_A  MAC = 28:05:a5:e2:b0:f4
 * USB2 = NODE_B  MAC = e0:8c:fe:f9:b8:c0
 * USB0 = NODE_C  MAC = 28:05:a5:e2:18:d0
 */
#ifndef NODE_CONFIG_H
#define NODE_CONFIG_H

#include <stdint.h>
#include "esp_now.h"

/* ================================================================
 * NODE ROLE SELECTION — enable exactly ONE
 * ================================================================ */
// #define NODE_ROLE_A   1
 #define NODE_ROLE_B   1
// #define NODE_ROLE_C   1

/* ================================================================
 * Compile-time validation: exactly one role must be selected
 * ================================================================ */
#ifndef NODE_ROLE_A
#define NODE_ROLE_A 0
#endif
#ifndef NODE_ROLE_B
#define NODE_ROLE_B 0
#endif
#ifndef NODE_ROLE_C
#define NODE_ROLE_C 0
#endif

#define _NODE_ROLE_COUNT (NODE_ROLE_A + NODE_ROLE_B + NODE_ROLE_C)

#if _NODE_ROLE_COUNT == 0
#error "No node role selected. Enable exactly one of NODE_ROLE_A, NODE_ROLE_B, NODE_ROLE_C."
#endif
#if _NODE_ROLE_COUNT > 1
#error "Multiple node roles selected. Enable exactly one of NODE_ROLE_A, NODE_ROLE_B, NODE_ROLE_C."
#endif

/* ================================================================
 * MAC addresses — authoritative hardware mapping
 * ================================================================ */
#define MAC_NODE_A  { 0x28, 0x05, 0xa5, 0xe2, 0xb0, 0xf4 }
#define MAC_NODE_B  { 0xe0, 0x8c, 0xfe, 0xf9, 0xb8, 0xc0 }
#define MAC_NODE_C  { 0x28, 0x05, 0xa5, 0xe2, 0x18, 0xd0 }

/* ================================================================
 * Role name string and expected MAC for the selected role
 * ================================================================ */
#if NODE_ROLE_A
#define NODE_ROLE_NAME  "NODE_A"
#define NODE_EXPECTED_MAC  MAC_NODE_A
#elif NODE_ROLE_B
#define NODE_ROLE_NAME  "NODE_B"
#define NODE_EXPECTED_MAC  MAC_NODE_B
#elif NODE_ROLE_C
#define NODE_ROLE_NAME  "NODE_C"
#define NODE_EXPECTED_MAC  MAC_NODE_C
#endif

/* ================================================================
 * Stage 3 static forwarding configuration
 *
 * NODE_A: destination=C, next_hop=B
 * NODE_B: destination=C, next_hop=C
 * NODE_C: destination=C, local delivery (no next hop needed)
 * ================================================================ */
#define FORWARD_DESTINATION  MAC_NODE_C

#if NODE_ROLE_A
#define FORWARD_NEXT_HOP     MAC_NODE_B
#elif NODE_ROLE_B
#define FORWARD_NEXT_HOP     MAC_NODE_C
#endif
/* NODE_C delivers locally — no FORWARD_NEXT_HOP defined */

/* Maximum hop count (loop protection, NOT routing) */
#define ESPNOW_MAX_HOPS  8

/* DATA generator interval (NODE_A only) */
#define DATA_GENERATE_INTERVAL_MS  2000

#endif /* NODE_CONFIG_H */
