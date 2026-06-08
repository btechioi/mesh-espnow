/*
 * 03_auto_mesh — Fully automatic mesh with self-electing root
 *
 * Nodes auto-discover each other and elect a root based on capability
 * and uptime. No manual gateway configuration needed.
 *
 * Root election protocol (application-layer):
 *   - Nodes broadcast CANDIDACY with their root_score on join & every 30s
 *   - Highest score wins; tiebreaker = lowest node_id
 *   - Elected root broadcasts HEARTBEAT every 10s
 *   - 3 missed heartbeats → re-election
 *   - Higher-score nodes appearing later trigger immediate handover
 *
 * All 8 mesh callbacks are demonstrated.
 *
 * Cross-platform: Arduino IDE | ESP-IDF | PlatformIO
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
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

static const char *TAG = "auto_mesh";

/*============================================================================
 *  Application protocol
 *============================================================================*/
#define MSG_CANDIDACY   0x01
#define MSG_HEARTBEAT   0x02
#define MSG_SURRENDER   0x03
#define MSG_SENSOR      0x04

#define SCORE_GATEWAY    1000
#define SCORE_ROUTER      500
#define SCORE_LEAF         50
#define SCORE_UPTIME_MAX  100

#define HEARTBEAT_IVL_MS  10000
#define HEARTBEAT_TO_MS   35000
#define CANDIDACY_IVL_MS  30000
#define SENSOR_IVL_MS     30000
#define DIAG_IVL_MS      300000
#define CANDIDATES_MAX      16

/*============================================================================
 *  State
 *============================================================================*/
typedef struct {
    uint32_t node_id;
    uint16_t root_score;
    uint8_t  caps;
    uint32_t uptime_s;
    uint32_t last_seen_ms;
} candidate_t;

static candidate_t s_cands[CANDIDATES_MAX];
static int         s_cand_cnt;
static uint32_t    s_root_id;
static bool        s_im_root;
static uint32_t    s_hb_last_ms;
static uint32_t    s_cand_last_ms;
static uint32_t    s_sensor_last_ms;
static uint32_t    s_diag_last_ms;
static uint32_t    s_hb_seq;
static uint32_t    s_sensor_seq;

/*============================================================================
 *  Candidate helpers
 *============================================================================*/
static int find_cand(uint32_t id) {
    for (int i = 0; i < s_cand_cnt; i++)
        if (s_cands[i].node_id == id) return i;
    return -1;
}

static void upsert_cand(uint32_t id, uint16_t score, uint8_t caps, uint32_t up) {
    int i = find_cand(id);
    if (i < 0) {
        if (s_cand_cnt >= CANDIDATES_MAX) return;
        i = s_cand_cnt++;
        s_cands[i].node_id = id;
    }
    s_cands[i].root_score  = score;
    s_cands[i].caps        = caps;
    s_cands[i].uptime_s    = up;
    s_cands[i].last_seen_ms = TIME_MS();
}

/*============================================================================
 *  Score & election
 *============================================================================*/
static uint16_t my_score(void) {
    uint32_t t = TIME_MS() / 1000;
    uint16_t b = (uint16_t)(t / 3600);
    if (b > SCORE_UPTIME_MAX) b = SCORE_UPTIME_MAX;
    return SCORE_ROUTER + b;
}

static void elect(void) {
    uint32_t best_id = mesh_espnow_get_node_id();
    uint16_t best_scr = my_score();

    for (int i = 0; i < s_cand_cnt; i++) {
        uint16_t sc = s_cands[i].root_score;
        if (sc > best_scr || (sc == best_scr && s_cands[i].node_id < best_id)) {
            best_scr = sc;
            best_id  = s_cands[i].node_id;
        }
    }

    if (best_id != s_root_id) {
        uint32_t old = s_root_id;
        s_root_id = best_id;
        s_hb_last_ms = TIME_MS();
        if (old) {
            ESP_LOGI(TAG, "Elected: 0x%08X (score=%u) was 0x%08X",
                     best_id, best_scr, old);
        } else {
            ESP_LOGI(TAG, "Elected: 0x%08X (score=%u)", best_id, best_scr);
        }
        s_im_root = (best_id == mesh_espnow_get_node_id());
        if (s_im_root) { s_hb_seq = 0; ESP_LOGI(TAG, ">>> I AM ROOT <<<"); }
    }
}

/*============================================================================
 *  Send
 *============================================================================*/
