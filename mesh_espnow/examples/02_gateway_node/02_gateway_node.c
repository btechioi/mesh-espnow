/*
 * 02_gateway_node: Mains-powered mesh root / gateway
 *
 * Root node with frequent beacons. Receives and logs data from all
 * sensor nodes. Demonstrates route-change tracking and periodic stats.
 *
 * Cross-platform: Arduino IDE | ESP-IDF | PlatformIO
 */

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "mesh_espnow.h"

#if defined(ARDUINO) || defined(ESP_ARDUINO)
#include <Arduino.h>
#define TIME_MS()  millis()
#define WAIT_MS(m) delay(m)
#else
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#define TIME_MS()  ((uint32_t)(esp_timer_get_time() / 1000))
#define WAIT_MS(m) vTaskDelay(pdMS_TO_TICKS(m))
#endif

static const char *TAG = "gateway";

static void on_data(uint32_t src, const uint8_t *data, uint16_t len, int8_t rssi);
static void on_node_discovered(uint32_t node_id, int8_t rssi);
static void on_node_lost(uint32_t node_id);
static void on_route_changed(uint32_t dest, uint32_t next_hop, uint8_t hops);

void setup(void) {
    ESP_LOGI(TAG, "Gateway node starting...");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    mesh_espnow_config_t cfg = MESH_ESPNOW_CONFIG_DEFAULT();
    cfg.node_id              = 0x01000001;
    cfg.gateway_mode         = true;
    cfg.channel              = 6;
    cfg.capabilities         = MESH_ESPNOW_CAP_GATEWAY | MESH_ESPNOW_CAP_ROUTER;
    cfg.power_mode           = MESH_ESPNOW_POWER_ALWAYS_ON;
    cfg.beacon_interval_ms   = 1000;
    cfg.max_neighbors        = 64;
    cfg.max_routes           = 128;

    cfg.callbacks.on_data            = on_data;
    cfg.callbacks.on_node_discovered = on_node_discovered;
    cfg.callbacks.on_node_lost       = on_node_lost;
    cfg.callbacks.on_route_changed   = on_route_changed;

    const char *err = NULL;
    ret = mesh_espnow_validate_config(&cfg, &err);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Config: %s", err);
        return;
    }
    ESP_ERROR_CHECK(mesh_espnow_init(&cfg));
    ESP_LOGI(TAG, "Gateway ID: 0x%08X  channel=%d",
             mesh_espnow_get_node_id(), cfg.channel);
    ESP_ERROR_CHECK(mesh_espnow_start());
}

static uint32_t s_counter;

void loop(void) {
    uint32_t now = TIME_MS();
    mesh_espnow_process(now);

    if (s_counter % 500 == 0) {
        mesh_espnow_stats_t stats;
        mesh_espnow_get_stats(&stats);
        ESP_LOGI(TAG, "Stats: %u neighbors, %u routes, fwd=%u, dropped=%u",
                 stats.neighbor_count, stats.route_count,
                 stats.forwarded, stats.dropped);
    }

    if (s_counter % 6000 == 0 && s_counter > 0) {
        mesh_espnow_diagnostic_scan();
    }

    s_counter++;
    WAIT_MS(100);
}

static void on_data(uint32_t src, const uint8_t *data, uint16_t len, int8_t rssi) {
    ESP_LOGI(TAG, "Received from 0x%08X [%ddBm]: %.*s", src, rssi, len, (const char *)data);
}

static void on_node_discovered(uint32_t node_id, int8_t rssi) {
    ESP_LOGI(TAG, "Node joined: 0x%08X (%ddBm)", node_id, rssi);
}

static void on_node_lost(uint32_t node_id) {
    ESP_LOGI(TAG, "Node left: 0x%08X", node_id);
}

static void on_route_changed(uint32_t dest, uint32_t next_hop, uint8_t hops) {
    ESP_LOGD(TAG, "Route: 0x%08X -> 0x%08X (%u hops)", dest, next_hop, hops);
}

#if !defined(ARDUINO) && !defined(ESP_ARDUINO)
void app_main(void) { setup(); while (1) { loop(); } }
#endif
