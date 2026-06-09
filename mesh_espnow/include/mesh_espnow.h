/**
 * @file mesh_espnow.h
 * @brief ESP-NOW Mesh Network Library for ESP32
 *
 * =============================================================================
 *  QUICK START (for people who don't read manuals)
 * =============================================================================
 *
 *  #include "mesh_espnow.h"
 *
 *  void app_main(void) {
 *      mesh_espnow_config_t cfg = MESH_ESPNOW_CONFIG_DEFAULT();
 *
 *      cfg.node_id = 0;           // 0 = auto from MAC
 *      cfg.gateway_mode = false;  // set true for root node
 *      cfg.channel = 1;           // all nodes must match
 *
 *      ESP_ERROR_CHECK(mesh_espnow_init(&cfg));
 *      ESP_ERROR_CHECK(mesh_espnow_start());
 *
 *      // Send a message (automatic routing + reliable delivery)
 *      mesh_espnow_send(0x02120001, "hello", 5, NULL);
 *
 *      // Broadcast to all nodes
 *      mesh_espnow_broadcast("alert", 5);
 *  }
 *
 *
 * =============================================================================
 *  ERROR HANDLING
 * =============================================================================
 *
 *  Every function that can fail returns an esp_err_t. Check all return values.
 *  Use ESP_ERROR_CHECK() during development, handle gracefully in production.
 *
 *  Get human-readable errors:
 *      const char *msg = mesh_espnow_err_to_str(err);
 *
 * =============================================================================
 *  THREAD SAFETY
 * =============================================================================
 *
 *  All public functions are safe to call from any FreeRTOS task.
 *  Do NOT call from ISRs (call mesh_espnow_process_from_isr() instead).
 *  Callbacks run in the context of the caller; keep them brief.
 *
 * =============================================================================
 *  POWER GUIDE
 * =============================================================================
 *
 *  Mode              Avg Current     250mAh Life     3400mAh Life
 *  ALWAYS_ON         ~15 mA          17 hours        9 days
 *  DUTY_CYCLE(5s)    ~130 µA         80 days         3 years
 *  DEEP_SLEEP(30s)   ~26 µA          400 days        15 years
 *  DEEP_SLEEP(60s)   ~14 µA          2 years         28 years
 *
 *  For battery nodes: POWER_DEEP_SLEEP + deep_sleep_interval_ms >= 30000
 *
 * =============================================================================
 *  LICENSE: MIT
 * =============================================================================
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 *  VERSIONING
 *============================================================================*/

#define MESH_ESPNOW_VERSION_MAJOR    3
#define MESH_ESPNOW_VERSION_MINOR    0
#define MESH_ESPNOW_VERSION_PATCH    0
#define MESH_ESPNOW_VERSION_STRING   "3.0.0"

/**
 * @brief Compile-time version info.
 */
typedef struct {
    uint8_t  major;
    uint8_t  minor;
    uint8_t  patch;
    uint32_t build_time;      /**< Unix timestamp of compilation */
    char     git_sha[16];     /**< Git commit hash (or "unknown") */
    char     idf_ver[32];     /**< ESP-IDF version used */
} mesh_espnow_version_t;

/**
 * @brief Get version information.
 */
const mesh_espnow_version_t* mesh_espnow_get_version(void);

/*============================================================================
 *  LIMITS (do not change; enforced at runtime)
 *============================================================================*/

