/*
  ESP-NOW Mesh - 03_auto_mesh

  Fully Automatic Mesh with Self-Electing Root.

  Nodes automatically discover each other and elect a root based on
  capability and uptime — no manual gateway configuration required.

  Root Election Protocol:
    - Each node computes a root_score from its capabilities
    - Nodes broadcast CANDIDACY on join and every 30s
    - Highest score wins; tiebreaker = lowest node_id
    - Elected root broadcasts HEARTBEAT every 10s
    - 3 missed heartbeats → re-election
    - Higher-score nodes appearing later trigger handover

  All 8 mesh callbacks are demonstrated.
*/

#include <esp_log.h>
#include <esp_random.h>
#include "mesh_espnow.h"

static const char *TAG = "auto_mesh";

/*============================================================================
 *  Application protocol message types
 *============================================================================*/
#define APP_MSG_CANDIDACY    0x01
#define APP_MSG_HEARTBEAT    0x02
#define APP_MSG_SURRENDER    0x03
#define APP_MSG_SENSOR_DATA  0x04

/*--------------------------------------------------------------------------*/

#define ROOT_SCORE_GATEWAY   1000
#define ROOT_SCORE_ROUTER     500
#define ROOT_SCORE_LEAF        50
#define ROOT_SCORE_UPTIME_BONUS_MAX  100

#define HEARTBEAT_INTERVAL_MS   10000
#define HEARTBEAT_TIMEOUT_MS    35000
#define CANDIDACY_INTERVAL_MS   30000
#define SENSOR_INTERVAL_MS      30000
#define DIAG_INTERVAL_MS       300000

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
    s_candidates[idx].last_seen_ms  = millis();
    s_candidates[idx].is_root       = (node_id == s_elected_root_id);
}

/*============================================================================
 *  Root score
 *============================================================================*/

static uint16_t compute_root_score(uint8_t caps) {
    uint16_t score = ROOT_SCORE_LEAF;
    if (caps & MESH_ESPNOW_CAP_GATEWAY) score = ROOT_SCORE_GATEWAY;
    else if (caps & MESH_ESPNOW_CAP_ROUTER && !(caps & MESH_ESPNOW_CAP_LEAF))
        score = ROOT_SCORE_ROUTER;
    uint32_t uptime_s = millis() / 1000;
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

    if (my_score > best_score || (my_score == best_score && my_id < best_id)) {
        best_score = my_score;
        best_id    = my_id;
    }

    if (best_id != s_elected_root_id) {
        s_elected_root_id = best_id;
        s_last_heartbeat_ms = millis();
        ESP_LOGI(TAG, "Elected root: 0x%08X (score=%u)", best_id, best_score);

        s_is_elected_root = (best_id == my_id);
        if (s_is_elected_root) {
            s_heartbeat_seqno = 0;
            ESP_LOGI(TAG, ">>> I AM THE ROOT <<<");
        }
    }
}

/*============================================================================
 *  Send application messages
 *============================================================================*/

static void send_candidacy(void) {
    uint16_t score = compute_root_score(MESH_ESPNOW_CAP_ROUTER);
    uint32_t uptime_s = millis() / 1000;
    uint32_t my_id = mesh_espnow_get_node_id();

    uint8_t buf[32];
    size_t n = 0;
    buf[n++] = APP_MSG_CANDIDACY;
    memcpy(buf + n, &my_id, 4);   n += 4;
    memcpy(buf + n, &score, 2);   n += 2;
    buf[n++] = MESH_ESPNOW_CAP_ROUTER;
    memcpy(buf + n, &uptime_s, 4); n += 4;

    esp_err_t ret = mesh_espnow_broadcast(buf, n);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Candidacy failed: %s", mesh_espnow_err_to_str(ret));
    }
}

static void send_heartbeat(void) {
    uint32_t my_id = mesh_espnow_get_node_id();
    uint8_t buf[16];
    size_t n = 0;
    buf[n++] = APP_MSG_HEARTBEAT;
    memcpy(buf + n, &my_id, 4);          n += 4;
    memcpy(buf + n, &s_heartbeat_seqno, 4); n += 4;
    s_heartbeat_seqno++;

    esp_err_t ret = mesh_espnow_broadcast(buf, n);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Heartbeat failed: %s", mesh_espnow_err_to_str(ret));
    }
}

