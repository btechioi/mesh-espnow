# API Reference

> **Every function, type, macro, enum, and error code in the mesh library.**

---

## Header

```c
#include "mesh_espnow.h"
```

---

## Using with Arduino

The library is fully compatible with the Arduino IDE and PlatformIO.

### Install
1. Copy `mesh_espnow/` to your Arduino `libraries/` folder, OR
2. In PlatformIO, add to `platformio.ini`:
   ```ini
   lib_deps = https://github.com/btechioi/mesh-espnow
   ```

### Arduino sketch structure

```cpp
#include <Arduino.h>
#include "mesh_espnow.h"

// Callbacks
void on_data(uint32_t src, const uint8_t *data, uint16_t len, int8_t rssi) {
    Serial.printf("Data from 0x%08X: %.*s\n", src, len, data);
}

void setup() {
    Serial.begin(115200);

    mesh_espnow_config_t cfg = MESH_ESPNOW_CONFIG_DEFAULT();
    cfg.channel = 6;
    cfg.callbacks.on_data = on_data;

    ESP_ERROR_CHECK(mesh_espnow_init(&cfg));
    ESP_ERROR_CHECK(mesh_espnow_start());
}

void loop() {
    mesh_espnow_process(millis());
    delay(100);
}
```

Use `setup()`/`loop()` instead of `app_main()`. Use `delay()` instead of `vTaskDelay()`.
`ESP_ERROR_CHECK()` and `esp_err_t` work because Arduino ESP32 core bundles ESP-IDF.

---

## Error Codes

Returned by most API functions. `ESP_OK` (0) = success.

```c
// Just use esp_err_t directly
```

| Constant | Value | Meaning |
|----------|-------|---------|
| `MESH_ESPNOW_ERR_INVALID_STATE` | 0x0061 | Wrong state for this operation (e.g., `send()` before `start()`) |
| `MESH_ESPNOW_ERR_NO_ROUTE` | 0x0062 | No known path to destination; RREQ may be in progress |
| `MESH_ESPNOW_ERR_RATE_LIMITED` | 0x0063 | RREQ sent too recently; waiting on exponential backoff |
| `MESH_ESPNOW_ERR_NO_GATEWAY` | 0x0064 | No gateway known in the network |
| `MESH_ESPNOW_ERR_DUPLICATE` | 0x0065 | Broadcast already seen (duplicate cache) |
| `MESH_ESPNOW_ERR_INVALID_PARAM` | 0x0066 | NULL pointer or out-of-range value |
| `MESH_ESPNOW_ERR_PAYLOAD_TOO_BIG` | 0x0067 | Data exceeds `MESH_ESPNOW_MAX_PAYLOAD_LEN` (208 bytes) |
| `MESH_ESPNOW_ERR_NOT_INITIALIZED` | 0x0068 | `mesh_espnow_init()` not called yet |
| `MESH_ESPNOW_ERR_ALREADY_INIT` | 0x0069 | `mesh_espnow_init()` already called |
| `MESH_ESPNOW_ERR_DECRYPT_FAILED` | 0x006A | MIC mismatch — wrong key or packet corruption |
| `MESH_ESPNOW_ERR_CONFIG_INVALID` | 0x006B | Config validation failed |

Convert to string:

```c
const char *msg = mesh_espnow_err_to_str(err);
```

---

## Version Info

```c
const mesh_espnow_version_t* mesh_espnow_get_version(void);
```

Returns compile-time version, git SHA, and ESP-IDF version:

```c
typedef struct {
    uint8_t  major;          // 3
    uint8_t  minor;          // 0
    uint8_t  patch;          // 0
    uint32_t build_time;     // Unix timestamp
    char     git_sha[16];    // Git commit hash (or "unknown")
    char     idf_ver[32];    // ESP-IDF version
} mesh_espnow_version_t;
```

---

## Enums

### `mesh_espnow_state_t`

