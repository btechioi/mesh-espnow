/* Sensor node example — demonstrates all features of mesh_espnow library */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "mesh_espnow.h"

static const char *TAG = "sensor_node";

/* Forward decl */
static void on_data(uint32_t src, const uint8_t *data, uint16_t len, int8_t rssi);
static void on_broadcast(uint32_t src, const uint8_t *data, uint16_t len);
static void on_node_discovered(uint32_t node_id, int8_t rssi);
static void on_node_lost(uint32_t node_id);
static void on_network_joined(uint32_t gateway_id);
static void on_network_lost(void);

void app_main(void) {
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

    cfg.node_id = 0;                         /* auto-generate from MAC */
    cfg.gateway_mode = false;                /* not a gateway */
    cfg.channel = 6;                         /* match your gateway */
    cfg.capabilities = MESH_ESPNOW_CAP_LEAF | MESH_ESPNOW_CAP_STORE_FWD;
    cfg.power_mode = MESH_ESPNOW_POWER_DUTY_CYCLE;
    cfg.beacon_interval_ms = 5000;
    cfg.max_retransmits = 3;
    cfg.encryption_enabled = true;

    /* Set up callbacks */
    cfg.callbacks.on_data = on_data;
    cfg.callbacks.on_broadcast = on_broadcast;
    cfg.callbacks.on_node_discovered = on_node_discovered;
    cfg.callbacks.on_node_lost = on_node_lost;
    cfg.callbacks.on_network_joined = on_network_joined;
    cfg.callbacks.on_network_lost = on_network_lost;

    /* Set custom network key (all nodes must match!) */
    uint8_t my_key[16] = "MyMeshKey1234!";
    memcpy(cfg.pre_shared_key, my_key, 16);

    /* Validate config (optional — init does this too) */
    const char *err = NULL;
    ret = mesh_espnow_validate_config(&cfg, &err);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Config error: %s", err);
        return;
    }

    /* Initialize mesh */
    ESP_ERROR_CHECK(mesh_espnow_init(&cfg));
    ESP_LOGI(TAG, "Node ID: 0x%08X", mesh_espnow_get_node_id());

    /* Start mesh */
    ESP_ERROR_CHECK(mesh_espnow_start());

    /* Report battery periodically (simulated) */
    uint32_t battery_mv = 3700;

    /* Main loop */
    uint32_t counter = 0;
    while (1) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        mesh_espnow_process(now_ms);

        /* Simulate battery drain */
        if (counter % 20 == 0) {
            mesh_espnow_update_battery(battery_mv);
        }

        /* Send a sensor reading every 30 seconds */
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

        /* Diagnostic scan every 5 minutes */
        if (counter % 3000 == 0 && counter > 0) {
            mesh_espnow_diagnostic_scan();
        }

        counter++;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/*============================================================================
 *  Callbacks
 *============================================================================*/

static void on_data(uint32_t src, const uint8_t *data, uint16_t len, int8_t rssi) {
    ESP_LOGI(TAG, "Data from 0x%08X [RSSI=%d]: %.*s", src, rssi, len, (const char *)data);
}

static void on_broadcast(uint32_t src, const uint8_t *data, uint16_t len) {
    ESP_LOGI(TAG, "Broadcast from 0x%08X: %.*s", src, len, (const char *)data);
}

static void on_node_discovered(uint32_t node_id, int8_t rssi) {
    ESP_LOGI(TAG, "New neighbor: 0x%08X (RSSI=%d)", node_id, rssi);
}

static void on_node_lost(uint32_t node_id) {
    ESP_LOGI(TAG, "Neighbor lost: 0x%08X", node_id);
}

static void on_network_joined(uint32_t gateway_id) {
    ESP_LOGI(TAG, "Joined mesh! Gateway: 0x%08X", gateway_id);
}

static void on_network_lost(void) {
    ESP_LOGW(TAG, "Lost connection to mesh network");
}
