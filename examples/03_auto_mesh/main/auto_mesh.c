/*
 * 03_auto_mesh — Fully Automatic Mesh with Self-Electing Root
 *
 * Nodes automatically discover each other and elect a root based on
 * capability and uptime — no manual gateway config required.
 *
 * Root Election Protocol (application-layer):
 *   - Each node computes a root_score from its capabilities
 *   - Nodes broadcast CANDIDACY on join and when score changes
 *   - Highest score wins; tiebreaker = lowest node_id
 *   - Elected root broadcasts periodic HEARTBEAT
 *   - 3 missed heartbeats → re-election
 *   - Higher-score nodes that appear later trigger immediate handover
 *
 * Architecture:
 *   setup()     — NVS init, mesh init/start, register callbacks
 *   loop()      — mesh_espnow_process(), election logic, data tx
 *   callbacks   — all 8 mesh callbacks with application handling
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "mesh_espnow.h"

static const char *TAG = "auto_mesh";

/*============================================================================
 *  Application Protocol — message types inside broadcast payloads
 *============================================================================*/
#define APP_MSG_CANDIDACY    0x01   /* "I exist, here are my credentials" */
#define APP_MSG_HEARTBEAT    0x02   /* "I am the root, I'm alive" */
#define APP_MSG_SURRENDER    0x03   /* "Stepping down, new root is X" */
#define APP_MSG_SENSOR_DATA  0x04   /* Sensor reading addressed to root */

/*--------------------------------------------------------------------------*/

#define ROOT_SCORE_GATEWAY   1000   /* Explicit gateway config */
#define ROOT_SCORE_ROUTER     500   /* Mains-powered forwarder */
#define ROOT_SCORE_LEAF        50   /* Battery-powered leaf */
#define ROOT_SCORE_UPTIME_BONUS_MAX  100  /* +1 per hour, capped */

#define HEARTBEAT_INTERVAL_MS   10000  /* Root broadcasts every 10 s */
#define HEARTBEAT_TIMEOUT_MS    35000  /* 3 missed → re-election */
#define CANDIDACY_INTERVAL_MS   30000  /* Re-announce every 30 s */
#define SENSOR_INTERVAL_MS      30000  /* Send reading every 30 s */
#define DIAG_INTERVAL_MS       300000  /* Full dump every 5 min */

/*--------------------------------------------------------------------------*/

typedef struct {
    uint32_t node_id;
    uint16_t root_score;
    uint8_t  capabilities;
    uint32_t uptime_s;
    uint32_t last_seen_ms;
    bool     is_root;
} root_candidate_t;

#define MAX_CANDIDATES 16
static root_candidate_t s_candidates[MAX_CANDIDATES];
static int              s_candidate_count;
static uint32_t         s_elected_root_id;
static uint32_t         s_last_heartbeat_ms;
static uint32_t         s_last_candidacy_ms;
static uint32_t         s_last_sensor_ms;
static uint32_t         s_last_diag_ms;
static uint32_t         s_heartbeat_seqno;
static uint32_t         s_sensor_seqno;
static bool             s_is_elected_root;

/*============================================================================
 *  Candidate management
 *============================================================================*/

static int find_candidate(uint32_t node_id) {
    for (int i = 0; i < s_candidate_count; i++)
        if (s_candidates[i].node_id == node_id) return i;
    return -1;
}

static void upsert_candidate(uint32_t node_id, uint16_t score,
                              uint8_t caps, uint32_t uptime_s) {
    int idx = find_candidate(node_id);
    if (idx < 0) {
        if (s_candidate_count >= MAX_CANDIDATES) return;
        idx = s_candidate_count++;
        s_candidates[idx].node_id = node_id;
    }
    s_candidates[idx].root_score    = score;
    s_candidates[idx].capabilities  = caps;
    s_candidates[idx].uptime_s      = uptime_s;
    s_candidates[idx].last_seen_ms  = (uint32_t)(esp_timer_get_time() / 1000);
    s_candidates[idx].is_root       = (node_id == s_elected_root_id);
}

/*============================================================================
 *  Root score computation
 *============================================================================*/

static uint16_t compute_root_score(uint8_t caps) {
    uint16_t score = ROOT_SCORE_LEAF;
    if (caps & MESH_ESPNOW_CAP_GATEWAY) score = ROOT_SCORE_GATEWAY;
    else if (caps & MESH_ESPNOW_CAP_ROUTER && !(caps & MESH_ESPNOW_CAP_LEAF))
        score = ROOT_SCORE_ROUTER;
    /* Bonus: +1 per hour uptime, capped */
    uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    uint16_t bonus = (uint16_t)(uptime_s / 3600);
    if (bonus > ROOT_SCORE_UPTIME_BONUS_MAX)
        bonus = ROOT_SCORE_UPTIME_BONUS_MAX;
    return score + bonus;
}