```c
typedef enum {
    MESH_ESPNOW_STATE_UNINITIALIZED = 0,  // Before init(), after deinit()
    MESH_ESPNOW_STATE_INIT,               // After init(), before start()
    MESH_ESPNOW_STATE_DISCOVERING,        // After start(), looking for network
    MESH_ESPNOW_STATE_CONNECTED,          // Have route to at least one gateway
    MESH_ESPNOW_STATE_SLEEPING,           // Deep sleep mode
    MESH_ESPNOW_STATE_ERROR               // Fatal error occurred
} mesh_espnow_state_t;
```

Get human-readable name:

```c
const char *mesh_espnow_state_str(mesh_espnow_state_t state);
```

### `mesh_espnow_power_mode_t`

```c
typedef enum {
    MESH_ESPNOW_POWER_ALWAYS_ON          = 0,  // Never sleep
    MESH_ESPNOW_POWER_DUTY_CYCLE,              // Modem-sleep between beacons
    MESH_ESPNOW_POWER_DEEP_SLEEP,              // Deep sleep with timer wakeup
    MESH_ESPNOW_POWER_DEEP_SLEEP_ON_DEMAND     // Deep sleep, wake on ESP-NOW packet
} mesh_espnow_power_mode_t;
```

Get human-readable name:

```c
const char *mesh_espnow_power_mode_str(mesh_espnow_power_mode_t mode);
```

### `mesh_espnow_log_level_t`

```c
typedef enum {
    MESH_ESPNOW_LOG_NONE    = 0,
    MESH_ESPNOW_LOG_ERROR   = 1,
    MESH_ESPNOW_LOG_WARN    = 2,
    MESH_ESPNOW_LOG_INFO    = 3,  // default
    MESH_ESPNOW_LOG_DEBUG   = 4,
    MESH_ESPNOW_LOG_VERBOSE = 5,
} mesh_espnow_log_level_t;
```

### Capability flags

```c
#define MESH_ESPNOW_CAP_GATEWAY   (1 << 0)  // Network root
#define MESH_ESPNOW_CAP_ROUTER    (1 << 1)  // Forwards traffic
#define MESH_ESPNOW_CAP_LEAF      (1 << 2)  // Battery-powered, no forwarding
#define MESH_ESPNOW_CAP_SLEEPY    (1 << 3)  // May sleep at any time
#define MESH_ESPNOW_CAP_STORE_FWD (1 << 4)  // Store-and-forward capable
#define MESH_ESPNOW_CAP_BRIDGE    (1 << 5)  // Forwards between sub-networks
```

Combine with bitwise OR:

```c
cfg.capabilities = MESH_ESPNOW_CAP_ROUTER | MESH_ESPNOW_CAP_SLEEPY;
```

---

## Limits

```c
#define MESH_ESPNOW_MAX_PAYLOAD_LEN     208   // Max application payload per packet
#define MESH_ESPNOW_MAX_HOPS             32   // Max route hop count
#define MESH_ESPNOW_MAX_NEIGHBORS        32   // Default max neighbors
#define MESH_ESPNOW_MAX_ROUTES           64   // Default max routes
#define MESH_ESPNOW_MAX_RETX             16   // Max outstanding retransmissions
#define MESH_ESPNOW_DUP_CACHE_SIZE      128   // Duplicate detection cache slots
#define MESH_ESPNOW_PSK_LEN              16   // Pre-shared key length
```

---

## Configuration

### `mesh_espnow_config_t`

Defines everything about a node before `mesh_espnow_init()`.

```c
mesh_espnow_config_t cfg = MESH_ESPNOW_CONFIG_DEFAULT();
```

Override the fields you care about.

#### Field reference