static void send_candidacy(void) {
    uint32_t id = mesh_espnow_get_node_id();
    uint16_t sc = my_score();
    uint32_t up = TIME_MS() / 1000;

    uint8_t buf[16]; size_t n = 0;
    buf[n++] = MSG_CANDIDACY;
    memcpy(buf + n, &id, 4); n += 4;
    memcpy(buf + n, &sc, 2); n += 2;
    buf[n++] = MESH_ESPNOW_CAP_ROUTER;
    memcpy(buf + n, &up, 4); n += 4;
    mesh_espnow_broadcast(buf, n);
}

static void send_heartbeat(void) {
    uint32_t id = mesh_espnow_get_node_id();
    uint8_t buf[12]; size_t n = 0;
    buf[n++] = MSG_HEARTBEAT;
    memcpy(buf + n, &id, 4);        n += 4;
    memcpy(buf + n, &s_hb_seq, 4);  n += 4;
    s_hb_seq++;
    mesh_espnow_broadcast(buf, n);
}

static void send_sensor(void) {
    uint32_t id = mesh_espnow_get_node_id();
    float temp = 20.0f + (float)(esp_random() % 200) / 10.0f;
    float hum  = 40.0f + (float)(esp_random() % 400) / 10.0f;

    mesh_espnow_stats_t st;
    uint32_t batt = 0;
    if (mesh_espnow_get_stats(&st) == ESP_OK) batt = st.battery_mv;

    uint8_t buf[48]; size_t n = 0;
    buf[n++] = MSG_SENSOR;
    memcpy(buf + n, &id, 4);           n += 4;
    memcpy(buf + n, &s_sensor_seq, 4); n += 4;
    memcpy(buf + n, &temp, 4);         n += 4;
    memcpy(buf + n, &hum, 4);          n += 4;
    memcpy(buf + n, &batt, 4);         n += 4;

    if (s_root_id && s_root_id != id) {
        esp_err_t e = mesh_espnow_send(s_root_id, buf, n, NULL);
        if (e == ESP_OK)
            ESP_LOGI(TAG, "Data #%u -> root 0x%08X", s_sensor_seq, s_root_id);
    } else if (s_im_root) {
        ESP_LOGI(TAG, "Data #%u (self): %.1fC %.1f%%", s_sensor_seq, temp, hum);
    }
    s_sensor_seq++;
}

/*============================================================================
 *  Receive
 *============================================================================*/
static void handle_msg(uint32_t src, const uint8_t *d, uint16_t len) {
    if (len < 1) return;
    switch (d[0]) {

    case MSG_CANDIDACY: {
        if (len < 11) return;
        uint32_t id;  memcpy(&id,  d + 1, 4);
        uint16_t scr; memcpy(&scr, d + 5, 2);
        uint8_t  c = d[7];
        uint32_t up;  memcpy(&up,  d + 8, 4);
        upsert_cand(id, scr, c, up);
        if (!s_im_root) {
            uint16_t m = my_score();
            if (m > scr) send_candidacy();
        }
        elect();
        break;
    }

    case MSG_HEARTBEAT: {
        if (len < 9) return;
        uint32_t rid; memcpy(&rid, d + 1, 4);
        if (rid == s_root_id) {
            s_hb_last_ms = TIME_MS();
        } else {
            upsert_cand(rid, SCORE_ROUTER, 0, 0);
            s_root_id = rid;
            s_hb_last_ms = TIME_MS();
            s_im_root = (rid == mesh_espnow_get_node_id());
        }
        break;
    }

    case MSG_SURRENDER: {
        if (len < 9) return;
        uint32_t old, new;
        memcpy(&old, d + 1, 4); memcpy(&new, d + 5, 4);
        if (old == s_root_id) {
            upsert_cand(new, SCORE_ROUTER, 0, 0);
            s_root_id = new;
            elect();
        }
        break;
    }

    case MSG_SENSOR: {
        if (len < 21) return;
        uint32_t id, seq, batt; float temp, hum;
        memcpy(&id,   d + 1, 4); memcpy(&seq,  d + 5, 4);
        memcpy(&temp, d + 9, 4); memcpy(&hum,  d + 13, 4);
        memcpy(&batt, d + 17, 4);
        ESP_LOGI(TAG, "Data from 0x%08X [#%u]: %.1fC %.1f%% %umV",
                 id, seq, temp, hum, batt);
        break;
    }

    default: break;
    }
}

/*============================================================================
 *  Callbacks
 *============================================================================*/