#define MESH_ESPNOW_MAX_PAYLOAD_LEN   208  /**< Max application payload per packet (240 - 24 hdr - 8 MIC) */
#define MESH_ESPNOW_MAX_BRIDGE_SUBNETS 4   /**< Max subnets a bridge node can bridge */
#define MESH_ESPNOW_BRIDGE_QUEUE_DEPTH 16  /**< Bridge cross-subnet queue depth */
#define MESH_ESPNOW_MAX_HOPS           32  /**< Max route hop count */
#define MESH_ESPNOW_MAX_NEIGHBORS      32  /**< Max tracked neighbors */
#define MESH_ESPNOW_MAX_ROUTES         64  /**< Max routing table entries */
#define MESH_ESPNOW_MAX_RETX           16  /**< Max outstanding retransmissions */
#define MESH_ESPNOW_DUP_CACHE_SIZE    128  /**< Duplicate detection cache slots */
#define MESH_ESPNOW_ADDR_LEN            6  /**< MAC address length */
#define MESH_ESPNOW_PSK_LEN            16  /**< Pre-shared key length (bytes) */
#define MESH_ESPNOW_RREQ_RETRIES        3  /**< Max route discovery attempts */
#define MESH_ESPNOW_RATE_LIMIT_MS    1000  /**< Minimum ms between RREQs for same dest */

/*============================================================================
 *  ERROR CODES (extend esp_err_t base)
 *============================================================================*/

#define MESH_ESPNOW_ERR_BASE          0x0060       /**< Error base */
#define MESH_ESPNOW_ERR_INVALID_STATE (MESH_ESPNOW_ERR_BASE + 1)  /**< Wrong state for operation */
#define MESH_ESPNOW_ERR_NO_ROUTE      (MESH_ESPNOW_ERR_BASE + 2)  /**< No route to destination */
#define MESH_ESPNOW_ERR_RATE_LIMITED  (MESH_ESPNOW_ERR_BASE + 3)  /**< Operation rate-limited */
#define MESH_ESPNOW_ERR_NO_GATEWAY    (MESH_ESPNOW_ERR_BASE + 4)  /**< No gateway known */
#define MESH_ESPNOW_ERR_DUPLICATE     (MESH_ESPNOW_ERR_BASE + 5)  /**< Duplicate packet dropped */
#define MESH_ESPNOW_ERR_INVALID_PARAM (MESH_ESPNOW_ERR_BASE + 6)  /**< Invalid parameter */
#define MESH_ESPNOW_ERR_PAYLOAD_TOO_BIG (MESH_ESPNOW_ERR_BASE + 7)/**> Payload exceeds limit */
#define MESH_ESPNOW_ERR_NOT_INITIALIZED (MESH_ESPNOW_ERR_BASE + 8)/**> init() not called */
#define MESH_ESPNOW_ERR_ALREADY_INIT   (MESH_ESPNOW_ERR_BASE + 9) /**> Already initialized */
#define MESH_ESPNOW_ERR_DECRYPT_FAILED (MESH_ESPNOW_ERR_BASE + 10)/**> MIC verification failed */
#define MESH_ESPNOW_ERR_CONFIG_INVALID (MESH_ESPNOW_ERR_BASE + 11)/**> Config rejected */

/**
 * @brief Convert error code to human-readable string.
 * @param err  Error code
 * @return Pointer to static string (never NULL)
 */
const char* mesh_espnow_err_to_str(esp_err_t err);

/*============================================================================
 *  LOG LEVELS (per-subsystem control)
 *============================================================================*/

typedef enum {
    MESH_ESPNOW_LOG_NONE    = 0,  /**< Suppress all logging */
    MESH_ESPNOW_LOG_ERROR   = 1,  /**< Only errors */
    MESH_ESPNOW_LOG_WARN    = 2,  /**< Errors + warnings */
    MESH_ESPNOW_LOG_INFO    = 3,  /**< Normal operation messages (default) */
    MESH_ESPNOW_LOG_DEBUG   = 4,  /**< Detailed debug */
    MESH_ESPNOW_LOG_VERBOSE = 5,  /**< Everything including hex dumps */
} mesh_espnow_log_level_t;

/**
 * @brief Set log level for a subsystem.
 * @param subsystem  "mesh", "routing", "reliable", "power", "security"
 * @param level      Log level
 */
void mesh_espnow_set_log_level(const char *subsystem, mesh_espnow_log_level_t level);

/*============================================================================
 *  NODE CAPABILITIES
 *============================================================================*/