| Field | Type | Default | Range | Description |
|-------|------|---------|-------|-------------|
| `node_id` | `uint32_t` | 0 | any 32-bit | 0 = auto-generate from MAC |
| `gateway_mode` | `bool` | false | true/false | true = this node is a network root |
| `capabilities` | `uint8_t` | `ROUTER\|SLEEPY` | bitmask | Advertised in every beacon |
| `channel` | `uint8_t` | 1 | 1-11 | **All nodes must match** |
| `beacon_interval_ms` | `uint32_t` | 3000 | 100-60000 | How often to beacon (ms) |
| `neighbor_timeout_ms` | `uint32_t` | 30000 | 5000-300000 | Forget silent neighbors (ms) |
| `route_timeout_ms` | `uint32_t` | 60000 | 10000-600000 | Expire unused routes (ms) |
| `retransmit_timeout_ms` | `uint32_t` | 500 | 100-10000 | ACK timeout (ms) |
| `power_mode` | enum | DUTY_CYCLE | enum | ALWAYS_ON / DUTY_CYCLE / DEEP_SLEEP / DEEP_SLEEP_ON_DEMAND |
| `deep_sleep_interval_ms` | `uint32_t` | 5000 | 100-600000 | Deep sleep duration (ms) |
| `awake_window_ms` | `uint32_t` | 200 | 20-5000 | Per-cycle awake time (ms) |
| `max_retransmits` | `uint8_t` | 3 | 0-10 | Per-hop retries |
| `ttl` | `uint8_t` | 32 | 1-64 | Max forwarding distance |
| `max_neighbors` | `uint16_t` | 32 | 4-128 | Neighbor table size |
| `max_routes` | `uint16_t` | 64 | 8-256 | Route table size |
| `encryption_enabled` | `bool` | true | true/false | AES-128-CCM per-packet |
| `pre_shared_key` | `uint8_t[16]` | `"MESH-ESPNOW-MESH"` | 16 bytes | Network key |
| `subnet_id` | `uint8_t` | 0 | 0-255 | Logical sub-network |
| `subnet_channel` | `uint8_t` | 0 | 0-11 | Dedicated subnet channel |
| `bridge_subnets` | `uint8_t[4]` | {0} | per entry | Subnets this node bridges |
| `bridge_interval_ms` | `uint16_t` | 0 | any | Ms per bridged channel |
| `enable_health_monitor` | `bool` | true | true/false | Boot/crash tracking via NVS |
| `callbacks` | struct | all NULL | — | Event handlers |

### Callbacks

```c
typedef struct {
    void (*on_data)(uint32_t src, const uint8_t *data,
                    uint16_t len, int8_t rssi);
    void (*on_broadcast)(uint32_t src, const uint8_t *data,
                         uint16_t len);
    void (*on_node_discovered)(uint32_t node_id, int8_t rssi);
    void (*on_node_lost)(uint32_t node_id);
    void (*on_network_joined)(uint32_t gateway_id);
    void (*on_network_lost)(void);
    void (*on_route_changed)(uint32_t dest, uint32_t next_hop,
                             uint8_t hops);
    void (*on_fatal_error)(esp_err_t err, const char *msg);
} mesh_espnow_callbacks_t;
```

| Callback | When it fires |
|----------|---------------|
| `on_data` | Unicast DATA received for this node |
| `on_broadcast` | Network-wide broadcast received |
| `on_node_discovered` | New neighbor in direct radio range |
| `on_node_lost` | Neighbor timed out |
| `on_network_joined` | Route to a gateway established (state → CONNECTED) |
| `on_network_lost` | All gateway routes lost (state → DISCOVERING) |
| `on_route_changed` | Route installed, switched, or metric changed |
| `on_fatal_error` | Internal error — should trigger reinit |

All callbacks run in the caller's context. Keep them short.

### Config validation

```c
esp_err_t mesh_espnow_validate_config(const mesh_espnow_config_t *cfg, const char **err);
```

Pre-flight check without initializing. Returns `ESP_OK` if valid, `MESH_ESPNOW_ERR_CONFIG_INVALID` otherwise. Optionally get a human-readable error message via `*err`.

---

## Lifecycle Functions

