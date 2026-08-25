#ifndef ESPNOW_MANAGER_H
#define ESPNOW_MANAGER_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include <stdint.h>
#include <stdbool.h>
#include "esp_now.h"
#include "packets.h"

#define ESPNOW_CHANNEL         6
#define ESPNOW_BROADCAST_MAC   { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }

typedef enum {
    ESPNOW_EVT_SEND_DONE = 0,
    ESPNOW_EVT_RECV      = 1,
} espnow_evt_id_t;

// Per-send tag attached to every esp_now_send so the dispatcher can
// identify which logical packet completed.
typedef struct {
    espnow_packet_type_t pkt_type;
    uint32_t            sequence;
    uint8_t             dest_mac[ESP_NOW_ETH_ALEN];
} espnow_send_tag_t;

typedef struct {
    esp_now_send_status_t status;
    espnow_send_tag_t     tag;
} espnow_evt_send_t;

typedef struct {
    uint8_t  src_mac[ESP_NOW_ETH_ALEN];
    int8_t   rssi;        // raw RSSI from rx_ctrl
    int      data_len;
    espnow_packet_t packet; // local copy, header-validated
} espnow_evt_recv_t;

typedef struct {
    espnow_evt_id_t id;
    union {
        espnow_evt_send_t send;
        espnow_evt_recv_t recv;
    } u;
} espnow_event_t;

// Initialize Wi-Fi (STA, fixed channel), esp_now, callbacks, event queue.
// Must be called once from app_main before any other espnow_* call.
void espnow_manager_init(void);

// Register a peer if not already present. Returns ESP_OK on success or
// ESP_ERR_ESPNOW_EXIST (not an error). Any other code is a real failure.
esp_err_t espnow_manager_ensure_peer(const uint8_t *mac);

// Remove a peer. Returns ESP_OK on success or ESP_ERR_ESPNOW_NOT_FOUND.
esp_err_t espnow_manager_remove_peer(const uint8_t *mac);

// Send a fully-formed packet. Copies the packet internally (caller may
// free its buffer immediately on return). tag identifies the logical
// packet for the dispatcher's send-callback log.
esp_err_t espnow_manager_send(const uint8_t *dest_mac,
                              const espnow_packet_t *pkt,
                              const espnow_send_tag_t *tag);

// True if a peer is currently registered with the ESP-NOW driver.
bool espnow_manager_is_peer(const uint8_t *mac);
void espnow_manager_get_local_mac(uint8_t *out_mac);

// Read this node's Wi-Fi STA MAC into the supplied 6-byte buffer.
void espnow_manager_log_local_mac(void);

// Get the event queue used by the ESP-NOW callbacks.
QueueHandle_t espnow_manager_get_queue(void);

#endif // ESPNOW_MANAGER_H
