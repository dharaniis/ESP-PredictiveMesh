/*
 * Wire format for all ESP-NOW application packets.
 * Every packet starts with espnow_header_t. The header.type field routes
 * the packet to the right handler in the dispatcher.
 */
#ifndef ESPNOW_PACKETS_H
#define ESPNOW_PACKETS_H

#include <stdint.h>
#include "esp_now.h"

#define ESPNOW_PROTOCOL_VERSION 1

typedef enum {
    ESPNOW_PKT_DATA      = 1,
    ESPNOW_PKT_HELLO     = 2,
    ESPNOW_PKT_HELLO_ACK = 3,
} espnow_packet_type_t;

typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint8_t  version;
    uint16_t length;     // total packet length including this header
} espnow_header_t;

_Static_assert(sizeof(espnow_header_t) == 4, "header must be 4 bytes");

// Stage 1 DATA payload (carried after the header).
typedef struct __attribute__((packed)) {
    uint32_t sequence;
    int64_t  timestamp_us;
    char     payload[32];
} data_payload_t;

// HELLO: broadcast. source_mac is the sender's Wi-Fi STA MAC.
typedef struct __attribute__((packed)) {
    uint8_t  source_mac[ESP_NOW_ETH_ALEN];
    uint32_t sequence;
} hello_payload_t;

// HELLO_ACK: unicast back to the HELLO sender. hello_sequence is the
// sequence from the HELLO we are acknowledging.
typedef struct __attribute__((packed)) {
    uint8_t  source_mac[ESP_NOW_ETH_ALEN];
    uint8_t  destination_mac[ESP_NOW_ETH_ALEN];
    uint32_t hello_sequence;
} hello_ack_payload_t;

typedef struct __attribute__((packed)) {
    espnow_header_t header;
    union {
        data_payload_t      data;
        hello_payload_t     hello;
        hello_ack_payload_t hello_ack;
    } u;
} espnow_packet_t;

// Convenience: offset where per-type payload begins.
#define ESPNOW_PAYLOAD_OFFSET sizeof(espnow_header_t)

// Compile-time size guard: keep well under ESP-NOW v1.0 limit (250).
_Static_assert(sizeof(espnow_packet_t) < ESP_NOW_MAX_DATA_LEN,
               "espnow_packet_t exceeds ESP_NOW_MAX_DATA_LEN (250 bytes).");

#endif // ESPNOW_PACKETS_H