typedef enum {
    MESH_ESPNOW_CAP_NONE         = 0,
    MESH_ESPNOW_CAP_GATEWAY      = (1 << 0),  /**< Internet backhaul available */
    MESH_ESPNOW_CAP_ROUTER       = (1 << 1),  /**< Always-on, forwards traffic */
    MESH_ESPNOW_CAP_LEAF         = (1 << 2),  /**< Battery-powered leaf */
    MESH_ESPNOW_CAP_SLEEPY       = (1 << 3),  /**< Deep-sleep capable */
    MESH_ESPNOW_CAP_STORE_FWD    = (1 << 4),  /**< Store-and-forward capable */
    MESH_ESPNOW_CAP_BRIDGE       = (1 << 5),  /**< Forwards between sub-networks */
} mesh_espnow_capability_t;

/*============================================================================
 *  POWER MODES
 *============================================================================*/

typedef enum {
    MESH_ESPNOW_POWER_ALWAYS_ON          = 0, /**< Mains powered, continuous listening */
    MESH_ESPNOW_POWER_DUTY_CYCLE         = 1, /**< Periodic sleep between wake windows */
    MESH_ESPNOW_POWER_DEEP_SLEEP         = 2, /**< Deep sleep with timer wakeup (periodic) */
    MESH_ESPNOW_POWER_DEEP_SLEEP_ON_DEMAND = 3, /**< Deep sleep, wakes only on ESP-NOW packet */
} mesh_espnow_power_mode_t;

/**
 * @brief Get human-readable power mode name.
 */
const char* mesh_espnow_power_mode_str(mesh_espnow_power_mode_t mode);

/*============================================================================
 *  NODE STATE MACHINE
 *============================================================================*/

typedef enum {
    MESH_ESPNOW_STATE_UNINITIALIZED = 0, /**< init() not called */
    MESH_ESPNOW_STATE_INIT          = 1, /**< init() done, start() not called */
    MESH_ESPNOW_STATE_DISCOVERING   = 2, /**< start() called, looking for network */
    MESH_ESPNOW_STATE_CONNECTED     = 3, /**< Joined network (has gateway route) */
    MESH_ESPNOW_STATE_SLEEPING      = 4, /**< Deep sleep */
    MESH_ESPNOW_STATE_ERROR         = 5, /**< Irrecoverable error; reinit required */
} mesh_espnow_state_t;

/**
 * @brief Get human-readable state name.
 */
const char* mesh_espnow_state_str(mesh_espnow_state_t state);

/*============================================================================
 *  DATA STRUCTURES
 *============================================================================*/

/**
 * @brief Route table entry (one destination).
 */
typedef struct {
    uint32_t node_id;           /**< Destination node ID */
    uint32_t next_hop;          /**< Next hop towards destination */
    uint8_t  hop_count;         /**< Distance in hops */
    int8_t   rssi;              /**< Signal strength of last packet (-dBm) */
    uint32_t last_seen_ms;      /**< When route was last used */
} mesh_espnow_route_t;

/**
 * @brief Neighbor table entry (node within direct radio range).
 */
typedef struct {
    uint32_t node_id;           /**< Neighbor's node ID */
    int8_t   rssi;              /**< Average signal strength */
    int8_t   rssi_min;          /**< Worst RSSI observed */
    int8_t   rssi_max;          /**< Best RSSI observed */
    uint32_t last_seen_ms;      /**< Last heard timestamp */
    uint8_t  hop_count;         /**< Their distance to gateway */
    uint8_t  capabilities;      /**< Bitmask of mesh_espnow_capability_t */
    uint32_t uptime_s;          /**< Their reported uptime (0 if unknown) */
    uint8_t  subnet_id;         /**< Their subnet (0 = global) */
    uint8_t  subnet_channel;    /**< Their subnet's channel (0 = same as global) */
} mesh_espnow_neighbor_t;

/**
 * @brief Network and node statistics.
 */
