#include "metrics.h"
#include "node_config.h"
#include "neighbor_manager.h"
#include "espnow_manager.h"
#include "packets.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#define MAX_METRIC_ENTRIES MAX_NEIGHBORS
#define MAX_PENDING_PROBES 32

static link_metric_t g_metrics[MAX_METRIC_ENTRIES];
static pending_probe_t g_pending_probes[MAX_PENDING_PROBES];
static uint32_t g_metric_sequence = 0;

void metrics_init(void) {
    memset(g_metrics, 0, sizeof(g_metrics));
    memset(g_pending_probes, 0, sizeof(g_pending_probes));
}

static link_metric_t *find_metric(const uint8_t *mac) {
    for (int i = 0; i < MAX_METRIC_ENTRIES; i++) {
        if (g_metrics[i].mac[0] != 0 && memcmp(g_metrics[i].mac, mac, ESP_NOW_ETH_ALEN) == 0) {
            return &g_metrics[i];
        }
    }
    return NULL;
}

static void track_probe_tx(uint32_t seq, const uint8_t *dest_mac, int64_t now_us) {
    for (int i = 0; i < MAX_PENDING_PROBES; i++) {
        if (!g_pending_probes[i].active) {
            g_pending_probes[i].sequence = seq;
            g_pending_probes[i].tx_time_us = now_us;
            memcpy(g_pending_probes[i].dest_mac, dest_mac, ESP_NOW_ETH_ALEN);
            g_pending_probes[i].active = true;
            return;
        }
    }
    // Buffer full: overwrite oldest (index 0 shift)
    g_pending_probes[0].sequence = seq;
    g_pending_probes[0].tx_time_us = now_us;
    memcpy(g_pending_probes[0].dest_mac, dest_mac, ESP_NOW_ETH_ALEN);
    g_pending_probes[0].active = true;
}

static int64_t pop_probe_rtt(uint32_t seq, const uint8_t *src_mac, int64_t now_us) {
    for (int i = 0; i < MAX_PENDING_PROBES; i++) {
        if (g_pending_probes[i].active &&
            g_pending_probes[i].sequence == seq &&
            memcmp(g_pending_probes[i].dest_mac, src_mac, ESP_NOW_ETH_ALEN) == 0) {
            
            g_pending_probes[i].active = false;
            int64_t rtt = now_us - g_pending_probes[i].tx_time_us;
            return rtt > 0 ? rtt : 0;
        }
    }
    return -1; // Not found or already processed
}

static void send_metric_probe(const uint8_t *dest_mac) {
    espnow_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = ESPNOW_PKT_METRIC_PROBE;
    pkt.header.version = ESPNOW_PROTOCOL_VERSION;
    pkt.header.length = sizeof(espnow_header_t) + sizeof(metric_probe_payload_t);
    
    espnow_manager_get_local_mac(pkt.u.metric_probe.source_mac);
    memcpy(pkt.u.metric_probe.destination_mac, dest_mac, ESP_NOW_ETH_ALEN);
    pkt.u.metric_probe.sequence = ++g_metric_sequence;
    
    int64_t now_us = esp_timer_get_time();
    track_probe_tx(pkt.u.metric_probe.sequence, dest_mac, now_us);

    espnow_send_tag_t tag = { .pkt_type = ESPNOW_PKT_METRIC_PROBE, .sequence = pkt.u.metric_probe.sequence };
    memcpy(tag.dest_mac, dest_mac, ESP_NOW_ETH_ALEN);
    espnow_manager_send(dest_mac, &pkt, &tag);
    // Suppress routine debug log: send_metric_probe
}