static void send_sensor_data(void) {
    uint32_t my_id = mesh_espnow_get_node_id();
    float temp = 20.0f + (float)(esp_random() % 200) / 10.0f;
    float hum  = 40.0f + (float)(esp_random() % 400) / 10.0f;

    mesh_espnow_stats_t stats;
    uint32_t battery_mv = 0;
    if (mesh_espnow_get_stats(&stats) == ESP_OK) {
        battery_mv = stats.battery_mv;
    }

    uint8_t buf[64];
    size_t n = 0;
    buf[n++] = APP_MSG_SENSOR_DATA;
    memcpy(buf + n, &my_id, 4);          n += 4;
    memcpy(buf + n, &s_sensor_seqno, 4); n += 4;
    memcpy(buf + n, &temp, 4);           n += 4;
    memcpy(buf + n, &hum, 4);            n += 4;
    memcpy(buf + n, &battery_mv, 4);     n += 4;

    if (s_elected_root_id && s_elected_root_id != my_id) {
        esp_err_t ret = mesh_espnow_send(s_elected_root_id, buf, n, NULL);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Sensor data #%u to root 0x%08X", s_sensor_seqno, s_elected_root_id);
        } else {
            ESP_LOGW(TAG, "Sensor send: %s", mesh_espnow_err_to_str(ret));
        }
    } else if (s_elected_root_id == my_id) {
        ESP_LOGI(TAG, "Sensor data #%u (self): temp=%.1f hum=%.1f", s_sensor_seqno, temp, hum);
    }
    s_sensor_seqno++;
}

/*============================================================================
 *  Handle incoming app messages
 *============================================================================*/

static void handle_app_message(uint32_t src, const uint8_t *data, uint16_t len) {
    if (len < 1) return;

    switch (data[0]) {

    case APP_MSG_CANDIDACY: {
        if (len < 11) return;
        uint32_t node_id;      memcpy(&node_id,  data + 1, 4);
        uint16_t score;        memcpy(&score,    data + 5, 2);
        uint8_t  caps = data[7];
        uint32_t uptime_s;     memcpy(&uptime_s, data + 8, 4);
        upsert_candidate(node_id, score, caps, uptime_s);

        uint32_t my_id = mesh_espnow_get_node_id();
        if (!s_is_elected_root && node_id != my_id) {
            uint16_t my_score = compute_root_score(MESH_ESPNOW_CAP_ROUTER);
            if (my_score > score) send_candidacy();
        }
        run_election();
        break;
    }

    case APP_MSG_HEARTBEAT: {
        if (len < 9) return;
        uint32_t root_id;      memcpy(&root_id, data + 1, 4);
        if (root_id == s_elected_root_id) {
            s_last_heartbeat_ms = millis();
        } else {
            upsert_candidate(root_id, ROOT_SCORE_ROUTER, 0, 0);
            s_elected_root_id = root_id;
            s_last_heartbeat_ms = millis();
            if (root_id != mesh_espnow_get_node_id()) s_is_elected_root = false;
        }
        break;
    }

    case APP_MSG_SURRENDER: {
        if (len < 9) return;
        uint32_t old_root, new_root;
        memcpy(&old_root, data + 1, 4);
        memcpy(&new_root, data + 5, 4);
        if (old_root == s_elected_root_id) {
            upsert_candidate(new_root, ROOT_SCORE_ROUTER, 0, 0);
            s_elected_root_id = new_root;
            run_election();
        }
        break;
    }

    case APP_MSG_SENSOR_DATA: {
        if (len < 21) return;
        uint32_t node_id, seqno, batt;
        float temp, hum;
        memcpy(&node_id, data + 1, 4);
        memcpy(&seqno,   data + 5, 4);
        memcpy(&temp,    data + 9, 4);
        memcpy(&hum,     data + 13, 4);
        memcpy(&batt,    data + 17, 4);
        ESP_LOGI(TAG, "Data from 0x%08X [#%u]: temp=%.1f hum=%.1f batt=%umV",
                 node_id, seqno, temp, hum, batt);
        break;
    }
    }
}

/*============================================================================
 *  Callbacks
 *============================================================================*/