typedef struct {
    /* Uptime */
    uint32_t uptime_ms;             /**< Milliseconds since start() */

    /* Traffic counters */
    uint32_t tx_packets;            /**< Total packets transmitted */
    uint32_t tx_bytes;              /**< Total payload bytes transmitted */
    uint32_t rx_packets;            /**< Total packets received (for us) */
    uint32_t rx_bytes;              /**< Total payload bytes received */
    uint32_t forwarded;             /**< Packets relayed for other nodes */
    uint32_t dropped;               /**< Packets we couldn't deliver */

    /* Reliability */
    uint32_t retransmissions;       /**< Total retransmission attempts */
    uint32_t ack_sent;              /**< ACKs we generated */
    uint32_t ack_received;          /**< ACKs we received */
    uint32_t duplicates_detected;   /**> Duplicate packets suppressed */

    /* Routing */
    uint32_t rreqs_sent;            /**< Route discoveries initiated */
    uint32_t rreqs_received;        /**< Route requests heard */
    uint32_t rreps_sent;            /**< Route replies sent */
    uint32_t rreps_received;        /**< Route replies received */

    /* Network info */
    uint16_t neighbor_count;        /**< Current neighbor count */
    uint16_t route_count;           /**< Current route table size */
    int8_t   avg_rssi;              /**< Average RSSI of all neighbors */
    uint8_t  avg_hop_count;         /**< Average hop count to gateway */

    /* Gateway */
    uint32_t gateway_id;            /**< Current gateway (0 if none) */
    uint32_t parent_id;             /**< Next hop to gateway (0 if none) */

    /* Health */
    uint32_t battery_mv;            /**< Last reported battery voltage */
    uint32_t heap_free;             /**< Free heap at last check */
    uint32_t boot_count;            /**< Number of boots (from NVS) */
    uint32_t crash_count;           /**< Number of crashes detected */

    /* Performance (rolling window) */
    float    avg_tx_latency_ms;     /**< Average send-to-ACK latency */
    uint32_t peak_tx_latency_ms;    /**> Worst-case send-to-ACK latency */
} mesh_espnow_stats_t;

/**
 * @brief Runtime diagnostics for a single packet flow.
 */
typedef struct {
    uint32_t dest_id;               /**< Where we tried to send */
    esp_err_t result;               /**< Result of the operation */
    uint32_t discovery_time_ms;     /**< Time spent waiting for route (0 if existed) */
    uint32_t tx_time_ms;            /**< Time from first TX to ACK */
    uint8_t  retries_used;          /**< How many retransmissions */
    uint8_t  hops_taken;            /**< Hop count of route used */
    int8_t   final_rssi;            /**> RSSI of last hop */
} mesh_espnow_tx_diag_t;

/*============================================================================
 *  CALLBACKS
 *============================================================================*/

/**
 * @brief Application event callbacks.
 *
 * All callbacks run in caller's context (not ISR). Keep them fast.
 * For heavy processing, push to a queue and handle in a task.
 * Set to NULL for unused callbacks.
 */
typedef struct {
    /**
     * @brief A unicast data packet arrived for this node.
     * @param src    Source node ID
     * @param data   Payload bytes (valid only during callback)
     * @param len    Payload length
     * @param rssi   Signal strength of last hop
     */
    void (*on_data)(uint32_t src, const uint8_t *data, uint16_t len, int8_t rssi);

    /**
     * @brief A network-wide broadcast arrived.
     */
    void (*on_broadcast)(uint32_t src, const uint8_t *data, uint16_t len);

    /**
     * @brief A new node appeared within direct radio range.
     * @param node_id  Node ID
     * @param rssi     Signal strength
     */
    void (*on_node_discovered)(uint32_t node_id, int8_t rssi);

    /**
     * @brief A neighbor timed out (gone for neighbor_timeout_ms).
     */
    void (*on_node_lost)(uint32_t node_id);

    /**
     * @brief Node successfully joined the mesh (has route to gateway).
     */
    void (*on_network_joined)(uint32_t gateway_id);

    /**
     * @brief Lost connection to all gateways.
     */
    void (*on_network_lost)(void);

    /**
     * @brief A route table entry was added or changed.
     */
    void (*on_route_changed)(uint32_t dest, uint32_t next_hop, uint8_t hops);

    /**
     * @brief A fatal internal error occurred.
     * @param err  Error code
     * @param msg  Description
     */
    void (*on_fatal_error)(esp_err_t err, const char *msg);
} mesh_espnow_callbacks_t;

