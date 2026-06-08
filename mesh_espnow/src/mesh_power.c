/* mesh_power.c — Power management with duty cycling and deep sleep */

#include "mesh_priv.h"

static const char *TAG = "mesh_pwr";

static struct {
    uint32_t battery_mv;
    uint32_t awake_until_ms;
    bool     is_awake;
    uint32_t last_check_ms;
} s_power;

esp_err_t mesh_power_init(void) {
    s_power.battery_mv = 0;
    s_power.awake_until_ms = 0;
    s_power.is_awake = true;
    s_power.last_check_ms = 0;

    POWER_LOG(ESP_LOG_INFO, "Power mode: %s",
              mesh_espnow_power_mode_str(g_mesh.config.power_mode));
    return ESP_OK;
}

void mesh_power_deinit(void) {
    /* Nothing to clean up */
}

void mesh_power_update_battery(uint32_t mv) {
    s_power.battery_mv = mv;
    POWER_LOG(ESP_LOG_DEBUG, "Battery: %u mV", mv);
}

void mesh_power_process(uint32_t now_ms) {
    if (g_mesh.config.power_mode == MESH_ESPNOW_POWER_ALWAYS_ON) {
        s_power.is_awake = true;
        return;
    }

    /* Only evaluate every 100ms to avoid busy-looping */
    if (now_ms - s_power.last_check_ms < 100) return;
    s_power.last_check_ms = now_ms;

    if (g_mesh.config.power_mode == MESH_ESPNOW_POWER_DUTY_CYCLE) {
        if (s_power.is_awake) {
            if (now_ms >= s_power.awake_until_ms) {
                s_power.is_awake = false;
                POWER_LOG(ESP_LOG_DEBUG, "Duty cycle: sleep");
                /* Enter modem-sleep for the rest of the interval */
                esp_wifi_set_ps(WIFI_PS_MODEM);
            }
        } else {
            /* Wake up at next beacon interval */
            if (now_ms - g_mesh.last_beacon_ms >= g_mesh.config.beacon_interval_ms / 2) {
                s_power.is_awake = true;
                s_power.awake_until_ms = now_ms + g_mesh.config.awake_window_ms;
                esp_wifi_set_ps(WIFI_PS_NONE);
                POWER_LOG(ESP_LOG_DEBUG, "Duty cycle: awake");
            }
        }
    }
}

bool mesh_power_should_sleep(void) {
    if (g_mesh.config.power_mode == MESH_ESPNOW_POWER_DEEP_SLEEP) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        return (now_ms - g_mesh.last_beacon_ms >= g_mesh.config.beacon_interval_ms);
    }
    /* ON_DEMAND: never auto-sleep — application calls mesh_espnow_sleep() explicitly */
    return false;
}
