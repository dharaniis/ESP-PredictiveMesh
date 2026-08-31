#ifndef TOPOLOGY_H
#define TOPOLOGY_H

#include "neighbor_manager.h"
#include "espnow_manager.h"
#include <stdbool.h>
#include <stdint.h>

#define TOPOLOGY_ADV_INTERVAL_MS 2000
#define TOPOLOGY_EXPIRE_MS       8000

typedef struct {
    uint8_t destination[ESP_NOW_ETH_ALEN];
    uint8_t next_hop[ESP_NOW_ETH_ALEN];
    uint8_t hop_count;
    bool    valid;
} route_entry_t;

void topology_init(void);
void topology_start(void);
void topology_handle_packet(const espnow_evt_recv_t *evt);
void topology_recalculate_routes(void);
bool routing_lookup(const uint8_t *dest_mac, uint8_t *out_next_hop, uint8_t *out_hop_count);
void routing_print_table(void);

#endif // TOPOLOGY_H
