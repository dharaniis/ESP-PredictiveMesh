#include "esp_mac.h"
#include "topology.h"
#include "espnow_manager.h"
#include "node_config.h"
#include "packets.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>

#define TAG "TOPOLOGY"

#define MAX_TOPOLOGY_NODES 16
#define MAX_GRAPH_NODES    16

typedef struct {
    uint8_t  node_mac[ESP_NOW_ETH_ALEN];
    uint32_t sequence;
    uint8_t  neighbor_count;
    uint8_t  neighbors[MAX_TOPOLOGY_NEIGHBORS][ESP_NOW_ETH_ALEN];
    int64_t  last_seen_us;
    bool     valid;
} topology_db_entry_t;

static topology_db_entry_t s_topology_db[MAX_TOPOLOGY_NODES];
static route_entry_t       s_routing_table[MAX_GRAPH_NODES];
static int                 s_route_count = 0;
static SemaphoreHandle_t   s_routing_mutex = NULL;

void topology_init(void)
{
    memset(s_topology_db, 0, sizeof(s_topology_db));
    memset(s_routing_table, 0, sizeof(s_routing_table));
    s_route_count = 0;

    s_routing_mutex = xSemaphoreCreateMutex();
    if (!s_routing_mutex) {
        ESP_LOGE(TAG, "Failed to create routing table mutex");
        abort();
    }
}

void routing_print_table(void)
{
    if (!s_routing_mutex) return;
    xSemaphoreTake(s_routing_mutex, portMAX_DELAY);
    printf("\n========================================\n");
    printf("ROUTING TABLE\n");
    printf("========================================\n");
    printf("%-24s %-24s %-6s\n", "Destination", "Next Hop", "Hops");
    if (s_route_count == 0) {
        printf("(empty - no valid routes)\n");
    } else {
        for (int i = 0; i < s_route_count; i++) {
            char dest_str[24];
            char next_str[24];
            snprintf(dest_str, sizeof(dest_str), MACSTR, MAC2STR(s_routing_table[i].destination));
            snprintf(next_str, sizeof(next_str), MACSTR, MAC2STR(s_routing_table[i].next_hop));
            printf("%-24s %-24s %-6u\n", dest_str, next_str, s_routing_table[i].hop_count);
        }
    }
    printf("========================================\n\n");
    xSemaphoreGive(s_routing_mutex);
}

