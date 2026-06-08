#pragma once

#include "mesh_espnow.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_idf_version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 *  Logging
 *============================================================================*/

#define MESH_LOG_TAG "mesh"
#define ROUTE_TAG    "mesh_route"
#define RELIABLE_TAG "mesh_rel"
#define POWER_TAG    "mesh_pwr"
#define SEC_TAG      "mesh_sec"
#define DIAG_TAG     "mesh_diag"

extern mesh_espnow_log_level_t g_mesh_log;
extern mesh_espnow_log_level_t g_route_log;
extern mesh_espnow_log_level_t g_reliable_log;
extern mesh_espnow_log_level_t g_power_log;
extern mesh_espnow_log_level_t g_sec_log;
extern mesh_espnow_log_level_t g_diag_log;

#define MESH_LOG(level, tag, fmt, ...) do { \
    if (level <= g_mesh_log) ESP_LOG_LEVEL(level, tag, fmt, ##__VA_ARGS__); \
} while(0)

#define ROUTE_LOG(level, fmt, ...) do { \
    if (level <= g_route_log) ESP_LOG_LEVEL(level, ROUTE_TAG, fmt, ##__VA_ARGS__); \
} while(0)

#define RELIABLE_LOG(level, fmt, ...) do { \
    if (level <= g_reliable_log) ESP_LOG_LEVEL(level, RELIABLE_TAG, fmt, ##__VA_ARGS__); \
} while(0)

#define POWER_LOG(level, fmt, ...) do { \
    if (level <= g_power_log) ESP_LOG_LEVEL(level, POWER_TAG, fmt, ##__VA_ARGS__); \
} while(0)

#define SEC_LOG(level, fmt, ...) do { \
    if (level <= g_sec_log) ESP_LOG_LEVEL(level, SEC_TAG, fmt, ##__VA_ARGS__); \
} while(0)

#define DIAG_LOG(level, fmt, ...) do { \
    if (level <= g_diag_log) ESP_LOG_LEVEL(level, DIAG_TAG, fmt, ##__VA_ARGS__); \
} while(0)

/*============================================================================
 *  Protocol constants
 *============================================================================*/

#define MESH_PROTO_VER      0x03
#define MESH_BEACON_PORT    0x4D45  /* "ME" */
#define MESH_DATA_PORT      0x4441  /* "DA" */

/* Packet types */
typedef enum {
    PKT_BEACON      = 0x01,
    PKT_DATA        = 0x02,
    PKT_DATA_ACK    = 0x03,
    PKT_RREQ        = 0x04,
    PKT_RREP        = 0x05,
    PKT_BROADCAST   = 0x06,
    PKT_GOODBYE     = 0x07,
} pkt_type_t;

/*============================================================================
 *  Packet header (on-wire)
 *============================================================================*/

typedef struct __attribute__((packed)) {
    uint8_t  proto_ver;
    uint8_t  type;
    uint8_t  ttl;
    uint32_t src_id;
    uint32_t dest_id;
    uint32_t seqno;
    uint32_t ack_seqno;
    uint16_t payload_len;
    uint8_t  flags;
    uint16_t subnet_id;   /* Source's subnet (0 = global) */
} mesh_espnow_header_t;

#define MESH_HEADER_SIZE sizeof(mesh_espnow_header_t)
#define MESH_MIC_TAG_LEN  8
#define MESH_PACKET_MAX  (MESH_HEADER_SIZE + MESH_ESPNOW_MAX_PAYLOAD_LEN + MESH_MIC_TAG_LEN)
#define MESH_PAYLOAD_MAX MESH_ESPNOW_MAX_PAYLOAD_LEN

#define MESH_FLAG_RREQ         0x01
#define MESH_FLAG_RREP         0x02
#define MESH_FLAG_ACK          0x04
#define MESH_FLAG_CROSS_SUBNET 0x08  /* packet has crossed subnets (loop prevention) */
#define MESH_FLAG_ENC          0x80

#define MESH_NVS_NAMESPACE "mesh_espnow"
#define MESH_NVS_KEY_BOOT  "boot_count"
#define MESH_NVS_KEY_CRASH "crash_count"
#define MESH_NVS_KEY_NODE  "node_id"
#define MESH_NVS_KEY_CFG   "saved_cfg"

/*============================================================================
 *  Internal state
 *============================================================================*/

typedef struct {
    /* Identity */
    uint8_t  mac[MESH_ESPNOW_ADDR_LEN];

    /* Configuration */
    mesh_espnow_config_t config;

    /* State machine */
    mesh_espnow_state_t state;
    bool started;

    /* Synchronization */
    SemaphoreHandle_t mutex;

    /* Timers */
    uint64_t start_time_us;
    uint32_t last_beacon_ms;
    uint32_t last_process_ms;

    /* ESP-NOW */
    bool espnow_inited;
    bool wifi_inited;

    /* Error handling */
    esp_err_t last_err;
    char last_err_msg[64];

    /* Diagnostics */
    mesh_espnow_stats_t stats;
    bool stats_dirty;

    /* Battery */
    uint32_t battery_mv;

    /* NVS handles */
    nvs_handle_t nvs_handle;
    bool nvs_opened;
} mesh_espnow_ctx_t;

extern mesh_espnow_ctx_t g_mesh;

/*============================================================================
 *  Internal API prototypes
 *============================================================================*/