/*============================================================================
 *  CONFIGURATION
 *============================================================================*/

/**
 * @brief Mesh network configuration.
 *
 * Fill this in, validate with mesh_espnow_validate_config(), then pass
 * to mesh_espnow_init(). Use MESH_ESPNOW_CONFIG_DEFAULT() for defaults.
 */
typedef struct {
    /* ---- Identity & Role ---- */
    uint32_t node_id;                    /**< 0 = auto-generate from MAC */
    bool     gateway_mode;               /**< true = this node is network root */
    uint8_t  capabilities;               /**< Bitmask of mesh_espnow_capability_t */

    /* ---- Radio ---- */
    uint8_t  channel;                    /**< Wi-Fi channel (1-11). ALL NODES MUST MATCH. */

    /* ---- Timing (milliseconds) ---- */
    uint32_t beacon_interval_ms;         /**> How often we beacon (100-60000, default 3000) */
    uint32_t neighbor_timeout_ms;        /**> Forget silent neighbors after (5000-300000, default 30000) */
    uint32_t route_timeout_ms;           /**> Expire unused routes after (10000-600000, default 60000) */
    uint32_t retransmit_timeout_ms;      /**> Wait for ACK before retry (100-10000, default 500) */

    /* ---- Power ---- */
    mesh_espnow_power_mode_t power_mode; /**> Power management strategy */
    uint32_t deep_sleep_interval_ms;     /**> Deep sleep duration (100-600000, default 5000). For DEEP_SLEEP mode only. */
    uint32_t awake_window_ms;            /**> Awake time per cycle (20-5000, default 200) */

    /* ---- Reliability ---- */
    uint8_t  max_retransmits;            /**> Max retries per packet (0-10, default 3) */
    uint8_t  ttl;                        /**> Max forwarding hops (1-64, default 32) */

    /* ---- Memory limits ---- */
    uint16_t max_neighbors;              /**> Neighbor table size (4-128, default 32) */
    uint16_t max_routes;                 /**> Route table size (8-256, default 64) */

    /* ---- Security ---- */
    bool     encryption_enabled;         /**> Enable AES-128-CCM (default true) */
    uint8_t  pre_shared_key[16];         /**> 16-byte network key. All nodes must share the same key. */

    /* ---- Sub-network ---- */
    uint8_t  subnet_id;                  /**> Logical sub-network (0=global/all, 1-255=specific subnet) */
    uint8_t  subnet_channel;             /**> Dedicated channel for this subnet (0=use cfg.channel, 1-11=separate) */

    /* ---- Bridging ---- */
    uint8_t  bridge_subnets[MESH_ESPNOW_MAX_BRIDGE_SUBNETS]; /**> Additional subnets this node bridges (0-terminated) */
    uint16_t bridge_interval_ms;         /**> Ms to spend on each bridged channel (0=no channel hopping, SW bridge only) */

    /* ---- Diagnostics ---- */
    bool     enable_health_monitor;      /**> Track boot count, detect crashes (default true) */

    /* ---- Events ---- */
    mesh_espnow_callbacks_t callbacks;   /**> Application event handlers */
} mesh_espnow_config_t;

/**
 * @brief Get safe default configuration.
 *
 * Usage:
 *     mesh_espnow_config_t cfg = MESH_ESPNOW_CONFIG_DEFAULT();
 *     cfg.gateway_mode = true;   // override what you need
 *     cfg.channel = 6;
 *     ESP_ERROR_CHECK(mesh_espnow_init(&cfg));
 */
