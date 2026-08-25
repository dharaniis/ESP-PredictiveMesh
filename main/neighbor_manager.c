#include "esp_mac.h"

#include "neighbor_manager.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "NEIGHBOR";
static neighbor_t s_table[MAX_NEIGHBORS];

static int find_slot(const uint8_t *mac)
{
    for (int i = 0; i < MAX_NEIGHBORS; i++) {
        if (s_table[i].state != NEIGHBOR_EMPTY &&
            memcmp(s_table[i].mac, mac, ESP_NOW_ETH_ALEN) == 0) {
            return i;
        }
    }
    return -1;
}

static int find_free_slot(void)
{
    for (int i = 0; i < MAX_NEIGHBORS; i++) {
        if (s_table[i].state == NEIGHBOR_EMPTY) return i;
    }
    return -1;
}

void neighbor_manager_init(void)
{
    memset(s_table, 0, sizeof(s_table));
}

neighbor_t *neighbor_find(const uint8_t *mac)
{
    int i = find_slot(mac);
    return (i < 0) ? NULL : &s_table[i];
}

neighbor_t *neighbor_add_or_update(const uint8_t *mac)
{
    neighbor_t *existing = neighbor_find(mac);
    if (existing) return existing;

    int i = find_free_slot();
    if (i < 0) {
        ESP_LOGW(TAG, "Table full, cannot add " MACSTR, MAC2STR(mac));
        return NULL;
    }
    memcpy(s_table[i].mac, mac, ESP_NOW_ETH_ALEN);
    s_table[i].rssi = 0;
    s_table[i].hello_sequence = 0;
    s_table[i].last_seen_us = 0;
    s_table[i].state = NEIGHBOR_ACTIVE;
    ESP_LOGI(TAG, "Neighbor added: " MACSTR, MAC2STR(mac));
    return &s_table[i];
}

neighbor_t *neighbor_update_observation(const uint8_t *mac,
                                        int8_t rssi,
                                        uint32_t hello_sequence)
{
    neighbor_t *n = neighbor_find(mac);
    if (!n) return NULL;
    n->rssi = rssi;
    n->last_seen_us = esp_timer_get_time();
    n->hello_sequence = hello_sequence;
    n->state = NEIGHBOR_ACTIVE;
    return n;
}

int neighbor_expire(int64_t now_us, int64_t stale_us, int64_t dead_us)
{
    int removed = 0;
    for (int i = 0; i < MAX_NEIGHBORS; i++) {
        if (s_table[i].state == NEIGHBOR_EMPTY) continue;
        int64_t age = now_us - s_table[i].last_seen_us;
        if (age >= dead_us) {
            ESP_LOGI(TAG, "Neighbor removed: " MACSTR, MAC2STR(s_table[i].mac));
            memset(&s_table[i], 0, sizeof(neighbor_t));
            removed++;
        } else if (age >= stale_us && s_table[i].state == NEIGHBOR_ACTIVE) {
            s_table[i].state = NEIGHBOR_STALE;
            ESP_LOGW(TAG, "Neighbor stale: " MACSTR, MAC2STR(s_table[i].mac));
        }
    }
    return removed;
}

int neighbor_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_NEIGHBORS; i++) {
        if (s_table[i].state != NEIGHBOR_EMPTY) n++;
    }
    return n;
}

static const char *state_name(neighbor_state_t s)
{
    switch (s) {
    case NEIGHBOR_ACTIVE: return "ACTIVE";
    case NEIGHBOR_STALE:  return "STALE ";
    case NEIGHBOR_EMPTY:  return "EMPTY ";
    default:              return "?";
    }
}

void neighbor_print_table(void)
{
    ESP_LOGI(TAG, "--------------------------------------------");
    ESP_LOGI(TAG, "Neighbor Table (%d)", neighbor_count());
    ESP_LOGI(TAG, "--------------------------------------------");
    ESP_LOGI(TAG, "MAC                  RSSI   Age(s)   State");
    for (int i = 0; i < MAX_NEIGHBORS; i++) {
        if (s_table[i].state == NEIGHBOR_EMPTY) continue;
        int64_t now = esp_timer_get_time();
        double age_s = (double)(now - s_table[i].last_seen_us) / 1e6;
        ESP_LOGI(TAG, MACSTR "  %4d   %5.2f   %s",
                 MAC2STR(s_table[i].mac),
                 (int)s_table[i].rssi,
                 age_s,
                 state_name(s_table[i].state));
    }
    ESP_LOGI(TAG, "--------------------------------------------");
}
