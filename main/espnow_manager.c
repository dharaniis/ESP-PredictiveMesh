#include "espnow_manager.h"
#include "node_config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "nvs_flash.h"

#include <string.h>

#define TAG "ESPNOW"
#define INFLIGHT_SLOTS 8

static QueueHandle_t s_event_queue = NULL;

// FIFO ring of in-flight send tags. Each esp_now_send pushes a tag; the
// Wi-Fi-task send callback pops the oldest. ESP-IDF invokes send
// callbacks in send order, so FIFO matching is correct.
static espnow_send_tag_t s_inflight[INFLIGHT_SLOTS];
static int s_inflight_head = 0;
static int s_inflight_count = 0;

static void push_inflight(const espnow_send_tag_t *tag)
{
    if (s_inflight_count == INFLIGHT_SLOTS) {
        ESP_LOGW(TAG, "In-flight ring full, oldest tag will be overwritten");
        s_inflight_head = (s_inflight_head + 1) % INFLIGHT_SLOTS;
        s_inflight_count--;
    }
    int tail = (s_inflight_head + s_inflight_count) % INFLIGHT_SLOTS;
    s_inflight[tail] = *tag;
    s_inflight_count++;
}

static bool pop_inflight(espnow_send_tag_t *out)
{
    if (s_inflight_count == 0) return false;
    *out = s_inflight[s_inflight_head];
    s_inflight_head = (s_inflight_head + 1) % INFLIGHT_SLOTS;
    s_inflight_count--;
    return true;
}

static void send_cb(const esp_now_send_info_t *tx_info,
                    esp_now_send_status_t status);
static void recv_cb(const esp_now_recv_info_t *recv_info,
                    const uint8_t *data, int data_len);

static void init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

static void wifi_init_sta_on_channel(uint8_t channel)
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
#ifdef NODE_TX_POWER
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(NODE_TX_POWER));
    ESP_LOGI(TAG, "Wi-Fi Max TX Power set to %d (%.2f dBm)", NODE_TX_POWER, NODE_TX_POWER * 0.25);
#endif

    uint8_t primary = 0;
    wifi_second_chan_t secondary = WIFI_SECOND_CHAN_NONE;
    esp_err_t ret = esp_wifi_get_channel(&primary, &secondary);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi channel: %u (configured: %u)", primary, channel);
    } else {
        ESP_LOGE(TAG, "esp_wifi_get_channel failed: %s", esp_err_to_name(ret));
    }
}

static void init_event_queue(void)
{
    s_event_queue = xQueueCreate(32, sizeof(espnow_event_t));
    if (!s_event_queue) {
        ESP_LOGE(TAG, "Failed to create ESP-NOW event queue");
        abort();
    }
}

void espnow_manager_init(void)
{
    init_nvs();
    wifi_init_sta_on_channel(ESPNOW_CHANNEL);
    init_event_queue();

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(send_cb));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));
    ESP_LOGI(TAG, "ESP-NOW initialized: YES");
}