#define MESH_ESPNOW_CONFIG_DEFAULT() (mesh_espnow_config_t){\
    .node_id = 0,\
    .gateway_mode = false,\
    .capabilities = MESH_ESPNOW_CAP_ROUTER | MESH_ESPNOW_CAP_SLEEPY,\
    .channel = 1,\
    .beacon_interval_ms = 3000,\
    .neighbor_timeout_ms = 30000,\
    .route_timeout_ms = 60000,\
    .retransmit_timeout_ms = 500,\
    .power_mode = MESH_ESPNOW_POWER_DUTY_CYCLE,\
    .deep_sleep_interval_ms = 5000,\
    .awake_window_ms = 200,\
    .max_retransmits = 3,\
    .ttl = 32,\
    .max_neighbors = 32,\
    .max_routes = 64,\
    .encryption_enabled = true,\
    .pre_shared_key = {0x4D,0x45,0x53,0x48,0x2D,\
                       0x45,0x53,0x50,0x4E,0x4F,\
                       0x57,0x2D,0x4D,0x45,0x53,0x48},\
    .subnet_id = 0,\
    .subnet_channel = 0,\
    .bridge_subnets = {0},\
    .bridge_interval_ms = 0,\
    .enable_health_monitor = true,\
    .callbacks = {\
        .on_data = NULL,\
        .on_broadcast = NULL,\
        .on_node_discovered = NULL,\
        .on_node_lost = NULL,\
        .on_network_joined = NULL,\
        .on_network_lost = NULL,\
        .on_route_changed = NULL,\
        .on_fatal_error = NULL,\
    },\
}

/**
 * @brief Validate configuration before initializing.
 *
 * Checks all fields for valid ranges and consistency.
 * Call this before mesh_espnow_init() to catch config errors early.
 *
 * @param cfg    Configuration to validate
 * @param err    [out] If non-NULL, receives human-readable description of first error
 * @return ESP_OK if valid, MESH_ESPNOW_ERR_CONFIG_INVALID if not
 */
esp_err_t mesh_espnow_validate_config(const mesh_espnow_config_t *cfg, const char **err);

/*============================================================================
 *  LIFECYCLE API
 *============================================================================*/

/**
 * @brief Initialize the mesh network stack.
 *
 * State transition: UNINITIALIZED → INIT
 *
 * Must be called once before any other mesh_espnow_*() function.
 * Initializes ESP-NOW, sets up Wi-Fi in promiscuous mode, and prepares
 * all subsystems. Does NOT start sending beacons yet.
 *
 * @param cfg  Configuration (NULL = use defaults)
 * @return ESP_OK on success, error code on failure
 */
esp_err_t mesh_espnow_init(const mesh_espnow_config_t *cfg);

/**
 * @brief Start mesh network operation.
 *
 * State transition: INIT → DISCOVERING
 *
 * Begins beacon transmission, neighbor discovery, and route learning.
 * After this call, the node is active on the mesh.
 *
 * @return ESP_OK on success
 *         MESH_ESPNOW_ERR_INVALID_STATE if init() not called or already started
 */
esp_err_t mesh_espnow_start(void);

/**
 * @brief Stop mesh network operation.
 *
 * State transition: any → INIT
 *
 * Sends farewell beacon, flushes queues, shuts down ESP-NOW.
 * Call init() again to restart.
 */
void mesh_espnow_stop(void);

/**
 * @brief Deinitialize and free all resources.
 *
 * State transition: any → UNINITIALIZED
 * After this call, init() must be called before any other operation.
 */
void mesh_espnow_deinit(void);

/**
 * @brief Reset configuration to factory defaults and clear NVS state.
 *
 * Reboots the chip after clearing.
 *
 * @return Does not return on success (reboots), error code on failure
 */
esp_err_t mesh_espnow_factory_reset(void);

/*============================================================================
 *  SEND API
 *============================================================================*/

