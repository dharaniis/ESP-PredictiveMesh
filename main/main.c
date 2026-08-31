#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "metrics.h"

#include "discovery.h"
#include "espnow_manager.h"
#include "forwarding.h"
#include "neighbor_manager.h"
#include "topology.h"
#include "packets.h"
#include "esp_log.h"

extern void handle_metric_probe_packet(const espnow_evt_recv_t *evt);
extern void handle_metric_ack_packet(const espnow_evt_recv_t *evt);
// These handlers are called by the dispatcher.
// They are defined in discovery.c.
void handle_hello_packet(const espnow_evt_recv_t *evt);
void handle_hello_ack_packet(const espnow_evt_recv_t *evt);

static void espnow_dispatch_task(void *pvParameter)
{
    (void)pvParameter;
    QueueHandle_t queue = espnow_manager_get_queue();
    espnow_event_t evt;

    for (;;) {
        if (xQueueReceive(queue, &evt, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (evt.id) {
        case ESPNOW_EVT_SEND_DONE:
            if (evt.u.send.status == ESP_NOW_SEND_SUCCESS) {
                ESP_LOGI("MAIN", "Packet #%" PRIu32 " (type %d) send status: SUCCESS",
                         evt.u.send.tag.sequence, evt.u.send.tag.pkt_type);
            } else {
                ESP_LOGW("MAIN", "Packet #%" PRIu32 " (type %d) send status: FAIL",
                         evt.u.send.tag.sequence, evt.u.send.tag.pkt_type);
            }
            break;

        case ESPNOW_EVT_RECV:
            switch (evt.u.recv.packet.header.type) {
            case ESPNOW_PKT_HELLO:
                handle_hello_packet(&evt.u.recv);
                break;
            case ESPNOW_PKT_HELLO_ACK:
                handle_hello_ack_packet(&evt.u.recv);
                break;
            case ESPNOW_PKT_METRIC_PROBE:
                handle_metric_probe_packet(&evt.u.recv);
                break;
            case ESPNOW_PKT_METRIC_ACK:
                handle_metric_ack_packet(&evt.u.recv);
                break;
            case ESPNOW_PKT_TOPOLOGY:
                topology_handle_packet(&evt.u.recv);
                break;
            case ESPNOW_PKT_DATA:
                forwarding_handle_data(&evt.u.recv);
                break;
            default:
                ESP_LOGW("MAIN", "Received unknown packet type %u",
                         evt.u.recv.packet.header.type);
                break;
            }
            break;

        default:
            ESP_LOGE("MAIN", "Unknown event id: %d", evt.id);
            break;
        }
    }
}

void app_main(void)
{
    espnow_manager_init();
    neighbor_manager_init();
    topology_init();

    /* Stage 4 startup banner with role/MAC validation */
    bool role_valid = forwarding_validate_role();

    discovery_start();
    topology_start();

    xTaskCreate(espnow_dispatch_task, "espnow_dispatch",
                4096, NULL, 3, NULL);

    /* Start DATA generator only if role/MAC is valid (NODE_A only) */
    if (role_valid) {
        forwarding_start();
    }

    ESP_LOGI("MAIN", "Node initialized. Stage 4 dynamic routing active.");
}
