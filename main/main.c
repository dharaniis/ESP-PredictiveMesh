/*
 * ESP-PredictiveMesh - Stage 1: point-to-point ESP-NOW between two ESP32s.
 * Set exactly one of NODE_ROLE_SENDER / NODE_ROLE_RECEIVER.
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "nvs_flash.h"

#define NODE_ROLE_SENDER
/* #define NODE_ROLE_RECEIVER */

#if !defined(NODE_ROLE_SENDER) && !defined(NODE_ROLE_RECEIVER)
#error "Define exactly one of NODE_ROLE_SENDER or NODE_ROLE_RECEIVER."
#endif

#if defined(NODE_ROLE_SENDER) && defined(NODE_ROLE_RECEIVER)
#error "Define only one of NODE_ROLE_SENDER / NODE_ROLE_RECEIVER."
#endif

#define ESP_NOW_CHANNEL            6
#define SENDER_SEND_PERIOD_MS      1000
#define SENDER_TASK_STACK_WORDS    4096
#define SENDER_TASK_PRIORITY       4

#if defined(NODE_ROLE_SENDER)
static const uint8_t RECEIVER_MAC[ESP_NOW_ETH_ALEN] = {
    0xE0, 0x8C, 0xFE, 0xF9, 0xB8, 0xC0
};
#endif

typedef struct __attribute__((packed)) {
    uint32_t sequence;
    int64_t  timestamp_us;
    char     payload[32];
} data_packet_t;

_Static_assert(sizeof(data_packet_t) < ESP_NOW_MAX_DATA_LEN,
               "data_packet_t exceeds ESP_NOW_MAX_DATA_LEN (250 bytes).");

void espnow_send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status);
void espnow_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int data_len);

static const char *TAG = "ESP-NOW";
static QueueHandle_t s_event_queue = NULL;

// Sequence of the most recent packet handed to esp_now_send(). Read by the
// Wi-Fi-task send callback to attach the per-packet status line.
static volatile uint32_t s_last_queued_sequence = 0;

typedef enum {
    ESPNOW_EVENT_SEND_DONE = 0,
    ESPNOW_EVENT_RECV_DATA = 1,
} espnow_event_id_t;

typedef struct {
    uint8_t  mac_addr[ESP_NOW_ETH_ALEN];
    esp_now_send_status_t status;
    uint32_t sequence;
} espnow_event_send_t;

typedef struct {
    uint8_t  mac_addr[ESP_NOW_ETH_ALEN];
    data_packet_t packet;
    int      data_len;
} espnow_event_recv_t;

typedef struct {
    espnow_event_id_t id;
    union {
        espnow_event_send_t send;
        espnow_event_recv_t recv;
    } info;
} espnow_event_t;

static void log_mac(const char *label, const uint8_t *mac)
{
    ESP_LOGI(TAG, "%s: " MACSTR, label, MAC2STR(mac));
}

static esp_err_t wifi_init_sta_on_channel(uint8_t channel)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE));

    uint8_t primary = 0;
    wifi_second_chan_t secondary = WIFI_SECOND_CHAN_NONE;
    esp_err_t ret = esp_wifi_get_channel(&primary, &secondary);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi channel: %u (configured: %u)", primary, channel);
    } else {
        ESP_LOGE(TAG, "esp_wifi_get_channel failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t espnow_register_callbacks(void)
{
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(espnow_send_cb));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));
    ESP_LOGI(TAG, "ESP-NOW initialized: YES");
    return ESP_OK;
}

#if defined(NODE_ROLE_SENDER)
static esp_err_t espnow_add_receiver_peer(void)
{
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, RECEIVER_MAC, ESP_NOW_ETH_ALEN);
    peer.channel = ESP_NOW_CHANNEL;
    peer.ifidx   = WIFI_IF_STA;
    peer.encrypt = false;

    esp_err_t ret = esp_now_add_peer(&peer);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Peer registered (receiver): " MACSTR,
                 MAC2STR(RECEIVER_MAC));
    } else {
        ESP_LOGE(TAG, "esp_now_add_peer failed: %s", esp_err_to_name(ret));
    }
    return ret;
}
#endif

