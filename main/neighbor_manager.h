#ifndef NEIGHBOR_MANAGER_H
#define NEIGHBOR_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_now.h"
#include "esp_timer.h"

#define MAX_NEIGHBORS 20

typedef enum {
    NEIGHBOR_EMPTY  = 0,
    NEIGHBOR_ACTIVE = 1,
    NEIGHBOR_STALE  = 2,
} neighbor_state_t;

typedef struct {
    uint8_t  mac[ESP_NOW_ETH_ALEN];
    int8_t   rssi;
    uint32_t hello_sequence;
    int64_t  last_seen_us;
    neighbor_state_t state;
} neighbor_t;

void neighbor_manager_init(void);

// Look up an entry by MAC. Returns NULL if not present.
neighbor_t *neighbor_find(const uint8_t *mac);

// Add (or update) a neighbor. Returns the (existing or new) entry, or NULL
// if the table is full and the MAC was unknown.
neighbor_t *neighbor_add_or_update(const uint8_t *mac);

// Refresh RSSI / last_seen_us / hello_sequence / state on a HELLO or
// HELLO_ACK receipt. Returns the entry, or NULL if the MAC is unknown
// (in which case the caller may call neighbor_add_or_update).
neighbor_t *neighbor_update_observation(const uint8_t *mac,
                                        int8_t rssi,
                                        uint32_t hello_sequence);

// Age entries. STALE after stale_us since last_seen, REMOVED (slot freed)
// after dead_us. Returns number of entries that transitioned to REMOVED.
int neighbor_expire(int64_t now_us, int64_t stale_us, int64_t dead_us,
                    uint8_t removed_macs[][ESP_NOW_ETH_ALEN], int max_removed);

// Number of active+stale entries currently held.
int neighbor_count(void);

// Pretty-print the table.
void neighbor_print_table(void);

#endif // NEIGHBOR_MANAGER_H
