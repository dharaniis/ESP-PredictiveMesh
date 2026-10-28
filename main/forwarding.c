/*
 * forwarding.c — Stage 4 Dynamic multi-hop DATA forwarding.
 *
 * Handles DATA packets from the dispatcher:
 *   1. Duplicate detection (source_mac + sequence)
 *   2. Destination check (local delivery vs forward)
 *   3. Hop-count enforcement
 *   4. Dynamic routing lookup via topology / BFS routing table
 *   5. Active-neighbor and RSSI-threshold validation via neighbor_manager
 *   6. Transmission via espnow_manager
 *
 * NODE_A runs a periodic DATA generator task targeting NODE_C.
 */

#include "forwarding.h"
#include "node_config.h"
#include "neighbor_manager.h"
#include "topology.h"
#include "packets.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdio.h>

#define TAG "FORWARD"

/* ================================================================
 * Duplicate detection — fixed-size circular cache
 * ================================================================ */
#define MAX_SEEN_PACKETS 32

typedef struct {
    uint8_t  source_mac[ESP_NOW_ETH_ALEN];
    uint32_t sequence;
    int64_t  timestamp_us;
} seen_entry_t;

static seen_entry_t s_seen[MAX_SEEN_PACKETS];
static int s_seen_index = 0;  /* next write position (circular) */

static bool is_duplicate(const uint8_t *src_mac, uint32_t sequence)
{
    for (int i = 0; i < MAX_SEEN_PACKETS; i++) {
        if (s_seen[i].timestamp_us != 0 &&
            s_seen[i].sequence == sequence &&
            memcmp(s_seen[i].source_mac, src_mac, ESP_NOW_ETH_ALEN) == 0) {
            return true;
        }
    }
    return false;
}

static void mark_seen(const uint8_t *src_mac, uint32_t sequence)
{
    memcpy(s_seen[s_seen_index].source_mac, src_mac, ESP_NOW_ETH_ALEN);
    s_seen[s_seen_index].sequence = sequence;
    s_seen[s_seen_index].timestamp_us = esp_timer_get_time();
    s_seen_index = (s_seen_index + 1) % MAX_SEEN_PACKETS;
}

/* ================================================================
 * Local MAC cache
 * ================================================================ */
static uint8_t s_local_mac[ESP_NOW_ETH_ALEN];
static bool s_local_mac_valid = false;

static const uint8_t *get_local_mac(void)
{
    if (!s_local_mac_valid) {
        espnow_manager_get_local_mac(s_local_mac);
        s_local_mac_valid = true;
    }
    return s_local_mac;
}

/* ================================================================
 * Forward a DATA packet to the next hop via dynamic routing table
 * ================================================================ */
static void forward_data(espnow_packet_t *pkt, const uint8_t *recv_from_mac)
{
    data_payload_t *d = &pkt->u.data;

    /* Hop-count check */
    if (d->hop_count >= ESPNOW_MAX_HOPS) {
        ESP_LOGW(TAG, "Maximum hop count exceeded (%u); dropping packet", d->hop_count);
        return;
    }

    uint8_t next_hop[ESP_NOW_ETH_ALEN];
    uint8_t route_hops = 0;

    /* Look up route in dynamic routing table */
    bool has_route = routing_lookup(d->destination_mac, next_hop, &route_hops);
    if (!has_route) return;

    /* Verify next hop is an active accepted neighbor */
    if (!neighbor_is_accepted(next_hop)) return;

    /* Ensure ESP-NOW peer is registered for next_hop */
    espnow_manager_ensure_peer(next_hop);

    uint8_t old_hop = d->hop_count;
    d->hop_count++;

    (void)recv_from_mac;
    (void)old_hop;

    espnow_send_tag_t tag = {
        .pkt_type = ESPNOW_PKT_DATA,
        .sequence = d->sequence,
    };
    memcpy(tag.dest_mac, next_hop, ESP_NOW_ETH_ALEN);

    esp_err_t ret = espnow_manager_send(next_hop, pkt, &tag);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to forward DATA to " MACSTR ": %s",
                 MAC2STR(next_hop), esp_err_to_name(ret));
    }
}

/* ================================================================
 * Handle incoming DATA packet (called from dispatcher)
 * ================================================================ */