/*============================================================================
 *  Election
 *============================================================================*/

static void run_election(void) {
    uint32_t best_id = 0;
    uint16_t best_score = 0;

    for (int i = 0; i < s_candidate_count; i++) {
        uint16_t s = s_candidates[i].root_score;
        if (s > best_score || (s == best_score && s_candidates[i].node_id < best_id)) {
            best_score = s;
            best_id    = s_candidates[i].node_id;
        }
    }

    uint32_t my_id = mesh_espnow_get_node_id();
    uint16_t my_score = compute_root_score(MESH_ESPNOW_CAP_ROUTER);

    /* Include self in election */
    if (my_score > best_score || (my_score == best_score && my_id < best_id)) {
        best_score = my_score;
        best_id    = my_id;
    }

    if (best_id != s_elected_root_id) {
        uint32_t old = s_elected_root_id;
        s_elected_root_id = best_id;
        s_last_heartbeat_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (old) {
            ESP_LOGI(TAG, "Elected root: 0x%08X (score=%u) (was 0x%08X)",
                     best_id, best_score, old);
        } else {
            ESP_LOGI(TAG, "Elected root: 0x%08X (score=%u)", best_id, best_score);
        }

        s_is_elected_root = (best_id == my_id);
        if (s_is_elected_root) {
            s_heartbeat_seqno = 0;
            ESP_LOGI(TAG, ">>> I AM THE ROOT <<<");
        }
    }
}

/*============================================================================
 *  Send application-level messages
 *============================================================================*/

static void send_candidacy(void) {
    uint8_t caps = MESH_ESPNOW_CAP_ROUTER;
    /* Mirror what we actually configured — get from mesh state */
    uint16_t score = compute_root_score(caps);
    uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    uint32_t my_id = mesh_espnow_get_node_id();

    uint8_t buf[32];
    size_t n = 0;
    buf[n++] = APP_MSG_CANDIDACY;
    memcpy(buf + n, &my_id, 4);   n += 4;
    memcpy(buf + n, &score, 2);   n += 2;
    buf[n++] = caps;
    memcpy(buf + n, &uptime_s, 4); n += 4;

    esp_err_t ret = mesh_espnow_broadcast(buf, n);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Candidacy broadcast failed: %s", mesh_espnow_err_to_str(ret));
    }
}

static void send_heartbeat(void) {
    uint32_t my_id = mesh_espnow_get_node_id();
    uint8_t buf[16];
    size_t n = 0;
    buf[n++] = APP_MSG_HEARTBEAT;
    memcpy(buf + n, &my_id, 4);      n += 4;
    memcpy(buf + n, &s_heartbeat_seqno, 4); n += 4;
    s_heartbeat_seqno++;

    esp_err_t ret = mesh_espnow_broadcast(buf, n);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Heartbeat failed: %s", mesh_espnow_err_to_str(ret));
    }
}

static void send_sensor_data(void) {
    uint32_t my_id = mesh_espnow_get_node_id();

    /* Simulated sensor readings */
    float temp = 20.0f + (float)(esp_random() % 200) / 10.0f;
    float hum  = 40.0f + (float)(esp_random() % 400) / 10.0f;

    uint32_t battery_mv = 0;
    mesh_espnow_stats_t stats;
    if (mesh_espnow_get_stats(&stats) == ESP_OK) {
        battery_mv = stats.battery_mv;
    }

    uint8_t buf[64];
    size_t n = 0;
    buf[n++] = APP_MSG_SENSOR_DATA;
    memcpy(buf + n, &my_id, 4);        n += 4;
    memcpy(buf + n, &s_sensor_seqno, 4); n += 4;
    memcpy(buf + n, &temp, 4);          n += 4;
    memcpy(buf + n, &hum, 4);           n += 4;
    memcpy(buf + n, &battery_mv, 4);    n += 4;

    if (s_elected_root_id && s_elected_root_id != my_id) {
        esp_err_t ret = mesh_espnow_send(s_elected_root_id, buf, n, NULL);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Sensor data #%u sent to root 0x%08X", s_sensor_seqno, s_elected_root_id);
        } else {
            ESP_LOGW(TAG, "Sensor data failed: %s", mesh_espnow_err_to_str(ret));
        }
    } else if (s_elected_root_id == my_id) {
        /* Root: just log locally */
        ESP_LOGI(TAG, "Sensor data #%u (self): temp=%.1f hum=%.1f", s_sensor_seqno, temp, hum);
    } else {
        ESP_LOGW(TAG, "No root elected yet, cannot send sensor data");
    }
    s_sensor_seqno++;
}