/**
 * @brief Send data to a specific node (unicast with reliable delivery).
 *
 * If no route exists, automatic route discovery is triggered.
 * The packet is delivered reliably (ACK + retransmission).
 *
 * @param dest_id  Destination node ID
 * @param data     Payload (must remain valid until function returns)
 * @param len      Payload length in bytes (max MESH_ESPNOW_MAX_PAYLOAD_LEN)
 * @param diag     [out] Optional diagnostic info (may be NULL)
 * @return ESP_OK if queued for transmission
 *         MESH_ESPNOW_ERR_INVALID_STATE if not started
 *         MESH_ESPNOW_ERR_PAYLOAD_TOO_BIG if len exceeds limit
 *         MESH_ESPNOW_ERR_SEND_FAILED if ESP-NOW TX failed
 */
esp_err_t mesh_espnow_send(uint32_t dest_id, const uint8_t *data, uint16_t len,
                           mesh_espnow_tx_diag_t *diag);

/**
 * @brief Send a network-wide broadcast (flooded with duplicate suppression).
 *
 * Every node in the mesh receives this exactly once.
 *
 * @param data  Payload
 * @param len   Payload length
 * @return ESP_OK if queued
 */
esp_err_t mesh_espnow_broadcast(const uint8_t *data, uint16_t len);

/**
 * @brief Send data to the nearest gateway node.
 *
 * Automatically discovers the gateway if not yet known.
 *
 * @return ESP_OK if queued
 *         MESH_ESPNOW_ERR_NO_GATEWAY if no gateway known and discovery failed
 */
esp_err_t mesh_espnow_send_to_gateway(const uint8_t *data, uint16_t len);

/**
 * @brief Initiate route discovery to a destination.
 *
 * Normally done automatically by send(). Call this to proactively
 * establish a route before sending.
 *
 * Rate-limited: only one discovery per destination per second.
 *
 * @param dest_id  Target node ID
 * @return ESP_OK if RREQ broadcasted
 *         MESH_ESPNOW_ERR_RATE_LIMITED if too soon since last attempt
 */
esp_err_t mesh_espnow_discover_route(uint32_t dest_id);

/*============================================================================
 *  SUB-NETWORK & BRIDGE API
 *============================================================================*/

/**
 * @brief Send data to a node in a specific subnet (routes through bridge if needed).
 *
 * If dest_subnet matches this node's subnet, behaves like mesh_espnow_send().
 * If different, the packet is queued for bridge forwarding to that subnet.
 *
 * @param dest_id      Destination node ID
 * @param dest_subnet  Destination subnet ID (1-255). Use 0 for global/any.
 * @param data         Payload
 * @param len          Payload length
 * @return ESP_OK if queued
 *         MESH_ESPNOW_ERR_NO_ROUTE if no bridge configured for that subnet
 */
esp_err_t mesh_espnow_send_to_subnet(uint32_t dest_id, uint8_t dest_subnet,
                                     const uint8_t *data, uint16_t len);

/**
 * @brief Add a subnet for this node to bridge.
 *
 * The node will listen on the given channel for the given subnet
 * and forward packets between it and other bridged subnets.
 *
 * @param subnet_id   Subnet ID to bridge
 * @param channel     Wi-Fi channel for that subnet (0 = use cfg.channel)
 * @return ESP_OK on success
 */
esp_err_t mesh_espnow_bridge_add_subnet(uint8_t subnet_id, uint8_t channel);

/**
 * @brief Remove a subnet from the bridge list.
 */
esp_err_t mesh_espnow_bridge_remove_subnet(uint8_t subnet_id);

/**
 * @brief Get this node's subnet ID.
 */
uint8_t mesh_espnow_get_subnet(void);

/*============================================================================
 *  POWER MANAGEMENT API
 *============================================================================*/

/**
 * @brief Enter deep sleep.
 *
 * Saves critical state to RTC memory, sends farewell beacon,
 * and enters deep sleep. Wake behavior depends on power_mode:
 *   - DEEP_SLEEP: wakes after deep_sleep_interval_ms (timer)
 *   - DEEP_SLEEP_ON_DEMAND: wakes when any ESP-NOW packet
 *     arrives for this node. The packet is received and
 *     processed automatically after re-init on wake.
 *
 * On wake, the chip resets and app_main() runs again.
 * Call mesh_espnow_init() + mesh_espnow_start() to rejoin the mesh.
 * Any packets sent while asleep are buffered by ESP-NOW and
 * delivered to the first receive callback after wake.
 *
 * To wake a sleeping node remotely, send a DATA packet to
 * its node ID. It will wake, process, and deliver via on_data.
 *
 * @return Does not return (chip sleeps)
 */