esp_err_t espnow_manager_ensure_peer(const uint8_t *mac)
{
    if (esp_now_is_peer_exist(mac)) {
        return ESP_OK;
    }
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, mac, ESP_NOW_ETH_ALEN);
    peer.channel = ESPNOW_CHANNEL;
    peer.ifidx   = WIFI_IF_STA;
    peer.encrypt = false;
    esp_err_t ret = esp_now_add_peer(&peer);
    if (ret == ESP_OK) {
        // Peer registered (routine log suppressed)
    } else if (ret != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGE(TAG, "esp_now_add_peer(" MACSTR ") failed: %s",
                 MAC2STR(mac), esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t espnow_manager_remove_peer(const uint8_t *mac)
{
    if (!esp_now_is_peer_exist(mac)) {
        return ESP_ERR_ESPNOW_NOT_FOUND;
    }
    esp_err_t ret = esp_now_del_peer(mac);
    if (ret == ESP_OK) {
        // Peer removed (routine log suppressed)
    } else {
        ESP_LOGE(TAG, "esp_now_del_peer failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t espnow_manager_send(const uint8_t *dest_mac,
                              const espnow_packet_t *pkt,
                              const espnow_send_tag_t *tag)
{
    if (tag == NULL || pkt == NULL || dest_mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    push_inflight(tag);
    // Total wire length = header + (header.length - sizeof(header))
    uint16_t payload_len = pkt->header.length - sizeof(espnow_header_t);
    esp_err_t ret = esp_now_send(dest_mac, (const uint8_t *)pkt,
                                 sizeof(espnow_header_t) + payload_len);
    if (ret != ESP_OK) {
        espnow_send_tag_t dropped;
        (void)pop_inflight(&dropped);
        ESP_LOGE(TAG, "esp_now_send failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

bool espnow_manager_is_peer(const uint8_t *mac)
{
    return esp_now_is_peer_exist(mac);
}

void espnow_manager_get_local_mac(uint8_t *out_mac)
{
    ESP_ERROR_CHECK(esp_read_mac(out_mac, ESP_MAC_WIFI_STA));
}

void espnow_manager_log_local_mac(void)
{
    uint8_t mac[ESP_NOW_ETH_ALEN];
    espnow_manager_get_local_mac(mac);
    ESP_LOGI(TAG, "Local MAC: " MACSTR, MAC2STR(mac));
}

QueueHandle_t espnow_manager_get_queue(void)
{
    return s_event_queue;
}

static void send_cb(const esp_now_send_info_t *tx_info,
                    esp_now_send_status_t status)
{
    if (tx_info == NULL || tx_info->des_addr == NULL) {
        ESP_LOGE(TAG, "Send callback: invalid argument");
        return;
    }

    espnow_event_t evt = { .id = ESPNOW_EVT_SEND_DONE };
    evt.u.send.status = status;
    memcpy(evt.u.send.tag.dest_mac, tx_info->des_addr, ESP_NOW_ETH_ALEN);

    espnow_send_tag_t tag;
    if (pop_inflight(&tag)) {
        evt.u.send.tag = tag;
    } else {
        evt.u.send.tag.pkt_type = (espnow_packet_type_t)0;
        evt.u.send.tag.sequence = 0;
    }

    if (xQueueSend(s_event_queue, &evt, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Send callback: queue full, dropping event");
    }
}

static void recv_cb(const esp_now_recv_info_t *recv_info,
                    const uint8_t *data, int data_len)
{
    if (recv_info == NULL || recv_info->src_addr == NULL ||
        data == NULL || data_len <= 0) {
        ESP_LOGE(TAG, "Recv callback: invalid argument");
        return;
    }

    espnow_event_t evt = { .id = ESPNOW_EVT_RECV };
    memcpy(evt.u.recv.src_mac, recv_info->src_addr, ESP_NOW_ETH_ALEN);
    evt.u.recv.rssi     = (recv_info->rx_ctrl != NULL) ? recv_info->rx_ctrl->rssi : 0;
    evt.u.recv.data_len = data_len;

    if (data_len < (int)sizeof(espnow_header_t)) {
        ESP_LOGW(TAG, "Recv callback: short frame (%d bytes), ignoring",
                 data_len);
        return;
    }

    int copy_len = (data_len < (int)sizeof(espnow_packet_t))
                   ? data_len : (int)sizeof(espnow_packet_t);
    memcpy(&evt.u.recv.packet, data, copy_len);

    if (evt.u.recv.packet.header.length != (uint16_t)data_len) {
        ESP_LOGW(TAG, "Recv callback: length mismatch (hdr=%u, wire=%d)",
                 evt.u.recv.packet.header.length, data_len);
        return;
    }
    if (evt.u.recv.packet.header.version != ESPNOW_PROTOCOL_VERSION) {
        ESP_LOGW(TAG, "Recv callback: unknown version %u",
                 evt.u.recv.packet.header.version);
        return;
    }

    if (xQueueSend(s_event_queue, &evt, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Recv callback: queue full, dropping event");
    }
}
