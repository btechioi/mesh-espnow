/*
  ESP-NOW Mesh - Sensor Node

  Battery-powered leaf node that reports simulated temperature/humidity
  to gateway every 30 seconds. Demonstrates:
  - Callbacks: on_data, on_broadcast, on_node_discovered, on_node_lost,
    on_network_joined, on_network_lost
  - DUTY_CYCLE power mode
  - mesh_espnow_send_to_gateway()
  - mesh_espnow_update_battery()
  - mesh_espnow_diagnostic_scan()
*/

#include <esp_log.h>
#include <esp_random.h>
#include "mesh_espnow.h"

static const char *TAG = "sensor_node";

void on_data(uint32_t src, const uint8_t *data, uint16_t len, int8_t rssi) {
    ESP_LOGI(TAG, "Data from 0x%08X [RSSI=%d]: %.*s", src, rssi, len, (const char *)data);
}

void on_broadcast(uint32_t src, const uint8_t *data, uint16_t len) {
    ESP_LOGI(TAG, "Broadcast from 0x%08X: %.*s", src, len, (const char *)data);
}

void on_node_discovered(uint32_t node_id, int8_t rssi) {
    ESP_LOGI(TAG, "New neighbor: 0x%08X (RSSI=%d)", node_id, rssi);
}

void on_node_lost(uint32_t node_id) {
    ESP_LOGI(TAG, "Neighbor lost: 0x%08X", node_id);
}

void on_network_joined(uint32_t gateway_id) {
    ESP_LOGI(TAG, "Joined mesh! Gateway: 0x%08X", gateway_id);
}

void on_network_lost(void) {
    ESP_LOGW(TAG, "Lost connection to mesh network");
}

void setup() {
    ESP_LOGI(TAG, "Mesh sensor node starting...");

    /* Init NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Configure mesh */
    mesh_espnow_config_t cfg = MESH_ESPNOW_CONFIG_DEFAULT();

    cfg.node_id = 0;
    cfg.gateway_mode = false;
    cfg.channel = 6;
    cfg.capabilities = MESH_ESPNOW_CAP_LEAF | MESH_ESPNOW_CAP_STORE_FWD;
    cfg.power_mode = MESH_ESPNOW_POWER_DUTY_CYCLE;
    cfg.beacon_interval_ms = 5000;
    cfg.max_retransmits = 3;
    cfg.encryption_enabled = true;

    cfg.callbacks.on_data = on_data;
    cfg.callbacks.on_broadcast = on_broadcast;
    cfg.callbacks.on_node_discovered = on_node_discovered;
    cfg.callbacks.on_node_lost = on_node_lost;
    cfg.callbacks.on_network_joined = on_network_joined;
    cfg.callbacks.on_network_lost = on_network_lost;

    ESP_ERROR_CHECK(mesh_espnow_init(&cfg));
    ESP_LOGI(TAG, "Node ID: 0x%08X", mesh_espnow_get_node_id());
    ESP_ERROR_CHECK(mesh_espnow_start());
}

uint32_t counter = 0;
uint32_t battery_mv = 3700;

void loop() {
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    mesh_espnow_process(now_ms);

    if (counter % 20 == 0) {
        mesh_espnow_update_battery(battery_mv);
    }

    if (counter % 300 == 0 && counter > 0) {
        char msg[64];
        snprintf(msg, sizeof(msg),
                 "{\"node\":\"0x%08X\",\"temp\":%.1f,\"humidity\":%.1f,\"batt\":%u}",
                 mesh_espnow_get_node_id(),
                 22.5f + (float)(esp_random() % 100) / 100.0f,
                 55.0f + (float)(esp_random() % 100) / 100.0f,
                 battery_mv);
        esp_err_t ret = mesh_espnow_send_to_gateway((uint8_t *)msg, strlen(msg));
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Sensor data sent to gateway");
        } else {
            ESP_LOGW(TAG, "Send failed: %s", mesh_espnow_err_to_str(ret));
        }
    }

    counter++;
    delay(100);
}