void forwarding_handle_data(const espnow_evt_recv_t *evt)
{
    espnow_packet_t pkt = evt->packet;
    data_payload_t *d = &pkt.u.data;

    if (pkt.header.type != ESPNOW_PKT_DATA) return;

    /* Duplicate detection — check BEFORE processing */
    if (is_duplicate(d->source_mac, d->sequence)) {
        // Duplicate DATA dropped (routine log suppressed)
        return;
    }
    mark_seen(d->source_mac, d->sequence);

    const uint8_t *local = get_local_mac();

    /* Destination check */
    if (memcmp(d->destination_mac, local, ESP_NOW_ETH_ALEN) == 0) {
        /* Local delivery */
        (void)d;
        (void)evt;
        return;
    }

    /* Not for us — forward */
    forward_data(&pkt, evt->src_mac);
}

/* ================================================================
 * Role / MAC validation and startup banner
 * ================================================================ */
bool forwarding_validate_role(void)
{
    const uint8_t *local = get_local_mac();
    const uint8_t expected[] = NODE_EXPECTED_MAC;

    bool valid = (memcmp(local, expected, ESP_NOW_ETH_ALEN) == 0);

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "ESP-NOW PREDICTIVE MESH");
    ESP_LOGI(TAG, "STAGE 4 — Dynamic Routing");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Node role: %s", NODE_ROLE_NAME);
    ESP_LOGI(TAG, "Local MAC: " MACSTR, MAC2STR(local));
    ESP_LOGI(TAG, "Expected MAC: " MACSTR, MAC2STR(expected));
    ESP_LOGI(TAG, "Role/MAC: %s", valid ? "VALID" : "*** INVALID ***");
    ESP_LOGI(TAG, "RSSI threshold: NONE (unfiltered)");
    ESP_LOGI(TAG, "========================================");

    if (!valid) {
        ESP_LOGE(TAG, "ERROR: Role/MAC mismatch!");
    }

    return valid;
}

/* ================================================================
 * DATA generator task (NODE_A only)
 * ================================================================ */
#if NODE_ROLE_A
static uint32_t s_data_sequence = 0;

static void data_generator_task(void *arg)
{
    (void)arg;

    const uint8_t *local = get_local_mac();
    const uint8_t dest[] = TARGET_DATA_DESTINATION;

    ESP_LOGI(TAG, "DATA generator started (interval=%d ms)", DATA_GENERATE_INTERVAL_MS);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(DATA_GENERATE_INTERVAL_MS));

        s_data_sequence++;

        uint8_t next_hop[ESP_NOW_ETH_ALEN];
        uint8_t route_hops = 0;

        /* Dynamic routing lookup for destination */
        bool has_route = routing_lookup(dest, next_hop, &route_hops);
        if (!has_route) {
            // Destination unreachable (routine log suppressed)
            continue;
        }

        /* Check next hop is active and accepted */
        if (!neighbor_is_accepted(next_hop)) {
            // Next hop not accepted (routine log suppressed)
            continue;
        }

        espnow_manager_ensure_peer(next_hop);

        /* Build DATA packet */
        espnow_packet_t pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.header.type    = ESPNOW_PKT_DATA;
        pkt.header.version = ESPNOW_PROTOCOL_VERSION;
        pkt.header.length  = sizeof(espnow_header_t) + sizeof(data_payload_t);

        memcpy(pkt.u.data.source_mac, local, ESP_NOW_ETH_ALEN);
        memcpy(pkt.u.data.destination_mac, dest, ESP_NOW_ETH_ALEN);
        pkt.u.data.sequence  = s_data_sequence;
        pkt.u.data.hop_count = 0;
        snprintf(pkt.u.data.payload, sizeof(pkt.u.data.payload),
                 "Stage4 DATA packet #%" PRIu32, s_data_sequence);

        /* Mark as seen */
        mark_seen(local, s_data_sequence);

        // Routine DATA send logs suppressed

        espnow_send_tag_t tag = {
            .pkt_type = ESPNOW_PKT_DATA,
            .sequence = s_data_sequence,
        };
        memcpy(tag.dest_mac, next_hop, ESP_NOW_ETH_ALEN);

        esp_err_t ret = espnow_manager_send(next_hop, &pkt, &tag);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send DATA #%" PRIu32 ": %s",
                     s_data_sequence, esp_err_to_name(ret));
        }
    }
}
#endif /* NODE_ROLE_A */

void forwarding_start(void)
{
#if NODE_ROLE_A
    xTaskCreate(data_generator_task, "data_gen", 4096, NULL, 2, NULL);
#else
    ESP_LOGI(TAG, "Not NODE_A — DATA generator not started");
#endif
}