void metrics_update(void) {
    int64_t now = esp_timer_get_time();

    // 1. Maintain synchronization
    for (int i = 0; i < MAX_METRIC_ENTRIES; i++) {
        if (g_metrics[i].mac[0] != 0 && !neighbor_is_accepted(g_metrics[i].mac)) {
            memset(&g_metrics[i], 0, sizeof(link_metric_t));
        }
    }

    // 2. Add entries for accepted neighbors
    uint8_t macs[MAX_NEIGHBORS][ESP_NOW_ETH_ALEN];
    int n = neighbor_get_accepted_macs(macs, MAX_NEIGHBORS);
    for (int i = 0; i < n; i++) {
        if (!find_metric(macs[i])) {
            for (int k = 0; k < MAX_METRIC_ENTRIES; k++) {
                if (g_metrics[k].mac[0] == 0) {
                    memcpy(g_metrics[k].mac, macs[i], ESP_NOW_ETH_ALEN);
                    g_metrics[k].last_window_start_us = now;
                    // Entry initialized for accepted neighbor
                    break;
                }
            }
        }
    }

    // 3. Sliding measurement window calculation & periodic probing
    for (int i = 0; i < MAX_METRIC_ENTRIES; i++) {
        if (g_metrics[i].mac[0] != 0) {
            if (now - g_metrics[i].last_window_start_us >= (METRICS_WINDOW_MS * 1000LL)) {
                if (g_metrics[i].window_probe_tx > 0) {
                    g_metrics[i].pdr = (float)g_metrics[i].window_probe_ack / (float)g_metrics[i].window_probe_tx;
                    g_metrics[i].has_valid_pdr = true;
                }
                if (g_metrics[i].window_rtt_count > 0) {
                    g_metrics[i].rtt_ms = ((double)g_metrics[i].window_rtt_sum_us / (double)g_metrics[i].window_rtt_count) / 1000.0;
                    g_metrics[i].has_valid_rtt = true;
                }
                
                if (neighbor_is_accepted(g_metrics[i].mac) && g_metrics[i].has_valid_pdr && g_metrics[i].has_valid_rtt) {
                    neighbor_t *nb = neighbor_find(g_metrics[i].mac);
                    int8_t rssi = nb ? nb->rssi : g_metrics[i].last_rssi;
                    int64_t timestamp_ms = now / 1000LL;
                    printf("DATA,%" PRId64 "," MACSTR ",%d,%.3f,%d\n",
                           timestamp_ms,
                           MAC2STR(g_metrics[i].mac),
                           rssi,
                           g_metrics[i].pdr,
                           (int)round(g_metrics[i].rtt_ms));
                }
                
                // Reset window counters
                g_metrics[i].window_probe_tx = 0;
                g_metrics[i].window_probe_ack = 0;
                g_metrics[i].window_rtt_sum_us = 0;
                g_metrics[i].window_rtt_count = 0;
                g_metrics[i].last_window_start_us = now;
            }
        }
    }
    
    static int64_t last_probe_time = 0;
    if (now - last_probe_time >= (METRIC_PROBE_INTERVAL_MS * 1000LL)) {
        last_probe_time = now;
        for (int i = 0; i < MAX_METRIC_ENTRIES; i++) {
            if (g_metrics[i].mac[0] != 0 && neighbor_is_accepted(g_metrics[i].mac)) {
                send_metric_probe(g_metrics[i].mac);
                g_metrics[i].window_probe_tx++;
            }
        }
    }
}

void handle_metric_probe_packet(const espnow_evt_recv_t *evt) {
    const metric_probe_payload_t *p = &evt->packet.u.metric_probe;
    if (!neighbor_is_accepted(p->source_mac)) return;
    
    link_metric_t *m = find_metric(p->source_mac);
    if (m) {
        m->last_rssi = evt->rssi;
    }
    
    espnow_packet_t ack;
    memset(&ack, 0, sizeof(ack));
    ack.header.type = ESPNOW_PKT_METRIC_ACK;
    ack.header.version = ESPNOW_PROTOCOL_VERSION;
    ack.header.length = sizeof(espnow_header_t) + sizeof(metric_ack_payload_t);
    
    espnow_manager_get_local_mac(ack.u.metric_ack.source_mac);
    memcpy(ack.u.metric_ack.destination_mac, p->source_mac, ESP_NOW_ETH_ALEN);
    ack.u.metric_ack.metric_sequence = p->sequence;
    
    espnow_send_tag_t tag = { .pkt_type = ESPNOW_PKT_METRIC_ACK, .sequence = p->sequence };
    memcpy(tag.dest_mac, p->source_mac, ESP_NOW_ETH_ALEN);
    espnow_manager_send(p->source_mac, &ack, &tag);
}

void handle_metric_ack_packet(const espnow_evt_recv_t *evt) {
    const metric_ack_payload_t *p = &evt->packet.u.metric_ack;
    if (!neighbor_is_accepted(p->source_mac)) return;
    
    int64_t now_us = esp_timer_get_time();
    int64_t rtt_us = pop_probe_rtt(p->metric_sequence, p->source_mac, now_us);
    
    link_metric_t *m = find_metric(p->source_mac);
    if (m) {
        m->last_rssi = evt->rssi;
        m->window_probe_ack++;
        
        if (rtt_us >= 0) {
            m->window_rtt_sum_us += rtt_us;
            m->window_rtt_count++;
            
            // RTT window accumulation
        }
    }
}

void metrics_print_table(void) {
    // Routine debug table suppressed
}