### `mesh_espnow_init()`

```c
esp_err_t mesh_espnow_init(const mesh_espnow_config_t *config);
```

**State transition**: UNINIT → INIT. Must be called first.

**Parameters**:
- `config` — pointer to config struct (not modified; can be stack-allocated). NULL = use defaults.

**Returns**: `ESP_OK` or a specific error code.

**What it does**: Validates config, inits Wi-Fi + ESP-NOW + NVS, allocates tables, inits all subsystems.

---

### `mesh_espnow_start()`

```c
esp_err_t mesh_espnow_start(void);
```

**State transition**: INIT → DISCOVERING. Begins beaconing and neighbor discovery.

**Returns**: `ESP_OK` or `MESH_ESPNOW_ERR_INVALID_STATE`.

---

### `mesh_espnow_stop()`

```c
void mesh_espnow_stop(void);
```

**State transition**: any → INIT. Sends GOODBYE, stops timers, deinits radio.

---

### `mesh_espnow_deinit()`

```c
void mesh_espnow_deinit(void);
```

**State transition**: any → UNINIT. Full teardown, frees all memory.

---

### `mesh_espnow_factory_reset()`

```c
esp_err_t mesh_espnow_factory_reset(void);
```

Erases NVS mesh data and reboots. Does not return on success.

---

### `mesh_espnow_process()`

```c
void mesh_espnow_process(uint32_t now_ms);
```

**Call every 50-100ms** in your main loop. Handles: packet RX/TX, beacon sending, neighbor/route expiry, route optimization, retransmissions, health logging.

**Parameters**:
- `now_ms` — current time in ms (`esp_timer_get_time() / 1000` or `millis()` on Arduino)

---

### `mesh_espnow_process_from_isr()`

```c
void mesh_espnow_process_from_isr(void);
```

Minimal ISR-safe path. Call from ESP-NOW receive ISR if handling ESP-NOW directly. Only enqueues packet for main-context processing.

---

## Send Functions

### `mesh_espnow_send()`

```c
esp_err_t mesh_espnow_send(uint32_t dest_id, const uint8_t *data,
                           uint16_t len, mesh_espnow_tx_diag_t *diag);
```

Unicast to a specific node with ACK + retransmission. Auto-discovers route if needed.

**Parameters**:
- `dest_id` — destination node ID
- `data` — payload
- `len` — payload length (max `MESH_ESPNOW_MAX_PAYLOAD_LEN`)
- `diag` — optional diagnostic output (may be NULL)

**Returns**: `ESP_OK` on successful submission (not delivery).

### `mesh_espnow_broadcast()`

```c
esp_err_t mesh_espnow_broadcast(const uint8_t *data, uint16_t len);
```

Network-wide flood with duplicate suppression.

### `mesh_espnow_send_to_gateway()`

```c
esp_err_t mesh_espnow_send_to_gateway(const uint8_t *data, uint16_t len);
```

Sends to the best-known gateway.

### `mesh_espnow_discover_route()`

```c
esp_err_t mesh_espnow_discover_route(uint32_t dest_id);
```

Proactive RREQ. Rate-limited with exponential backoff.

### `mesh_espnow_send_to_subnet()`

```c
esp_err_t mesh_espnow_send_to_subnet(uint32_t dest_id, uint8_t dest_subnet,
                                     const uint8_t *data, uint16_t len);
```

Send to a node in a specific subnet (routes through bridge if needed).

---

## Bridge API

### `mesh_espnow_bridge_add_subnet()`

```c
esp_err_t mesh_espnow_bridge_add_subnet(uint8_t subnet_id, uint8_t channel);
```

Add a subnet for this node to bridge.

### `mesh_espnow_bridge_remove_subnet()`

```c
esp_err_t mesh_espnow_bridge_remove_subnet(uint8_t subnet_id);
```

Remove a subnet from the bridge list.

### `mesh_espnow_get_subnet()`

