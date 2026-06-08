/*
 * 01_sensor_node — Battery-powered leaf node
 *
 * Reports simulated sensor readings to the gateway every 30s.
 * Uses DUTY_CYCLE power mode for battery efficiency.
 *
 * Cross-platform: Arduino IDE | ESP-IDF | PlatformIO
 */

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
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

static const char *TAG = "sensor";

static void on_data(uint32_t src, const uint8_t *data, uint16_t len, int8_t rssi);
static void on_broadcast(uint32_t src, const uint8_t *data, uint16_t len);
static void on_node_discovered(uint32_t node_id, int8_t rssi);
static void on_node_lost(uint32_t node_id);
static void on_network_joined(uint32_t gateway_id);
static void on_network_lost(void);

void setup(void) {
    ESP_LOGI(TAG, "Sensor node starting...");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    mesh_espnow_config_t cfg = MESH_ESPNOW_CONFIG_DEFAULT();
    cfg.node_id              = 0;
    cfg.gateway_mode         = false;
    cfg.channel              = 6;
    cfg.capabilities         = MESH_ESPNOW_CAP_LEAF | MESH_ESPNOW_CAP_STORE_FWD;
    cfg.power_mode           = MESH_ESPNOW_POWER_DUTY_CYCLE;
    cfg.beacon_interval_ms   = 5000;
    cfg.max_retransmits      = 3;
    cfg.encryption_enabled   = true;

    uint8_t key[16] = "MyMeshKey1234!";
    memcpy(cfg.pre_shared_key, key, 16);

    cfg.callbacks.on_data            = on_data;
    cfg.callbacks.on_broadcast       = on_broadcast;
    cfg.callbacks.on_node_discovered = on_node_discovered;
    cfg.callbacks.on_node_lost       = on_node_lost;
    cfg.callbacks.on_network_joined  = on_network_joined;
    cfg.callbacks.on_network_lost    = on_network_lost;

    const char *err = NULL;
    ret = mesh_espnow_validate_config(&cfg, &err);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Config: %s", err);
        return;
    }
    ESP_ERROR_CHECK(mesh_espnow_init(&cfg));
    ESP_LOGI(TAG, "Node ID: 0x%08X", mesh_espnow_get_node_id());
    ESP_ERROR_CHECK(mesh_espnow_start());
}

static uint32_t s_counter;
static uint32_t s_battery_mv = 3700;

void loop(void) {
    uint32_t now = TIME_MS();
    mesh_espnow_process(now);

    if (s_counter % 20 == 0) {
        if (s_battery_mv > 3000) s_battery_mv -= 1;
        mesh_espnow_update_battery(s_battery_mv);
    }

    if (s_counter % 300 == 0 && s_counter > 0) {
        char msg[80];
        snprintf(msg, sizeof(msg),
                 "{\"node\":\"0x%08X\",\"temp\":%.1f,\"humidity\":%.1f,\"batt\":%u}",
                 mesh_espnow_get_node_id(),
                 22.5f + (float)(esp_random() % 100) / 100.0f,
                 55.0f + (float)(esp_random() % 100) / 100.0f,
                 s_battery_mv);
        esp_err_t ret = mesh_espnow_send_to_gateway((uint8_t *)msg, strlen(msg));
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Data sent to gateway");
        } else {
            ESP_LOGW(TAG, "Send: %s", mesh_espnow_err_to_str(ret));
        }
    }

    if (s_counter % 3000 == 0 && s_counter > 0) {
        mesh_espnow_diagnostic_scan();
    }

    s_counter++;
    WAIT_MS(100);
}

static void on_data(uint32_t src, const uint8_t *data, uint16_t len, int8_t rssi) {
    ESP_LOGI(TAG, "Data from 0x%08X [%ddBm]: %.*s", src, rssi, len, (const char *)data);
}

static void on_broadcast(uint32_t src, const uint8_t *data, uint16_t len) {
    ESP_LOGI(TAG, "Broadcast from 0x%08X: %.*s", src, len, (const char *)data);
}

static void on_node_discovered(uint32_t node_id, int8_t rssi) {
    ESP_LOGI(TAG, "New neighbor: 0x%08X (%ddBm)", node_id, rssi);
}

static void on_node_lost(uint32_t node_id) {
    ESP_LOGI(TAG, "Neighbor lost: 0x%08X", node_id);
}

static void on_network_joined(uint32_t gateway_id) {
    ESP_LOGI(TAG, "Joined mesh via gateway 0x%08X", gateway_id);
}

static void on_network_lost(void) {
    ESP_LOGW(TAG, "Lost connection to mesh");
}

#if !defined(ARDUINO) && !defined(ESP_ARDUINO)
void app_main(void) { setup(); while (1) { loop(); } }
#endif