static void cb_data(uint32_t src, const uint8_t *d, uint16_t len, int8_t r) {
    handle_msg(src, d, len);
}
static void cb_bcast(uint32_t src, const uint8_t *d, uint16_t len) {
    handle_msg(src, d, len);
}
static void cb_discover(uint32_t id, int8_t rssi) {
    ESP_LOGI(TAG, "+ 0x%08X (%ddBm)", id, rssi);
    send_candidacy();
}
static void cb_lost(uint32_t id) {
    ESP_LOGI(TAG, "- 0x%08X", id);
    int i = find_cand(id);
    if (i >= 0) s_cands[i] = s_cands[--s_cand_cnt];
    if (id == s_root_id) {
        ESP_LOGW(TAG, "Root gone — re-electing");
        s_root_id = 0; elect();
    }
}
static void cb_joined(uint32_t gid) {
    ESP_LOGI(TAG, "Mesh joined via 0x%08X", gid);
}
static void cb_lost_net(void) {
    ESP_LOGW(TAG, "Network lost");
}
static void cb_route(uint32_t d, uint32_t n, uint8_t h) {
    ESP_LOGD(TAG, "Route: 0x%08X -> 0x%08X (%u)", d, n, h);
}
static void cb_fatal(esp_err_t err, const char *msg) {
    ESP_LOGE(TAG, "FATAL: %s", msg);
}

/*============================================================================
 *  Setup & loop
 *============================================================================*/
void setup(void) {
    ESP_LOGI(TAG, "Auto-mesh node starting...");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    mesh_espnow_config_t cfg = MESH_ESPNOW_CONFIG_DEFAULT();
    cfg.node_id            = 0;
    cfg.gateway_mode       = false;
    cfg.channel            = 6;
    cfg.capabilities       = MESH_ESPNOW_CAP_ROUTER;
    cfg.power_mode         = MESH_ESPNOW_POWER_ALWAYS_ON;
    cfg.beacon_interval_ms = 3000;
    cfg.encryption_enabled = true;

    cfg.callbacks.on_data            = cb_data;
    cfg.callbacks.on_broadcast       = cb_bcast;
    cfg.callbacks.on_node_discovered = cb_discover;
    cfg.callbacks.on_node_lost       = cb_lost;
    cfg.callbacks.on_network_joined  = cb_joined;
    cfg.callbacks.on_network_lost    = cb_lost_net;
    cfg.callbacks.on_route_changed   = cb_route;
    cfg.callbacks.on_fatal_error     = cb_fatal;

    const char *e = NULL;
    ret = mesh_espnow_validate_config(&cfg, &e);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "Config: %s", e); return; }
    ESP_ERROR_CHECK(mesh_espnow_init(&cfg));
    ESP_LOGI(TAG, "Node ID: 0x%08X", mesh_espnow_get_node_id());
    ESP_ERROR_CHECK(mesh_espnow_start());
}

void loop(void) {
    uint32_t now = TIME_MS();
    mesh_espnow_process(now);

    if (now - s_cand_last_ms >= CANDIDACY_IVL_MS) {
        send_candidacy(); s_cand_last_ms = now;
    }

    if (s_root_id && !s_im_root && now - s_hb_last_ms >= HEARTBEAT_TO_MS) {
        ESP_LOGW(TAG, "Heartbeat timeout — re-electing");
        s_root_id = 0; elect();
    }

    if (s_im_root && now - s_hb_last_ms >= HEARTBEAT_IVL_MS) {
        send_heartbeat(); s_hb_last_ms = now;
    }

    if (now - s_sensor_last_ms >= SENSOR_IVL_MS) {
        send_sensor(); s_sensor_last_ms = now;
    }

    if (now - s_diag_last_ms >= DIAG_IVL_MS) {
        mesh_espnow_stats_t st;
        if (mesh_espnow_get_stats(&st) == ESP_OK) {
            ESP_LOGI(TAG, "State=%s  Nb=%u  Rt=%u  TX=%u  RX=%u  Fwd=%u  Drop=%u",
                     mesh_espnow_state_str(mesh_espnow_get_state()),
                     st.neighbor_count, st.route_count,
                     st.tx_packets, st.rx_packets,
                     st.forwarded, st.dropped);
        }
        ESP_LOGI(TAG, "Root: 0x%08X  I_am_root=%d  Candidates=%d",
                 s_root_id, s_im_root, s_cand_cnt);
        s_diag_last_ms = now;
    }

    WAIT_MS(100);
}

#if !defined(ARDUINO) && !defined(ESP_ARDUINO)
void app_main(void) { setup(); while (1) { loop(); } }
#endif