```c
uint8_t mesh_espnow_get_subnet(void);
```

Get this node's subnet ID.

---

## Send Diagnostics

```c
typedef struct {
    uint32_t dest_id;             // Where we tried to send
    esp_err_t result;             // Result of the operation
    uint32_t discovery_time_ms;   // Time spent waiting for route (0 if existed)
    uint32_t tx_time_ms;          // Time from first TX to ACK
    uint8_t  retries_used;        // How many retransmissions
    uint8_t  hops_taken;          // Hop count of route used
    int8_t   final_rssi;          // RSSI of last hop
} mesh_espnow_tx_diag_t;
```

Pass to `mesh_espnow_send()` for per-call diagnostics.

---

## Data Structures

### `mesh_espnow_route_t`

```c
typedef struct {
    uint32_t node_id;           // Destination node ID
    uint32_t next_hop;          // Next hop towards destination
    uint8_t  hop_count;         // Distance in hops
    int8_t   rssi;              // Signal strength of last packet
    uint32_t last_seen_ms;      // When route was last used
} mesh_espnow_route_t;
```

### `mesh_espnow_neighbor_t`

```c
typedef struct {
    uint32_t node_id;           // Neighbor's node ID
    int8_t   rssi;              // Average signal strength
    int8_t   rssi_min;          // Worst RSSI observed
    int8_t   rssi_max;          // Best RSSI observed
    uint32_t last_seen_ms;      // Last heard timestamp
    uint8_t  hop_count;         // Their distance to gateway
    uint8_t  capabilities;      // Bitmask of mesh_espnow_capability_t
    uint32_t uptime_s;          // Their reported uptime
    uint8_t  subnet_id;         // Their subnet (0 = global)
    uint8_t  subnet_channel;    // Their subnet's channel
} mesh_espnow_neighbor_t;
```

### `mesh_espnow_stats_t`

```c
typedef struct {
    uint32_t uptime_ms;             // ms since start()
    uint32_t tx_packets;            // Total packets transmitted
    uint32_t tx_bytes;              // Total payload bytes transmitted
    uint32_t rx_packets;            // Total packets received (for us)
    uint32_t rx_bytes;              // Total payload bytes received
    uint32_t forwarded;             // Packets relayed for other nodes
    uint32_t dropped;               // Packets we couldn't deliver
    uint32_t retransmissions;       // Total retransmission attempts
    uint32_t ack_sent;              // ACKs we generated
    uint32_t ack_received;          // ACKs we received
    uint32_t duplicates_detected;   // Duplicate packets suppressed
    uint32_t rreqs_sent;            // Route discoveries initiated
    uint32_t rreqs_received;        // Route requests heard
    uint32_t rreps_sent;            // Route replies sent
    uint32_t rreps_received;        // Route replies received
    uint16_t neighbor_count;        // Current neighbor count
    uint16_t route_count;           // Current route table size
    int8_t   avg_rssi;              // Average RSSI of all neighbors
    uint8_t  avg_hop_count;         // Average hop count to gateway
    uint32_t gateway_id;            // Current gateway (0 if none)
    uint32_t parent_id;             // Next hop to gateway (0 if none)
    uint32_t battery_mv;            // Last reported battery voltage
    uint32_t heap_free;             // Free heap at last check
    uint32_t boot_count;            // Number of boots (from NVS)
    uint32_t crash_count;           // Number of crashes detected
    float    avg_tx_latency_ms;     // Average send-to-ACK latency
    uint32_t peak_tx_latency_ms;    // Worst-case send-to-ACK latency
} mesh_espnow_stats_t;
```

---

## Info Query Functions