esp_err_t mesh_espnow_sleep(void);

/**
 * @brief Update battery voltage reading.
 *
 * Used for power-aware routing decisions and statistics.
 * Call this when you take an ADC reading.
 *
 * @param millivolts  Battery voltage in mV (0 = unknown)
 */
void mesh_espnow_update_battery(uint32_t millivolts);

/**
 * @brief Estimate remaining battery life.
 *
 * Based on current power mode, duty cycle, and battery capacity.
 *
 * @param battery_capacity_mah  Battery capacity in mAh (e.g., 250, 3400)
 * @return Estimated remaining life in seconds
 */
uint32_t mesh_espnow_estimate_life_s(uint32_t battery_capacity_mah);

/*============================================================================
 *  INFO & DIAGNOSTICS API
 *============================================================================*/

/**
 * @brief Get this node's unique 32-bit ID.
 * @return Node ID (0 if not initialized)
 */
uint32_t mesh_espnow_get_node_id(void);

/**
 * @brief Get the current node state.
 */
mesh_espnow_state_t mesh_espnow_get_state(void);

/**
 * @brief Get network statistics snapshot.
 * @param[out] stats  Filled with current stats
 */
esp_err_t mesh_espnow_get_stats(mesh_espnow_stats_t *stats);

/**
 * @brief Get routing table snapshot.
 *
 * @param[out] entries  Array to fill with route entries
 * @param      max      Capacity of entries array
 * @return Number of routes written (may be less than max)
 */
uint16_t mesh_espnow_get_routes(mesh_espnow_route_t *entries, uint16_t max);

/**
 * @brief Get neighbor table snapshot.
 *
 * @param[out] entries  Array to fill
 * @param      max      Capacity
 * @return Number of neighbors written
 */
uint16_t mesh_espnow_get_neighbors(mesh_espnow_neighbor_t *entries, uint16_t max);

/**
 * @brief Get current parent (next hop towards gateway).
 * @return Parent node ID, or 0 if not connected to a gateway.
 */
uint32_t mesh_espnow_get_parent(void);

/**
 * @brief Get current gateway node ID.
 * @return Gateway ID, or 0 if none known.
 */
uint32_t mesh_espnow_get_gateway(void);

/**
 * @brief Check if the mesh is healthy.
 *
 * @return true if: started, has neighbors, has gateway route, no errors
 */
bool mesh_espnow_is_healthy(void);

/**
 * @brief Get detailed error message for the last failure.
 * @return Pointer to static string (never NULL, valid until next operation)
 */
const char* mesh_espnow_last_error(void);

/**
 * @brief Clear all diagnostic counters.
 */
void mesh_espnow_reset_stats(void);

/*============================================================================
 *  MAINTENANCE API
 *============================================================================*/

/**
 * @brief Process mesh network events.
 *
 * Call this periodically from your main loop or task.
 * Handles: beacon transmission, neighbor aging, route maintenance,
 * retransmission timeouts, store-and-forward delivery, bridge channel-hopping.
 *
 * Best practice: call every 50-100ms.
 *
 * @param now_ms  Current time in ms (use esp_timer_get_time() / 1000)
 */
void mesh_espnow_process(uint32_t now_ms);

/**
 * @brief Process events from an ISR context (minimal processing).
 *
 * Call this from your ESP-NOW receive callback ISR if you handle
 * ESP-NOW directly. Only enqueues the packet for main-context processing.
 */
void mesh_espnow_process_from_isr(void);

/**
 * @brief Trigger an immediate health check.
 *
 * Logs current state, neighbor count, route count, memory usage.
 * Call periodically or when debugging connectivity issues.
 */
void mesh_espnow_diagnostic_scan(void);

#ifdef __cplusplus
}
#endif
