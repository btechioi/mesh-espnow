/* mesh_diag.c: Health monitoring, boot tracking, diagnostics */

#include "mesh_priv.h"

static const char *TAG = "mesh_diag";

static struct {
    uint32_t last_health_check_ms;
    uint32_t health_check_interval_ms;
    uint32_t boot_count;
    uint32_t crash_count;
    uint32_t rtc_boot_count;
    uint32_t rtc_crash_count;
} s_diag;

/* RTC memory for crash recovery */
static RTC_DATA_ATTR uint32_t s_rtc_boot_count = 0;
static RTC_DATA_ATTR uint32_t s_rtc_crash_count = 0;

/*============================================================================
 *  Init / Deinit
 *============================================================================*/

esp_err_t mesh_diag_init(void) {
    s_diag.health_check_interval_ms = 30000; /* every 30s */
    s_diag.last_health_check_ms = 0;

    /* Read boot count from NVS */
    s_diag.boot_count = 1;
    s_diag.crash_count = 0;

    if (g_mesh.nvs_opened) {
        uint32_t val = 0;
        if (nvs_get_u32(g_mesh.nvs_handle, MESH_NVS_KEY_BOOT, &val) == ESP_OK) {
            s_diag.boot_count = val + 1;
        }
        if (nvs_get_u32(g_mesh.nvs_handle, MESH_NVS_KEY_CRASH, &val) == ESP_OK) {
            s_diag.crash_count = val;
        }
    }

    /* Check RTC for crash detection */
    if (s_rtc_boot_count > 0) {
        /* Previous boot recorded: if boot count did not increment, it was a crash */
        if (s_diag.boot_count <= s_rtc_boot_count) {
            s_diag.crash_count++;
            s_diag.rtc_crash_count = s_diag.crash_count;
            DIAG_LOG(ESP_LOG_WARN, "Crash detected! Total: %u", s_diag.crash_count);

            if (g_mesh.nvs_opened) {
                nvs_set_u32(g_mesh.nvs_handle, MESH_NVS_KEY_CRASH, s_diag.crash_count);
                nvs_commit(g_mesh.nvs_handle);
            }
        }
    }

    s_rtc_boot_count = s_diag.boot_count;

    /* Persist boot count */
    if (g_mesh.nvs_opened && g_mesh.config.enable_health_monitor) {
        nvs_set_u32(g_mesh.nvs_handle, MESH_NVS_KEY_BOOT, s_diag.boot_count);
        nvs_commit(g_mesh.nvs_handle);
    }

    g_mesh.stats.boot_count = s_diag.boot_count;
    g_mesh.stats.crash_count = s_diag.crash_count;

    DIAG_LOG(ESP_LOG_INFO, "Boot #%u, crashes: %u", s_diag.boot_count, s_diag.crash_count);
    return ESP_OK;
}

void mesh_diag_deinit(void) {
    /* Save boot count on clean shutdown */
    s_diag.rtc_boot_count = s_diag.boot_count;
}

/*============================================================================
 *  Periodic health check
 *============================================================================*/

void mesh_diag_process(uint32_t now_ms) {
    if (!g_mesh.config.enable_health_monitor) return;

    if (now_ms - s_diag.last_health_check_ms >= s_diag.health_check_interval_ms) {
        s_diag.last_health_check_ms = now_ms;
        g_mesh.stats_dirty = true;

        /* Update heap */
        g_mesh.stats.heap_free = (uint32_t)esp_get_free_heap_size();

        /* Minimal health log */
        uint32_t uptime_min = g_mesh.stats.uptime_ms / 60000;
        uint16_t neighbors = mesh_routing_neighbor_count();
        uint16_t routes    = mesh_routing_route_count();

        DIAG_LOG(ESP_LOG_INFO, "Health: up=%umin nb=%u rt=%u heap=%uKB rssi=%d",
                 uptime_min, neighbors, routes,
                 g_mesh.stats.heap_free / 1024,
                 mesh_routing_avg_rssi());
    }
}

/*============================================================================
 *  Diagnostic scan (full dump)
 *============================================================================*/

void mesh_diag_scan(void) {
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t uptime_ms = now_ms;

    mesh_espnow_stats_t stats;
    mesh_espnow_get_stats(&stats);

    printf("\n"
           "========================================\n"
           "  MESH DIAGNOSTIC SCAN\n"
           "========================================\n"
           "  Node ID:      0x%08X\n"
           "  State:        %s\n"
           "  Uptime:       %u ms\n"
           "  Boot count:   %u\n"
           "  Crashes:      %u\n"
           "  Mode:         %s\n"
           "  Gateway:      %s\n"
           "  Gateway ID:   0x%08X\n"
           "  Parent ID:    0x%08X\n"
           "  Battery:      %u mV\n"
           "  Heap Free:    %u bytes\n"
           "----------------------------------------\n"
           "  Neighbors:    %u\n"
           "  Routes:       %u\n"
           "  Avg RSSI:     %d dBm\n"
           "  Avg Hops:     %u\n"
           "----------------------------------------\n"
           "  TX packets:   %u\n"
           "  TX bytes:     %u\n"
           "  RX packets:   %u\n"
           "  RX bytes:     %u\n"
           "  Forwarded:    %u\n"
           "  Dropped:      %u\n"
           "  Retransmits:  %u\n"
           "  Duplicates:   %u\n"
           "  RREQs sent:   %u\n"
           "  RREPs recv:   %u\n"
           "----------------------------------------\n"
           "  Avg latency:  %.1f ms\n"
           "  Peak latency: %u ms\n"
           "========================================\n",
           g_mesh.config.node_id,
           mesh_espnow_state_str(g_mesh.state),
           uptime_ms,
           s_diag.boot_count,
           s_diag.crash_count,
           mesh_espnow_power_mode_str(g_mesh.config.power_mode),
           g_mesh.config.gateway_mode ? "YES (self)" : (stats.gateway_id ? "yes" : "no"),
           stats.gateway_id,
           stats.parent_id,
           g_mesh.battery_mv,
           stats.heap_free,
           stats.neighbor_count,
           stats.route_count,
           stats.avg_rssi,
           stats.avg_hop_count,
           stats.tx_packets, stats.tx_bytes,
           stats.rx_packets, stats.rx_bytes,
           stats.forwarded, stats.dropped,
           stats.retransmissions,
           stats.duplicates_detected,
           stats.rreqs_sent,
           stats.rreps_received,
           (double)stats.avg_tx_latency_ms,
           stats.peak_tx_latency_ms
    );
}

/*============================================================================
 *  Boot/crash counter helpers
 *============================================================================*/

void mesh_diag_write_boot_count(void) {
    if (g_mesh.nvs_opened) {
        nvs_set_u32(g_mesh.nvs_handle, MESH_NVS_KEY_BOOT, s_diag.boot_count);
        nvs_commit(g_mesh.nvs_handle);
    }
}

void mesh_diag_write_crash_count(void) {
    if (g_mesh.nvs_opened) {
        nvs_set_u32(g_mesh.nvs_handle, MESH_NVS_KEY_CRASH, s_diag.crash_count);
        nvs_commit(g_mesh.nvs_handle);
    }
}