bool routing_lookup(const uint8_t *dest_mac, uint8_t *out_next_hop, uint8_t *out_hop_count)
{
    if (!s_routing_mutex || !dest_mac) return false;

    bool found = false;
    xSemaphoreTake(s_routing_mutex, portMAX_DELAY);
    for (int i = 0; i < s_route_count; i++) {
        if (s_routing_table[i].valid && memcmp(s_routing_table[i].destination, dest_mac, ESP_NOW_ETH_ALEN) == 0) {
            if (out_next_hop) memcpy(out_next_hop, s_routing_table[i].next_hop, ESP_NOW_ETH_ALEN);
            if (out_hop_count) *out_hop_count = s_routing_table[i].hop_count;
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_routing_mutex);
    return found;
}

void topology_recalculate_routes(void)
{
    if (!s_routing_mutex) return;

    uint8_t local_mac[ESP_NOW_ETH_ALEN];
    espnow_manager_get_local_mac(local_mac);

    // 1. Build graph node list
    uint8_t graph_nodes[MAX_GRAPH_NODES][ESP_NOW_ETH_ALEN];
    int node_count = 0;

    // Node 0 is always local MAC
    memcpy(graph_nodes[node_count++], local_mac, ESP_NOW_ETH_ALEN);

    // Add direct accepted neighbors
    uint8_t direct_macs[MAX_NEIGHBORS][ESP_NOW_ETH_ALEN];
    int direct_count = neighbor_get_accepted_macs(direct_macs, MAX_NEIGHBORS);
    for (int i = 0; i < direct_count; i++) {
        bool exists = false;
        for (int j = 0; j < node_count; j++) {
            if (memcmp(graph_nodes[j], direct_macs[i], ESP_NOW_ETH_ALEN) == 0) {
                exists = true;
                break;
            }
        }
        if (!exists && node_count < MAX_GRAPH_NODES) {
            memcpy(graph_nodes[node_count++], direct_macs[i], ESP_NOW_ETH_ALEN);
        }
    }

    // Add remote nodes from topology DB
    int64_t now_us = esp_timer_get_time();
    for (int i = 0; i < MAX_TOPOLOGY_NODES; i++) {
        if (s_topology_db[i].valid && (now_us - s_topology_db[i].last_seen_us <= (int64_t)TOPOLOGY_EXPIRE_MS * 1000LL)) {
            bool exists = false;
            for (int j = 0; j < node_count; j++) {
                if (memcmp(graph_nodes[j], s_topology_db[i].node_mac, ESP_NOW_ETH_ALEN) == 0) {
                    exists = true;
                    break;
                }
            }
            if (!exists && node_count < MAX_GRAPH_NODES) {
                memcpy(graph_nodes[node_count++], s_topology_db[i].node_mac, ESP_NOW_ETH_ALEN);
            }

            for (int k = 0; k < s_topology_db[i].neighbor_count; k++) {
                bool n_exists = false;
                for (int j = 0; j < node_count; j++) {
                    if (memcmp(graph_nodes[j], s_topology_db[i].neighbors[k], ESP_NOW_ETH_ALEN) == 0) {
                        n_exists = true;
                        break;
                    }
                }
                if (!n_exists && node_count < MAX_GRAPH_NODES) {
                    memcpy(graph_nodes[node_count++], s_topology_db[i].neighbors[k], ESP_NOW_ETH_ALEN);
                }
            }
        }
    }

    // 2. Build adjacency matrix/lists
    typedef struct {
        int neighbor_indices[MAX_GRAPH_NODES];
        int count;
    } adj_list_t;

    adj_list_t adj[MAX_GRAPH_NODES];
    memset(adj, 0, sizeof(adj));

    // Adjacency for node 0 (local node)
    for (int i = 0; i < direct_count; i++) {
        for (int j = 0; j < node_count; j++) {
            if (memcmp(graph_nodes[j], direct_macs[i], ESP_NOW_ETH_ALEN) == 0) {
                adj[0].neighbor_indices[adj[0].count++] = j;
                break;
            }
        }
    }

    // Adjacency for remote nodes
    for (int idx = 1; idx < node_count; idx++) {
        for (int i = 0; i < MAX_TOPOLOGY_NODES; i++) {
            if (s_topology_db[i].valid &&
                (now_us - s_topology_db[i].last_seen_us <= (int64_t)TOPOLOGY_EXPIRE_MS * 1000LL) &&
                memcmp(s_topology_db[i].node_mac, graph_nodes[idx], ESP_NOW_ETH_ALEN) == 0) {

                for (int k = 0; k < s_topology_db[i].neighbor_count; k++) {
                    for (int j = 0; j < node_count; j++) {
                        if (memcmp(graph_nodes[j], s_topology_db[i].neighbors[k], ESP_NOW_ETH_ALEN) == 0) {
                            adj[idx].neighbor_indices[adj[idx].count++] = j;
                            break;
                        }
                    }
                }
                break;
            }
        }
    }

    // 3. BFS Shortest Path Algorithm
    bool visited[MAX_GRAPH_NODES] = { false };
    uint8_t dist[MAX_GRAPH_NODES] = { 0 };
    int next_hop_idx[MAX_GRAPH_NODES];
    for (int i = 0; i < MAX_GRAPH_NODES; i++) next_hop_idx[i] = -1;

    int queue[MAX_GRAPH_NODES];
    int head = 0, tail = 0;

    visited[0] = true;
    dist[0] = 0;
    queue[tail++] = 0;

    while (head < tail) {
        int u = queue[head++];
        for (int i = 0; i < adj[u].count; i++) {
            int v = adj[u].neighbor_indices[i];
            if (!visited[v]) {
                visited[v] = true;
                dist[v] = dist[u] + 1;
                next_hop_idx[v] = (u == 0) ? v : next_hop_idx[u];
                queue[tail++] = v;
            }
        }
    }

    // 4. Construct new routing table
    route_entry_t new_routes[MAX_GRAPH_NODES];
    int new_route_count = 0;

    for (int i = 1; i < node_count; i++) {
        if (visited[i] && next_hop_idx[i] >= 0) {
            // CRITICAL RULE: Never create a route to the local node
            if (memcmp(graph_nodes[i], local_mac, ESP_NOW_ETH_ALEN) == 0) {
                continue;
            }

            memcpy(new_routes[new_route_count].destination, graph_nodes[i], ESP_NOW_ETH_ALEN);
            memcpy(new_routes[new_route_count].next_hop, graph_nodes[next_hop_idx[i]], ESP_NOW_ETH_ALEN);
            new_routes[new_route_count].hop_count = dist[i];
            new_routes[new_route_count].valid = true;
            new_route_count++;
        }
    }

    // Check if routing table changed
    bool changed = (new_route_count != s_route_count);
    if (!changed) {
        for (int i = 0; i < new_route_count; i++) {
            if (memcmp(new_routes[i].destination, s_routing_table[i].destination, ESP_NOW_ETH_ALEN) != 0 ||
                memcmp(new_routes[i].next_hop, s_routing_table[i].next_hop, ESP_NOW_ETH_ALEN) != 0 ||
                new_routes[i].hop_count != s_routing_table[i].hop_count) {
                changed = true;
                break;
            }
        }
    }

    if (changed) {
        xSemaphoreTake(s_routing_mutex, portMAX_DELAY);
        memcpy(s_routing_table, new_routes, sizeof(route_entry_t) * new_route_count);
        s_route_count = new_route_count;
        xSemaphoreGive(s_routing_mutex);

        ESP_LOGI(TAG, "Routing table updated (total routes: %d)", s_route_count);
        routing_print_table();
    }
}

void topology_handle_packet(const espnow_evt_recv_t *evt)
{
    const espnow_packet_t *pkt = &evt->packet;
    if (pkt->header.type != ESPNOW_PKT_TOPOLOGY) return;

    const topology_payload_t *topo = &pkt->u.topology;
    const uint8_t *sender = topo->sender_mac;

    uint8_t local_mac[ESP_NOW_ETH_ALEN];
    espnow_manager_get_local_mac(local_mac);

    // Ignore topology packets from local node
    if (memcmp(sender, local_mac, ESP_NOW_ETH_ALEN) == 0) return;

    int free_idx = -1;
    int found_idx = -1;
    int64_t now_us = esp_timer_get_time();

    for (int i = 0; i < MAX_TOPOLOGY_NODES; i++) {
        if (s_topology_db[i].valid && memcmp(s_topology_db[i].node_mac, sender, ESP_NOW_ETH_ALEN) == 0) {
            found_idx = i;
            break;
        }
        if (!s_topology_db[i].valid && free_idx == -1) {
            free_idx = i;
        }
    }

    int idx = (found_idx >= 0) ? found_idx : free_idx;
    if (idx < 0) {
        ESP_LOGW(TAG, "Topology DB full, dropping packet from " MACSTR, MAC2STR(sender));
        return;
    }

    if (found_idx >= 0) {
        if (topo->sequence <= s_topology_db[idx].sequence &&
            (s_topology_db[idx].sequence - topo->sequence) < 0x7FFFFFFF) {
            return;
        }
    }

    memcpy(s_topology_db[idx].node_mac, sender, ESP_NOW_ETH_ALEN);
    s_topology_db[idx].sequence = topo->sequence;
    s_topology_db[idx].neighbor_count = (topo->neighbor_count > MAX_TOPOLOGY_NEIGHBORS) ? MAX_TOPOLOGY_NEIGHBORS : topo->neighbor_count;
    for (int k = 0; k < s_topology_db[idx].neighbor_count; k++) {
        memcpy(s_topology_db[idx].neighbors[k], topo->neighbors[k], ESP_NOW_ETH_ALEN);
    }
    s_topology_db[idx].last_seen_us = now_us;
    s_topology_db[idx].valid = true;

    topology_recalculate_routes();
}

static void topology_task(void *arg)
{
    (void)arg;
    static uint32_t s_topo_seq = 0;
    uint8_t broadcast_mac[ESP_NOW_ETH_ALEN] = ESPNOW_BROADCAST_MAC;

    ESP_LOGI(TAG, "Topology task started (interval=%d ms)", TOPOLOGY_ADV_INTERVAL_MS);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(TOPOLOGY_ADV_INTERVAL_MS));

        espnow_packet_t pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.header.type = ESPNOW_PKT_TOPOLOGY;
        pkt.header.version = ESPNOW_PROTOCOL_VERSION;
        pkt.header.length = sizeof(espnow_header_t) + sizeof(topology_payload_t);

        espnow_manager_get_local_mac(pkt.u.topology.sender_mac);
        pkt.u.topology.sequence = ++s_topo_seq;

        uint8_t accepted_macs[MAX_TOPOLOGY_NEIGHBORS][ESP_NOW_ETH_ALEN];
        int count = neighbor_get_accepted_macs(accepted_macs, MAX_TOPOLOGY_NEIGHBORS);
        pkt.u.topology.neighbor_count = count;
        for (int i = 0; i < count; i++) {
            memcpy(pkt.u.topology.neighbors[i], accepted_macs[i], ESP_NOW_ETH_ALEN);
        }

        espnow_send_tag_t tag = {
            .pkt_type = ESPNOW_PKT_TOPOLOGY,
            .sequence = s_topo_seq,
        };
        memcpy(tag.dest_mac, broadcast_mac, ESP_NOW_ETH_ALEN);

        espnow_manager_ensure_peer(broadcast_mac);
        espnow_manager_send(broadcast_mac, &pkt, &tag);

        topology_recalculate_routes();
    }
}

void topology_start(void)
{
    xTaskCreate(topology_task, "topology", 4096, NULL, 3, NULL);
}
