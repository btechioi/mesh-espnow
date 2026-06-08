/*
  ESP-NOW Mesh - Gateway Node

  Mains-powered root node with frequent beacons. Demonstrates:
  - gateway_mode = true
  - MESH_ESPNOW_POWER_ALWAYS_ON
  - Fixed node ID
  - Route change tracking
  - Periodic stats logging
*/

#include <esp_log.h>
#include "mesh_espnow.h"

static const char *TAG = "gateway";

void on_data(uint32_t src, const uint8_t *data, uint16_t len, int8_t rssi) {
    ESP_LOGI(TAG, "Received from 0x%08X [%ddBm]: %.*s", src, rssi, len, (const char *)data);
}

void on_node_discovered(uint32_t node_id, int8_t rssi) {
    ESP_LOGI(TAG, "Node joined mesh: 0x%08X (RSSI=%d)", node_id, rssi);
}

void on_node_lost(uint32_t node_id) {
    ESP_LOGI(TAG, "Node left mesh: 0x%08X", node_id);
}

void on_route_changed(uint32_t dest, uint32_t next_hop, uint8_t hops) {
    ESP_LOGD(TAG, "Route: 0x%08X -> 0x%08X (%u hops)", dest, next_hop, hops);
}

void setup() {
    ESP_LOGI(TAG, "Mesh gateway node starting...");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    mesh_espnow_config_t cfg = MESH_ESPNOW_CONFIG_DEFAULT();

    cfg.node_id = 0x01000001;
    cfg.gateway_mode = true;
    cfg.channel = 6;
    cfg.capabilities = MESH_ESPNOW_CAP_GATEWAY | MESH_ESPNOW_CAP_ROUTER;
    cfg.power_mode = MESH_ESPNOW_POWER_ALWAYS_ON;
    cfg.beacon_interval_ms = 1000;
    cfg.max_neighbors = 64;
    cfg.max_routes = 128;

    cfg.callbacks.on_data = on_data;
    cfg.callbacks.on_node_discovered = on_node_discovered;
    cfg.callbacks.on_node_lost = on_node_lost;
    cfg.callbacks.on_route_changed = on_route_changed;

    ESP_ERROR_CHECK(mesh_espnow_init(&cfg));
    ESP_LOGI(TAG, "Gateway ID: 0x%08X (fixed)", mesh_espnow_get_node_id());
    ESP_ERROR_CHECK(mesh_espnow_start());
}

uint32_t counter = 0;

void loop() {
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    mesh_espnow_process(now_ms);

    if (counter % 500 == 0) {
        mesh_espnow_stats_t stats;
        mesh_espnow_get_stats(&stats);
        ESP_LOGI(TAG, "Stats: %u neighbors, %u routes, fwd=%u, dropped=%u",
                 stats.neighbor_count, stats.route_count,
                 stats.forwarded, stats.dropped);
    }

    if (counter % 6000 == 0 && counter > 0) {
        mesh_espnow_diagnostic_scan();
    }

    counter++;
    delay(100);
}
