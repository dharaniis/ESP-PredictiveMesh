#ifndef METRICS_H
#define METRICS_H

#include <stdint.h>
#include "esp_now.h"
#include "espnow_manager.h"
#include "packets.h"

typedef struct {
    uint32_t sequence;
    int64_t  tx_time_us;
    uint8_t  dest_mac[ESP_NOW_ETH_ALEN];
    bool     active;
} pending_probe_t;

typedef struct {
    uint8_t mac[ESP_NOW_ETH_ALEN];
    int8_t  last_rssi;
    float   pdr;
    double  rtt_ms;
    bool    has_valid_pdr;
    bool    has_valid_rtt;
    
    // Sliding window stats
    uint32_t window_probe_tx;
    uint32_t window_probe_ack;
    int64_t  window_rtt_sum_us;
    uint32_t window_rtt_count;
    int64_t  last_window_start_us;
} link_metric_t;

void metrics_init(void);
void metrics_update(void);
void handle_metric_probe_packet(const espnow_evt_recv_t *evt);
void handle_metric_ack_packet(const espnow_evt_recv_t *evt);
void metrics_print_table(void);

#endif // METRICS_H
