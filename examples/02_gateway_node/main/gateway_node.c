/* Gateway node example — root of the mesh network with serial bridge */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "mesh_espnow.h"

static const char *TAG = "gateway";

static void on_data(uint32_t src, const uint8_t *data, uint16_t len, int8_t rssi);
static void on_node_discovered(uint32_t node_id, int8_t rssi);
static void on_node_lost(uint32_t node_id);
static void on_route_changed(uint32_t dest, uint32_t next_hop, uint8_t hops);

void app_main(void) {
    ESP_LOGI(TAG, "Mesh gateway node starting...");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Configure as gateway (root of mesh) */
    mesh_espnow_config_t cfg = MESH_ESPNOW_CONFIG_DEFAULT();

    cfg.node_id = 0x01000001;                  /* fixed ID for the gateway */
    cfg.gateway_mode = true;                   /* this is the root node */
    cfg.channel = 6;
    cfg.capabilities = MESH_ESPNOW_CAP_GATEWAY | MESH_ESPNOW_CAP_ROUTER;
    cfg.power_mode = MESH_ESPNOW_POWER_ALWAYS_ON; /* mains-powered */
    cfg.beacon_interval_ms = 1000;             /* frequent beacons */
    cfg.max_neighbors = 64;
    cfg.max_routes = 128;

    cfg.callbacks.on_data = on_data;
    cfg.callbacks.on_node_discovered = on_node_discovered;
    cfg.callbacks.on_node_lost = on_node_lost;
    cfg.callbacks.on_route_changed = on_route_changed;

    ESP_ERROR_CHECK(mesh_espnow_init(&cfg));
    ESP_LOGI(TAG, "Gateway ID: 0x%08X (fixed)", mesh_espnow_get_node_id());
    ESP_LOGI(TAG, "All nodes must set channel=%d and use the same network key", cfg.channel);

    ESP_ERROR_CHECK(mesh_espnow_start());

    uint32_t counter = 0;
    while (1) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        mesh_espnow_process(now_ms);

        /* Log network status periodically */
        if (counter % 500 == 0) {
            mesh_espnow_stats_t stats;
            mesh_espnow_get_stats(&stats);
            ESP_LOGI(TAG, "Stats: %u neighbors, %u routes, fwd=%u, dropped=%u",
                     stats.neighbor_count, stats.route_count,
                     stats.forwarded, stats.dropped);
        }

        /* Full diagnostic every 10 minutes */
        if (counter % 6000 == 0 && counter > 0) {
            mesh_espnow_diagnostic_scan();
        }

        counter++;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void on_data(uint32_t src, const uint8_t *data, uint16_t len, int8_t rssi) {
    ESP_LOGI(TAG, "Received from 0x%08X [%ddBm]: %.*s", src, rssi, len, (const char *)data);
    /* Forward to serial/UART, MQTT, HTTP, etc. */
}

static void on_node_discovered(uint32_t node_id, int8_t rssi) {
    ESP_LOGI(TAG, "Node joined mesh: 0x%08X (RSSI=%d)", node_id, rssi);
}

static void on_node_lost(uint32_t node_id) {
    ESP_LOGI(TAG, "Node left mesh: 0x%08X", node_id);
}

static void on_route_changed(uint32_t dest, uint32_t next_hop, uint8_t hops) {
    ESP_LOGD(TAG, "Route: 0x%08X → 0x%08X (%u hops)", dest, next_hop, hops);
}