/*============================================================================
 *  Handle incoming application messages
 *============================================================================*/

static void handle_app_message(uint32_t src, const uint8_t *data, uint16_t len) {
    if (len < 1) return;

    switch (data[0]) {

    case APP_MSG_CANDIDACY: {
        if (len < 11) return;
        uint32_t node_id;      memcpy(&node_id,      data + 1,  4);
        uint16_t score;        memcpy(&score,        data + 5,  2);
        uint8_t  caps = data[7];
        uint32_t uptime_s;     memcpy(&uptime_s,     data + 8,  4);

        upsert_candidate(node_id, score, caps, uptime_s);
        ESP_LOGD(TAG, "Candidate 0x%08X score=%u caps=0x%02X uptime=%us",
                 node_id, score, caps, uptime_s);

        uint32_t my_id = mesh_espnow_get_node_id();
        if (!s_is_elected_root && node_id != my_id) {
            uint16_t my_score = compute_root_score(
                MESH_ESPNOW_CAP_ROUTER);
            if (my_score > score) {
                send_candidacy();
            }
        }
        run_election();
        break;
    }

    case APP_MSG_HEARTBEAT: {
        if (len < 9) return;
        uint32_t root_id;      memcpy(&root_id, data + 1, 4);
        /* uint32_t seqno; */  /* memcpy(&seqno, data + 5, 4); */

        if (root_id == s_elected_root_id) {
            s_last_heartbeat_ms = (uint32_t)(esp_timer_get_time() / 1000);
        } else {
            /* New root announced via heartbeat — update */
            upsert_candidate(root_id, ROOT_SCORE_ROUTER, 0, 0);
            s_elected_root_id = root_id;
            s_last_heartbeat_ms = (uint32_t)(esp_timer_get_time() / 1000);
            if (root_id != mesh_espnow_get_node_id()) {
                s_is_elected_root = false;
            }
        }
        break;
    }

    case APP_MSG_SURRENDER: {
        if (len < 9) return;
        uint32_t old_root;     memcpy(&old_root, data + 1, 4);
        uint32_t new_root;     memcpy(&new_root, data + 5, 4);

        if (old_root == s_elected_root_id) {
            ESP_LOGI(TAG, "Root 0x%08X surrendered to 0x%08X", old_root, new_root);
            upsert_candidate(new_root, ROOT_SCORE_ROUTER, 0, 0);
            s_elected_root_id = new_root;
            run_election();
        }
        break;
    }

    case APP_MSG_SENSOR_DATA: {
        if (len < 21) return;
        uint32_t node_id;      memcpy(&node_id,  data + 1, 4);
        uint32_t seqno;        memcpy(&seqno,    data + 5, 4);
        float    temp;         memcpy(&temp,     data + 9, 4);
        float    hum;          memcpy(&hum,      data + 13, 4);
        uint32_t batt;         memcpy(&batt,     data + 17, 4);

        ESP_LOGI(TAG, "Data from 0x%08X [#%u]: temp=%.1f hum=%.1f batt=%umV",
                 node_id, seqno, temp, hum, batt);
        break;
    }

    default:
        ESP_LOGD(TAG, "Unknown app msg type 0x%02X from 0x%08X", data[0], src);
        break;
    }
}

/*============================================================================
 *  Mesh callbacks
 *============================================================================*/

static void on_data(uint32_t src, const uint8_t *data, uint16_t len, int8_t rssi) {
    handle_app_message(src, data, len);
}

static void on_broadcast(uint32_t src, const uint8_t *data, uint16_t len) {
    handle_app_message(src, data, len);
}

static void on_node_discovered(uint32_t node_id, int8_t rssi) {
    ESP_LOGI(TAG, "Discovered: 0x%08X (RSSI=%d)", node_id, rssi);
    send_candidacy();
}

static void on_node_lost(uint32_t node_id) {
    ESP_LOGI(TAG, "Lost: 0x%08X", node_id);
    /* Remove from candidates */
    int idx = find_candidate(node_id);
    if (idx >= 0) {
        s_candidates[idx] = s_candidates[--s_candidate_count];
    }
    if (node_id == s_elected_root_id) {
        ESP_LOGW(TAG, "Root candidate 0x%08X disappeared — re-electing", node_id);
        s_elected_root_id = 0;
        run_election();
    }
}

static void on_network_joined(uint32_t gateway_id) {
    ESP_LOGI(TAG, "Network joined via gateway 0x%08X", gateway_id);
}

static void on_network_lost(void) {
    ESP_LOGW(TAG, "Network connection lost");
}

static void on_route_changed(uint32_t dest, uint32_t next_hop, uint8_t hops) {
    ESP_LOGD(TAG, "Route: 0x%08X → 0x%08X (%u hops)", dest, next_hop, hops);
}