// Callbacks run in the Wi-Fi task: keep them short. Validate, copy the
// fixed-size event, and hand it off to the dispatcher task.
void espnow_send_cb(const esp_now_send_info_t *tx_info,
                    esp_now_send_status_t status)
{
    if (tx_info == NULL || tx_info->des_addr == NULL) {
        ESP_LOGE(TAG, "Send callback: invalid argument");
        return;
    }

    espnow_event_t evt = {
        .id = ESPNOW_EVENT_SEND_DONE,
    };
    memcpy(evt.info.send.mac_addr, tx_info->des_addr, ESP_NOW_ETH_ALEN);
    evt.info.send.status = status;
    evt.info.send.sequence = s_last_queued_sequence;

    if (xQueueSend(s_event_queue, &evt, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Send callback: queue full, dropping event");
    }
}

void espnow_recv_cb(const esp_now_recv_info_t *recv_info,
                    const uint8_t *data, int data_len)
{
    if (recv_info == NULL || recv_info->src_addr == NULL ||
        data == NULL || data_len <= 0) {
        ESP_LOGE(TAG, "Recv callback: invalid argument");
        return;
    }

    if (data_len < (int)sizeof(data_packet_t)) {
        ESP_LOGW(TAG, "Recv callback: short frame (%d bytes), ignoring",
                 data_len);
        return;
    }

    espnow_event_t evt = {
        .id = ESPNOW_EVENT_RECV_DATA,
    };
    memcpy(evt.info.recv.mac_addr, recv_info->src_addr, ESP_NOW_ETH_ALEN);
    memcpy(&evt.info.recv.packet, data, sizeof(data_packet_t));
    evt.info.recv.data_len = data_len;

    if (xQueueSend(s_event_queue, &evt, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Recv callback: queue full, dropping event");
    }
}

static void espnow_dispatch_task(void *pvParameter)
{
    (void)pvParameter;
    espnow_event_t evt;

    for (;;) {
        if (xQueueReceive(s_event_queue, &evt, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (evt.id) {
        case ESPNOW_EVENT_SEND_DONE:
            if (evt.info.send.status == ESP_NOW_SEND_SUCCESS) {
                ESP_LOGI(TAG, "Packet #%" PRIu32 " send status: SUCCESS",
                         evt.info.send.sequence);
            } else {
                ESP_LOGW(TAG, "Packet #%" PRIu32 " send status: FAIL",
                         evt.info.send.sequence);
            }
            break;

        case ESPNOW_EVENT_RECV_DATA:
            log_mac("Received from", evt.info.recv.mac_addr);
            ESP_LOGI(TAG, "Received packet #%" PRIu32,
                     evt.info.recv.packet.sequence);
            ESP_LOGI(TAG, "Payload: %.*s",
                     (int)sizeof(evt.info.recv.packet.payload),
                     evt.info.recv.packet.payload);
            break;

        default:
            ESP_LOGE(TAG, "Unknown event id: %d", evt.id);
            break;
        }
    }
}

#if defined(NODE_ROLE_SENDER)
static void sender_task(void *pvParameter)
{
    (void)pvParameter;

    uint32_t sequence = 0;
    for (;;) {
        data_packet_t packet = {0};
        sequence++;
        packet.sequence    = sequence;
        packet.timestamp_us = esp_timer_get_time();
        snprintf(packet.payload, sizeof(packet.payload),
                 "Hello from ESP32");

        esp_err_t ret = esp_now_send(RECEIVER_MAC,
                                     (const uint8_t *)&packet,
                                     sizeof(packet));
        if (ret == ESP_OK) {
            s_last_queued_sequence = sequence;
            ESP_LOGI(TAG, "Packet #%" PRIu32 " queued for transmission",
                     sequence);
        } else {
            ESP_LOGE(TAG, "Packet #%" PRIu32 " esp_now_send failed: %s",
                     sequence, esp_err_to_name(ret));
        }

        vTaskDelay(pdMS_TO_TICKS(SENDER_SEND_PERIOD_MS));
    }
}
#endif

static void print_banner(const uint8_t *local_mac)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "ESP-NOW RESEARCH PROJECT - STAGE 1");
    ESP_LOGI(TAG, "========================================");
#if defined(NODE_ROLE_SENDER)
    ESP_LOGI(TAG, "Role: SENDER");
#else
    ESP_LOGI(TAG, "Role: RECEIVER");
#endif
    log_mac("Local MAC", local_mac);
    ESP_LOGI(TAG, "Channel: %d", ESP_NOW_CHANNEL);
    ESP_LOGI(TAG, "========================================");
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(wifi_init_sta_on_channel(ESP_NOW_CHANNEL));

    uint8_t local_mac[ESP_NOW_ETH_ALEN] = {0};
    ESP_ERROR_CHECK(esp_read_mac(local_mac, ESP_MAC_WIFI_STA));
    print_banner(local_mac);

    s_event_queue = xQueueCreate(16, sizeof(espnow_event_t));
    if (s_event_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create ESP-NOW event queue");
        abort();
    }

    ESP_ERROR_CHECK(espnow_register_callbacks());

#if defined(NODE_ROLE_SENDER)
    ESP_ERROR_CHECK(espnow_add_receiver_peer());

    xTaskCreate(espnow_dispatch_task, "espnow_dispatch",
                2048, NULL, 3, NULL);
    xTaskCreate(sender_task, "espnow_sender",
                SENDER_TASK_STACK_WORDS, NULL, SENDER_TASK_PRIORITY, NULL);

    ESP_LOGI(TAG, "Sender running, transmitting every %d ms.",
             SENDER_SEND_PERIOD_MS);
#else
    xTaskCreate(espnow_dispatch_task, "espnow_dispatch",
                2048, NULL, 3, NULL);

    ESP_LOGI(TAG, "Receiver running, awaiting packets.");
#endif
}