void on_data(uint32_t src, const uint8_t *data, uint16_t len, int8_t rssi) {
    handle_app_message(src, data, len);
}

void on_broadcast(uint32_t src, const uint8_t *data, uint16_t len) {
    handle_app_message(src, data, len);
}

void on_node_discovered(uint32_t node_id, int8_t rssi) {
    ESP_LOGI(TAG, "Discovered: 0x%08X (RSSI=%d)", node_id, rssi);
    send_candidacy();
}

void on_node_lost(uint32_t node_id) {
    ESP_LOGI(TAG, "Lost: 0x%08X", node_id);
    int idx = find_candidate(node_id);
    if (idx >= 0) s_candidates[idx] = s_candidates[--s_candidate_count];
    if (node_id == s_elected_root_id) {
        ESP_LOGW(TAG, "Root 0x%08X disappeared — re-electing", node_id);
        s_elected_root_id = 0;
        run_election();
    }
}

void on_network_joined(uint32_t gateway_id) {
    ESP_LOGI(TAG, "Network joined via gateway 0x%08X", gateway_id);
}

void on_network_lost(void) {
    ESP_LOGW(TAG, "Network connection lost");
}

void on_route_changed(uint32_t dest, uint32_t next_hop, uint8_t hops) {
    ESP_LOGD(TAG, "Route: 0x%08X → 0x%08X (%u hops)", dest, next_hop, hops);
}

void on_fatal_error(esp_err_t err, const char *msg) {
    ESP_LOGE(TAG, "FATAL: %s (%s)", msg, esp_err_to_name(err));
}

/*============================================================================
 *  Arduino setup / loop
 *============================================================================*/

void setup() {
    ESP_LOGI(TAG, "Auto-Mesh node starting...");

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
    cfg.capabilities         = MESH_ESPNOW_CAP_ROUTER;
    cfg.power_mode           = MESH_ESPNOW_POWER_ALWAYS_ON;
    cfg.beacon_interval_ms   = 3000;
    cfg.max_neighbors        = 32;
    cfg.max_routes           = 64;
    cfg.max_retransmits      = 3;
    cfg.encryption_enabled   = true;

    cfg.callbacks.on_data            = on_data;
    cfg.callbacks.on_broadcast       = on_broadcast;
    cfg.callbacks.on_node_discovered = on_node_discovered;
    cfg.callbacks.on_node_lost       = on_node_lost;
    cfg.callbacks.on_network_joined  = on_network_joined;
    cfg.callbacks.on_network_lost    = on_network_lost;
    cfg.callbacks.on_route_changed   = on_route_changed;
    cfg.callbacks.on_fatal_error     = on_fatal_error;

    ESP_ERROR_CHECK(mesh_espnow_init(&cfg));
    ESP_LOGI(TAG, "Node ID: 0x%08X", mesh_espnow_get_node_id());
    ESP_ERROR_CHECK(mesh_espnow_start());

    srand((unsigned)(esp_timer_get_time() + cfg.channel));
}

void loop() {
    uint32_t now_ms = millis();
    mesh_espnow_process(now_ms);

    /* Periodic candidacy */
    if (now_ms - s_last_candidacy_ms >= CANDIDACY_INTERVAL_MS) {
        send_candidacy();
        s_last_candidacy_ms = now_ms;
    }

    /* Heartbeat timeout check */
    if (s_elected_root_id && s_elected_root_id != mesh_espnow_get_node_id()) {
        if (now_ms - s_last_heartbeat_ms >= HEARTBEAT_TIMEOUT_MS) {
            ESP_LOGW(TAG, "Heartbeat timeout for root 0x%08X", s_elected_root_id);
            s_elected_root_id = 0;
            run_election();
        }
    }

    /* Root heartbeat */
    if (s_is_elected_root) {
        if (now_ms - s_last_heartbeat_ms >= HEARTBEAT_INTERVAL_MS) {
            send_heartbeat();
            s_last_heartbeat_ms = now_ms;
        }
    }

    /* Sensor data */
    if (now_ms - s_last_sensor_ms >= SENSOR_INTERVAL_MS) {
        send_sensor_data();
        s_last_sensor_ms = now_ms;
    }

    /* Diagnostics */
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

    delay(100);
}
