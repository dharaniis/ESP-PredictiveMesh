#ifndef DISCOVERY_H
#define DISCOVERY_H

#include <stdint.h>
#include "espnow_manager.h"
#include "esp_now.h"

#define DISCOVERY_INTERVAL_MS 1000
#define STALE_THRESHOLD_MS    3000
#define DEAD_THRESHOLD_MS     6000

// Opaque handle; call discovery_start() to launch the background task.
typedef struct discovery_ctx discovery_ctx_t;

void discovery_start(void);

void handle_hello_packet(const espnow_evt_recv_t *evt);
void handle_hello_ack_packet(const espnow_evt_recv_t *evt);
void handle_data_packet(const espnow_evt_recv_t *evt);
#endif // DISCOVERY_H