static void on_fatal_error(esp_err_t err, const char *msg) {
    ESP_LOGE(TAG, "FATAL: %s (%s)", msg, esp_err_to_name(err));
}

/*============================================================================
 *  Setup & main loop
 *============================================================================*/

void setup(void) {
    ESP_LOGI(TAG, "Auto-Mesh node starting...");

    /* Init NVS (needed by mesh library) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Build config — no manual gateway, let election decide */
    mesh_espnow_config_t cfg = MESH_ESPNOW_CONFIG_DEFAULT();

    cfg.node_id              = 0;                     /* auto from MAC */
    cfg.gateway_mode         = false;                 /* election-based */
    cfg.channel              = 6;
    cfg.capabilities         = MESH_ESPNOW_CAP_ROUTER;/* change per node */
    cfg.power_mode           = MESH_ESPNOW_POWER_ALWAYS_ON;
    cfg.beacon_interval_ms   = 3000;
    cfg.max_neighbors        = 32;
    cfg.max_routes           = 64;
    cfg.max_retransmits      = 3;
    cfg.encryption_enabled   = true;

    /* All 8 callbacks */
    cfg.callbacks.on_data            = on_data;
    cfg.callbacks.on_broadcast       = on_broadcast;
    cfg.callbacks.on_node_discovered = on_node_discovered;
    cfg.callbacks.on_node_lost       = on_node_lost;
    cfg.callbacks.on_network_joined  = on_network_joined;
    cfg.callbacks.on_network_lost    = on_network_lost;
    cfg.callbacks.on_route_changed   = on_route_changed;
    cfg.callbacks.on_fatal_error     = on_fatal_error;

    /* Validate & init */
    const char *err_str = NULL;
    ret = mesh_espnow_validate_config(&cfg, &err_str);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Config rejected: %s", err_str);
        return;
    }

    ESP_ERROR_CHECK(mesh_espnow_init(&cfg));
    ESP_LOGI(TAG, "Node ID: 0x%08X", mesh_espnow_get_node_id());

    ESP_ERROR_CHECK(mesh_espnow_start());

    /* Seed RNG */
    srand((unsigned)(esp_timer_get_time() + cfg.channel));
}

void loop(void) {
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    mesh_espnow_process(now_ms);

    /* ---- Root election & maintenance ---- */

    /* Periodic candidacy broadcast */
    if (now_ms - s_last_candidacy_ms >= CANDIDACY_INTERVAL_MS) {
        send_candidacy();
        s_last_candidacy_ms = now_ms;
    }

    /* Check heartbeat timeout — trigger re-election */
    if (s_elected_root_id && s_elected_root_id != mesh_espnow_get_node_id()) {
        if (now_ms - s_last_heartbeat_ms >= HEARTBEAT_TIMEOUT_MS) {
            ESP_LOGW(TAG, "Heartbeat timeout for root 0x%08X — re-electing",
                     s_elected_root_id);
            s_elected_root_id = 0;
            run_election();
        }
    }

    /* If elected root: send heartbeat */
    if (s_is_elected_root) {
        if (now_ms - s_last_heartbeat_ms >= HEARTBEAT_INTERVAL_MS) {
            send_heartbeat();
            s_last_heartbeat_ms = now_ms;
        }
    }

    /* ---- Application data ---- */

    if (now_ms - s_last_sensor_ms >= SENSOR_INTERVAL_MS) {
        send_sensor_data();
        s_last_sensor_ms = now_ms;
    }

    /* ---- Diagnostics ---- */

    if (now_ms - s_last_diag_ms >= DIAG_INTERVAL_MS) {
        mesh_espnow_stats_t stats;
        if (mesh_espnow_get_stats(&stats) == ESP_OK) {
            ESP_LOGI(TAG,
                     "State=%s  Neighbors=%u  Routes=%u  "
                     "TX=%u  RX=%u  Fwd=%u  Drop=%u  Retx=%u",
                     mesh_espnow_state_str(mesh_espnow_get_state()),
                     stats.neighbor_count, stats.route_count,
                     stats.tx_packets, stats.rx_packets,
                     stats.forwarded, stats.dropped,
                     stats.retransmissions);
        }
        ESP_LOGI(TAG, "Elected root: 0x%08X  I_am_root=%d  Candidates=%d",
                 s_elected_root_id, s_is_elected_root, s_candidate_count);
        mesh_espnow_diagnostic_scan();
        s_last_diag_ms = now_ms;
    }
}

/*============================================================================
 *  ESP-IDF entry point
 *============================================================================*/
#ifndef ARDUINO
void app_main(void) {
    setup();
    while (1) {
        loop();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
#endif