| Function | Return | Description |
|----------|--------|-------------|
| `mesh_espnow_get_node_id()` | `uint32_t` | This node's 32-bit ID (0 if not init) |
| `mesh_espnow_get_state()` | `mesh_espnow_state_t` | Current state |
| `mesh_espnow_get_stats(&stats)` | `esp_err_t` | Snapshot of all counters |
| `mesh_espnow_get_routes(buf, max)` | `uint16_t` | Route table snapshot (count written) |
| `mesh_espnow_get_neighbors(buf, max)` | `uint16_t` | Neighbor table snapshot (count written) |
| `mesh_espnow_get_parent()` | `uint32_t` | Next hop to gateway (0 if none) |
| `mesh_espnow_get_gateway()` | `uint32_t` | Gateway ID (0 if none) |
| `mesh_espnow_is_healthy()` | `bool` | true if CONNECTED with neighbors + gateway |
| `mesh_espnow_last_error()` | `const char*` | Description of last error |

---

## Diagnostics Functions

| Function | Description |
|----------|-------------|
| `mesh_espnow_diagnostic_scan()` | Full state dump to stdout via ESP_LOG |
| `mesh_espnow_reset_stats()` | Zeros all statistical counters |

---

## Power Management Functions

### `mesh_espnow_sleep()`

```c
esp_err_t mesh_espnow_sleep(void);
```

Enter deep sleep. Sends GOODBYE, saves state, enters deep sleep.
- DEEP_SLEEP mode: wakes after `deep_sleep_interval_ms` (timer)
- DEEP_SLEEP_ON_DEMAND: wakes when any ESP-NOW packet arrives for this node

Does not return — the chip resets. On wake, call `init()` + `start()` again.

### `mesh_espnow_update_battery()`

```c
void mesh_espnow_update_battery(uint32_t millivolts);
```

Report battery voltage for routing decisions. 0 = mains-powered.

### `mesh_espnow_estimate_life_s()`

```c
uint32_t mesh_espnow_estimate_life_s(uint32_t battery_capacity_mah);
```

Theoretical battery life estimate based on current power mode.

---

## Logging

```c
void mesh_espnow_set_log_level(const char *subsystem, mesh_espnow_log_level_t level);
```

Subsystems: `"mesh"`, `"routing"`, `"reliable"`, `"power"`, `"security"`, `"diag"`

```c
mesh_espnow_set_log_level("routing", MESH_ESPNOW_LOG_DEBUG);
mesh_espnow_set_log_level("mesh",    MESH_ESPNOW_LOG_WARN);
```

---

## Thread Safety

- All public API functions are **thread-safe** (mutex-guarded)
- Do **not** call from ISR context (use `mesh_espnow_process_from_isr()`)
- Callbacks fire inside `mesh_espnow_process()` — keep them short

---

## Example: Full Configuration

```c
#include "mesh_espnow.h"

void app_main(void) {   // or setup() for Arduino
    mesh_espnow_config_t cfg = MESH_ESPNOW_CONFIG_DEFAULT();

    cfg.node_id = 0xA1000042;                 // fixed ID (0 = auto)
    cfg.channel = 6;                          // all nodes must match
    cfg.gateway_mode = false;
    memcpy(cfg.pre_shared_key, "CHANGE-ME-KEY!", 16);

    cfg.beacon_interval_ms = 5000;
    cfg.neighbor_timeout_ms = 60000;
    cfg.retransmit_timeout_ms = 300;
    cfg.max_retransmits = 5;
    cfg.max_neighbors = 64;
    cfg.max_routes = 128;
    cfg.power_mode = MESH_ESPNOW_POWER_DUTY_CYCLE;
    cfg.encryption_enabled = true;

    cfg.callbacks.on_data = my_data_callback;
    cfg.callbacks.on_node_discovered = my_discover_callback;
    cfg.callbacks.on_network_joined = my_joined_callback;

    ESP_ERROR_CHECK(mesh_espnow_init(&cfg));
    ESP_ERROR_CHECK(mesh_espnow_start());

    while (1) {
        mesh_espnow_process(esp_timer_get_time() / 1000);
        vTaskDelay(pdMS_TO_TICKS(50));   // or delay(50)
    }
}
```
