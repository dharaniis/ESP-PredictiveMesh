#include "esp_mac.h"

#include "discovery.h"
#include "neighbor_manager.h"
#include "topology.h"
#include "espnow_manager.h"

#include "packets.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

#include <string.h>

#define TAG "DISCOVERY"

static uint8_t s_broadcast_mac[ESP_NOW_ETH_ALEN] = ESPNOW_BROADCAST_MAC;
static uint32_t s_hello_sequence = 0;

// ---- packet builders ---------------------------------------------------

static void build_hello(espnow_packet_t *pkt)
{
    memset(pkt, 0, sizeof(*pkt));
    pkt->header.type    = ESPNOW_PKT_HELLO;
    pkt->header.version = ESPNOW_PROTOCOL_VERSION;
    pkt->header.length  = sizeof(espnow_header_t) + sizeof(hello_payload_t);
    espnow_manager_get_local_mac(pkt->u.hello.source_mac);
    pkt->u.hello.sequence = ++s_hello_sequence;
}

static void build_hello_ack(const espnow_packet_t *hello,
                            espnow_packet_t *ack)
{
    memset(ack, 0, sizeof(*ack));
    ack->header.type    = ESPNOW_PKT_HELLO_ACK;
    ack->header.version = ESPNOW_PROTOCOL_VERSION;
    ack->header.length  = sizeof(espnow_header_t) + sizeof(hello_ack_payload_t);

    // ACK goes back to the HELLO sender.
    espnow_manager_get_local_mac(ack->u.hello_ack.source_mac);
    memcpy(ack->u.hello_ack.destination_mac, hello->u.hello.source_mac,
           ESP_NOW_ETH_ALEN);
    ack->u.hello_ack.hello_sequence = hello->u.hello.sequence;
}

// ---- internal ----------------------------------------------------------

void handle_hello_packet(const espnow_evt_recv_t *evt)
{
    const espnow_packet_t *pkt = &evt->packet;
    const uint8_t *src = evt->src_mac;
    int8_t rssi = evt->rssi;

    // Sanity: packet type should be HELLO
    if (pkt->header.type != ESPNOW_PKT_HELLO) return;

    // Look up or create neighbor entry.
    neighbor_t *n = neighbor_find(src);
    if (!n) {
        n = neighbor_add_or_update(src);
        if (!n) {
            ESP_LOGW(TAG, "Cannot add neighbor " MACSTR " (table full)",
                     MAC2STR(src));
            return;
        }
    }

    // Update observation (RSSI + sequence + timestamp).
    neighbor_update_observation(src, rssi, pkt->u.hello.sequence);

    // Ensure the sender is registered as a peer (for future unicast ACK).
    espnow_manager_ensure_peer(src);

    // Send HELLO_ACK unicast to the HELLO sender.
    espnow_packet_t ack;
    build_hello_ack(pkt, &ack);
    espnow_send_tag_t tag = {
        .pkt_type = ESPNOW_PKT_HELLO_ACK,
        .sequence = pkt->u.hello.sequence,
    };
    memcpy(tag.dest_mac, src, ESP_NOW_ETH_ALEN);
    esp_err_t ret = espnow_manager_send(src, &ack, &tag);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send HELLO_ACK to " MACSTR ": %s",
                 MAC2STR(src), esp_err_to_name(ret));
    }
}

void handle_hello_ack_packet(const espnow_evt_recv_t *evt)
{
    const espnow_packet_t *pkt = &evt->packet;
    const uint8_t *src = evt->src_mac;
    int8_t rssi = evt->rssi;

    if (pkt->header.type != ESPNOW_PKT_HELLO_ACK) return;

    // The ACK is from the destination MAC field in the ACK.
    const uint8_t *orig_dest = pkt->u.hello_ack.destination_mac;
    // We only care about ACKs addressed to us.
    uint8_t local_mac[ESP_NOW_ETH_ALEN];
    espnow_manager_get_local_mac(local_mac);
    if (memcmp(orig_dest, local_mac, ESP_NOW_ETH_ALEN) != 0) {
        // Not for us — ignore.
        return;
    }

    // Update / create neighbor entry with the ACK sender's RSSI.
    neighbor_t *n = neighbor_find(src);
    if (!n) {
        n = neighbor_add_or_update(src);
        if (!n) {
            ESP_LOGW(TAG, "Cannot add neighbor " MACSTR " (table full)",
                     MAC2STR(src));
            return;
        }
    }
    neighbor_update_observation(src, rssi, pkt->u.hello_ack.hello_sequence);
    // Ensure peer is registered (for future DATA/HELLO unicast if needed).
    espnow_manager_ensure_peer(src);
}

// ---- discovery task ----------------------------------------------------

static void discovery_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Discovery task started (interval=%d ms, stale=%d ms, dead=%d ms)",
             DISCOVERY_INTERVAL_MS, STALE_THRESHOLD_MS, DEAD_THRESHOLD_MS);

    // Register broadcast peer once for HELLO sends.
    espnow_manager_ensure_peer(s_broadcast_mac);

    while (1) {
        // 1. Broadcast HELLO.
        espnow_packet_t hello;
        build_hello(&hello);
        espnow_send_tag_t tag = {
            .pkt_type = ESPNOW_PKT_HELLO,
            .sequence = hello.u.hello.sequence,
        };
        memcpy(tag.dest_mac, s_broadcast_mac, ESP_NOW_ETH_ALEN);
        espnow_manager_send(s_broadcast_mac, &hello, &tag);

        int64_t now = esp_timer_get_time();
        uint8_t expired_macs[MAX_NEIGHBORS][ESP_NOW_ETH_ALEN];
        int num_expired = neighbor_expire(now, STALE_THRESHOLD_MS * 1000, DEAD_THRESHOLD_MS * 1000, expired_macs, MAX_NEIGHBORS);
        for (int i = 0; i < num_expired; i++) {
            espnow_manager_remove_peer(expired_macs[i]);
        }
        if (num_expired > 0) {
            topology_recalculate_routes();
        }

        // 3. Print table periodically.
        neighbor_print_table();

        vTaskDelay(pdMS_TO_TICKS(DISCOVERY_INTERVAL_MS));
    }
}

void discovery_start(void)
{
    xTaskCreate(discovery_task, "discovery", 4096, NULL, 3, NULL);
}