/* mesh_core.c */
esp_err_t mesh_core_mutex_lock(void);
esp_err_t mesh_core_mutex_unlock(void);
bool      mesh_core_is_initialized(void);
bool      mesh_core_is_started(void);
void      mesh_core_enter_error_state(esp_err_t err, const char *msg);
void      mesh_core_transition_to(mesh_espnow_state_t new_state);
int8_t    mesh_core_rssi_to_quality(int8_t rssi);
bool      mesh_core_is_gateway(void);
bool      mesh_core_addr_is_broadcast(const uint8_t *mac);
void      mesh_core_mac_to_node_id(const uint8_t *mac, uint32_t *id);
void      mesh_core_node_id_to_mac(uint32_t id, uint8_t *mac);
esp_err_t mesh_core_espnow_send(const uint8_t *mac, const uint8_t *data, size_t len);
esp_err_t mesh_core_send_packet(uint32_t dest_id, const uint8_t *payload, uint16_t payload_len,
                                uint8_t type, uint32_t ack_seqno, uint8_t flags);
esp_err_t mesh_core_send_packet_to(uint32_t dest_id, uint32_t next_hop, const uint8_t *payload,
                                    uint16_t payload_len, uint8_t type, uint32_t ack_seqno, uint8_t flags);
void      mesh_core_handle_data(const uint8_t *mac, const uint8_t *data, int len);

/* mesh_routing.c — intelligent metric-based routing */
esp_err_t mesh_routing_init(void);
void      mesh_routing_process(uint32_t now_ms);
void      mesh_routing_add_neighbor(uint32_t node_id, uint8_t hops, uint8_t caps, int8_t rssi, uint8_t subnet_id, uint8_t subnet_channel);
void      mesh_routing_remove_neighbor(uint32_t node_id);
void      mesh_routing_update_route(uint32_t dest, uint32_t next_hop, uint8_t hops, int8_t rssi);
bool      mesh_routing_find_route(uint32_t dest, uint32_t *next_hop, uint8_t *hops, int8_t *rssi);
uint32_t  mesh_routing_get_gateway(void);
uint32_t  mesh_routing_get_parent(void);
esp_err_t mesh_routing_discover_route(uint32_t dest_id);
void      mesh_routing_handle_rreq(uint32_t src, uint32_t orig, uint32_t seqno, uint8_t ttl, int8_t rssi);
void      mesh_routing_handle_rrep(uint32_t src, uint32_t dest, uint32_t seqno, uint8_t hops, int8_t rssi);
void      mesh_routing_handle_beacon(uint32_t node_id, uint8_t caps, uint8_t hops, uint8_t gateway, uint32_t uptime_s, int8_t rssi, uint8_t subnet_id, uint8_t subnet_channel);
void      mesh_routing_handle_battery_info(uint32_t node_id, uint32_t mv);
void      mesh_routing_record_tx_success(uint32_t neighbor_id);
void      mesh_routing_record_tx_failure(uint32_t neighbor_id);
uint32_t  mesh_routing_get_route_repairs(void);
uint32_t  mesh_routing_get_route_switches(void);
uint16_t  mesh_routing_neighbor_count(void);
uint16_t  mesh_routing_route_count(void);
int8_t    mesh_routing_avg_rssi(void);
uint8_t   mesh_routing_avg_hops(void);
void      mesh_routing_get_table(mesh_espnow_route_t *entries, uint16_t *count, uint16_t max);
void      mesh_routing_get_neighbors(mesh_espnow_neighbor_t *entries, uint16_t *count, uint16_t max);
void      mesh_routing_deinit(void);

/* mesh_reliable.c */
esp_err_t mesh_reliable_init(void);
void      mesh_reliable_process(uint32_t now_ms);
esp_err_t mesh_reliable_send(uint32_t dest_id, const uint16_t *payload, uint16_t len);
void      mesh_reliable_handle_ack(uint32_t src, uint32_t ack_seqno);
void      mesh_reliable_deinit(void);

/*============================================================================
 *  Bridge data types
 *============================================================================*/

typedef struct {
    uint8_t  subnet_id;
    uint8_t  channel;
    uint32_t dest_id;
    uint8_t  data[MESH_PACKET_MAX];
    uint16_t len;
} bridge_queue_entry_t;

typedef struct {
    uint8_t  subnet_id;
    uint8_t  channel;
} bridge_subnet_t;

/* mesh_bridge.c */
esp_err_t mesh_bridge_init(void);
void      mesh_bridge_process(uint32_t now_ms);
esp_err_t mesh_bridge_enqueue(uint8_t dest_subnet, uint32_t dest_id,
                               const uint8_t *data, uint16_t len);
esp_err_t mesh_bridge_add_subnet(uint8_t subnet_id, uint8_t channel);
esp_err_t mesh_bridge_remove_subnet(uint8_t subnet_id);
void      mesh_bridge_deinit(void);

/* mesh_power.c */
esp_err_t mesh_power_init(void);
void      mesh_power_process(uint32_t now_ms);
bool      mesh_power_should_sleep(void);
void      mesh_power_update_battery(uint32_t mv);
void      mesh_power_deinit(void);

/* mesh_security.c */
esp_err_t mesh_security_init(const uint8_t *key, size_t key_len);
esp_err_t mesh_security_encrypt(uint8_t *data, size_t *len);
esp_err_t mesh_security_decrypt(uint8_t *data, size_t *len);
void      mesh_security_deinit(void);

/* mesh_diag.c */
esp_err_t mesh_diag_init(void);
void      mesh_diag_process(uint32_t now_ms);
void      mesh_diag_scan(void);
void      mesh_diag_deinit(void);
void      mesh_diag_write_boot_count(void);
void      mesh_diag_write_crash_count(void);

#ifdef __cplusplus
}
#endif
