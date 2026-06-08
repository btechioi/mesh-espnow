# API Reference

> **Every function, type, macro, enum, and error code in the mesh library.**

---

## Header

```c
#include "mesh_espnow.h"
```

---

## Error Codes

### `mesh_espnow_err_t`

Returned by all API functions. `ESP_OK` (0) = success.

```c
typedef esp_err_t mesh_espnow_err_t;
```

| Constant | Value (ESP_ERROR_BASE +) | Meaning |
|----------|--------------------------|---------|
| `MESH_ESPNOW_ERR_INVALID_STATE` | 0x0061 | Wrong state for this operation (e.g., `send()` before `start()`) |
| `MESH_ESPNOW_ERR_NO_ROUTE` | 0x0062 | No known path to destination; RREQ may be in progress |
| `MESH_ESPNOW_ERR_RATE_LIMITED` | 0x0063 | RREQ sent too recently; waiting on exponential backoff |
| `MESH_ESPNOW_ERR_NO_GATEWAY` | 0x0064 | No gateway known in the network |
| `MESH_ESPNOW_ERR_DUPLICATE` | 0x0065 | Broadcast already seen (duplicate cache) |
| `MESH_ESPNOW_ERR_INVALID_PARAM` | 0x0066 | NULL pointer, bad pointer, or out-of-range value |
| `MESH_ESPNOW_ERR_PAYLOAD_TOO_BIG` | 0x0067 | Data exceeds max payload (226 encrypted / 234 plain) |
| `MESH_ESPNOW_ERR_NOT_INITIALIZED` | 0x0068 | `mesh_espnow_init()` not called yet |
| `MESH_ESPNOW_ERR_ALREADY_INIT` | 0x0069 | `mesh_espnow_init()` already called |
| `MESH_ESPNOW_ERR_DECRYPT_FAILED` | 0x006A | MIC mismatch — wrong key or packet corruption |
| `MESH_ESPNOW_ERR_CONFIG_INVALID` | 0x006B | Config validation failed (see error message) |

Convert any error code to a human-readable string:

```c
const char *mesh_espnow_err_to_str(mesh_espnow_err_t err);
```

Returns strings like `"INVALID_STATE"`, `"NO_ROUTE"`, etc. Never returns NULL.

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

### `mesh_espnow_power_mode_t`

```c
typedef enum {
    MESH_ESPNOW_POWER_ALWAYS_ON          = 0,  // Never sleep
    MESH_ESPNOW_POWER_DUTY_CYCLE,              // Modem-sleep between beacons
    MESH_ESPNOW_POWER_DEEP_SLEEP,              // Deep sleep with timer wakeup
    MESH_ESPNOW_POWER_DEEP_SLEEP_ON_DEMAND     // Deep sleep, wake on ESP-NOW packet
} mesh_espnow_power_mode_t;
```

### Capability flags

```c
#define MESH_ESPNOW_CAP_GATEWAY  (1 << 0)  // Network root
#define MESH_ESPNOW_CAP_ROUTER   (1 << 1)  // Forwards traffic
#define MESH_ESPNOW_CAP_LEAF     (1 << 2)  // Battery-powered, no forwarding
#define MESH_ESPNOW_CAP_SLEEPY   (1 << 3)  // May sleep at any time
```

Combine with bitwise OR:

```c
cfg.capabilities = MESH_ESPNOW_CAP_ROUTER | MESH_ESPNOW_CAP_SLEEPY;
// A router that also sleeps (wakes periodically)
```

---

## Configuration

### `mesh_espnow_config_t`

Defines everything about a node before `mesh_espnow_init()`. Fill it using:

```c
mesh_espnow_config_t cfg = MESH_ESPNOW_CONFIG_DEFAULT();
```

Then override the fields you care about.

#### Field reference

| Field | Type | Default | Valid range | Description |
|-------|------|---------|-------------|-------------|
| `node_id` | `uint32_t` | 0 | any 32-bit | 0 = auto-generate from MAC OUI + unique suffix |
| `gateway_mode` | `bool` | false | true/false | true = this node is a network root |
| `capabilities` | `uint8_t` | `ROUTER\|SLEEPY` | bitmask | Advertised capabilities in beacons |
| `channel` | `uint8_t` | 1 | 1-11 | Wi-Fi channel. **Must match all nodes.** |
| `beacon_interval_ms` | `uint32_t` | 3000 | 100-60000 | How often to broadcast beacon (ms) |
| `neighbor_timeout_ms` | `uint32_t` | 30000 | 5000-300000 | Forget neighbor after this silence (ms) |
| `route_timeout_ms` | `uint32_t` | 60000 | 10000-600000 | Expire unused route after this (ms) |
| `retransmit_timeout_ms` | `uint32_t` | 500 | 100-10000 | Wait for ACK before retry (ms) |
| `power_mode` | `uint8_t` | DUTY_CYCLE | enum | ALWAYS_ON / DUTY_CYCLE / DEEP_SLEEP / DEEP_SLEEP_ON_DEMAND |
| `deep_sleep_interval_ms` | `uint32_t` | 5000 | 100-600000 | Deep sleep timer duration (DEEP_SLEEP mode only) |
| `awake_window_ms` | `uint32_t` | 200 | 20-5000 | How long to stay awake in duty cycle (ms) |
| `max_retransmits` | `uint8_t` | 3 | 0-10 | Max retries per hop before giving up |
| `ttl` | `uint8_t` | 32 | 1-64 | Max number of hops a packet can travel |
| `max_neighbors` | `uint16_t` | 32 | 4-128 | Max entries in neighbor table |
| `max_routes` | `uint16_t` | 64 | 8-256 | Max entries in route table |
| `encryption_enabled` | `bool` | true | true/false | Enable AES-128-CCM per-packet |
| `pre_shared_key` | `uint8_t[16]` | `"MESH-ESPNOW-MESH"` | 16 bytes | Network encryption key |
| `enable_health_monitor` | `bool` | true | true/false | Track boot count, crashes via NVS |
| `callbacks` | `mesh_espnow_callbacks_t` | all NULL | struct | Event callbacks (see below) |

#### Default config macro

```c
#define MESH_ESPNOW_CONFIG_DEFAULT() {                      \
    .node_id = 0,                                           \
    .gateway_mode = false,                                  \
    .capabilities = MESH_ESPNOW_CAP_ROUTER |                \
                    MESH_ESPNOW_CAP_SLEEPY,                 \
    .channel = 1,                                           \
    .beacon_interval_ms = 3000,                             \
    .neighbor_timeout_ms = 30000,                           \
    .route_timeout_ms = 60000,                              \
    .retransmit_timeout_ms = 500,                           \
    .power_mode = MESH_ESPNOW_POWER_DUTY_CYCLE,            \
    .deep_sleep_interval_ms = 5000,                         \
    .awake_window_ms = 200,                                 \
    .max_retransmits = 3,                                   \
    .ttl = 32,                                              \
    .max_neighbors = 32,                                    \
    .max_routes = 64,                                       \
    .encryption_enabled = true,                             \
    .pre_shared_key = {0x4D,0x45,0x53,0x48,0x2D,0x45,0x53, \
                       0x50,0x4E,0x4F,0x57,0x2D,0x4D,0x45, \
                       0x53,0x48},  /* "MESH-ESPNOW-MESH" */ \
    .enable_health_monitor = true,                          \
    .callbacks = {0}                                        \
}
```

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

#### Callback details

| Callback | When it fires | Called from |
|----------|---------------|-------------|
| `on_data` | Unicast DATA received for this node | `mesh_espnow_process()` |
| `on_broadcast` | Broadcast received (first time only) | `mesh_espnow_process()` |
| `on_node_discovered` | New neighbor seen via beacon or any packet | `mesh_espnow_process()` |
| `on_node_lost` | Neighbor timed out (no beacon/packet for `neighbor_timeout_ms`) | `mesh_espnow_process()` |
| `on_network_joined` | First route to a gateway established | `mesh_espnow_process()` |
| `on_network_lost` | Last gateway route expired or lost | `mesh_espnow_process()` |
| `on_route_changed` | Route installed, switched, metric changed, or backup promoted | `mesh_espnow_process()` |
| `on_fatal_error` | Internal error (encryption failure, malloc fail, etc.) | `mesh_espnow_process()` |

All callbacks run in the context of the caller's `mesh_espnow_process()` call. Keep them short — don't block.

---

## Lifecycle Functions

### `mesh_espnow_init()`

```c
mesh_espnow_err_t mesh_espnow_init(const mesh_espnow_config_t *config);
```

**Purpose**: Initialize the mesh library. Must be called first.

**Parameters**:
- `config` — pointer to configuration struct. Not modified; can be stack-allocated.

**Returns**: `ESP_OK` or:
- `MESH_ESPNOW_ERR_CONFIG_INVALID` — one or more config fields out of range
- `MESH_ESPNOW_ERR_ALREADY_INIT` — already initialized
- `ESP_ERR_NO_MEM` — memory allocation failed
- Various ESP-IDF errors from Wi-Fi / ESP-NOW / NVS init

**What it does**:
1. Validates all config fields (range checks)
2. Initializes Wi-Fi in station mode (no AP)
3. Initializes ESP-NOW
4. Opens NVS (for health monitoring)
5. Allocates neighbor table, route table, retransmit queue
6. Initializes all subsystems (routing, reliable, security, power, diag)
7. Transitions state to `INIT`

---

### `mesh_espnow_start()`

```c
mesh_espnow_err_t mesh_espnow_start(void);
```

**Purpose**: Start mesh operation — begin beaconing and listening.

**Returns**: `ESP_OK` or:
- `MESH_ESPNOW_ERR_INVALID_STATE` — not in INIT state
- Various ESP-IDF errors

**What it does**:
1. Adds ESP-NOW peers as discovered (on-demand, not all at once)
2. Starts the beacon timer
3. Transitions state to `DISCOVERING`
4. Records boot timestamp

---

### `mesh_espnow_stop()`

```c
mesh_espnow_err_t mesh_espnow_stop(void);
```

**Purpose**: Gracefully stop mesh operation. Sends a GOODBYE beacon.

**Returns**: `ESP_OK` or `MESH_ESPNOW_ERR_INVALID_STATE` (not started).

**What it does**:
1. Broadcasts a GOODBYE packet (best-effort)
2. Stops all timers
3. Deinitializes Wi-Fi / ESP-NOW
4. Clears neighbor and route tables
5. Transitions state to `INIT`

---

### `mesh_espnow_deinit()`

```c
mesh_espnow_err_t mesh_espnow_deinit(void);
```

**Purpose**: Full teardown. Frees all memory. Returns to UNINITIALIZED state.

**Returns**: `ESP_OK` (never fails).

**What it does**:
1. Calls `stop()` if running
2. Frees all allocated tables
3. Closes NVS
4. Transitions state to `UNINITIALIZED`

---

### `mesh_espnow_factory_reset()`

```c
mesh_espnow_err_t mesh_espnow_factory_reset(void);
```

**Purpose**: Erase all NVS mesh data and reboot.

**Returns**: Never returns — calls `esp_restart()`.

**What it does**:
1. Erases the NVS mesh namespace
2. Calls `esp_restart()`

---

### `mesh_espnow_process()`

```c
mesh_espnow_err_t mesh_espnow_process(uint64_t now_ms);
```

**Purpose**: Process all pending events. **Must call this regularly** — at least every 50-100ms.

**Parameters**:
- `now_ms` — current time in milliseconds (pass `esp_timer_get_time() / 1000` or equivalent)

**Returns**: `ESP_OK` always.

**What it does** (in order):
1. Process received packets (decrypt, dispatch, forward)
2. Check ACK timeouts / trigger retransmissions
3. Check beacon send timer
4. Check neighbor / route expiry
5. Run route optimization pass (every 15s)
6. Log health diagnostics (every 30s)

---

## Send Functions

### `mesh_espnow_send()`

```c
mesh_espnow_err_t mesh_espnow_send(
    uint32_t dest_id,
    const uint8_t *data,
    uint16_t len,
    mesh_espnow_diag_t *diag);
```

**Purpose**: Send data to a specific node. Reliable (ACK + retransmit).

**Parameters**:
- `dest_id` — target node ID (must exist in route table, or RREQ will be sent)
- `data` — pointer to payload
- `len` — payload length in bytes
- `diag` — optional (can be NULL). Filled with send diagnostics (see below)

**Returns**: `ESP_OK` on successful **submission** (not delivery). Or:
- `MESH_ESPNOW_ERR_INVALID_STATE` — not in CONNECTED or DISCOVERING
- `MESH_ESPNOW_ERR_INVALID_PARAM` — NULL data or len=0
- `MESH_ESPNOW_ERR_PAYLOAD_TOO_BIG` — len > max payload
- `MESH_ESPNOW_ERR_NOT_INITIALIZED` — init() not called

**Note**: This is asynchronous. Delivery is reported via ACK at the reliable layer. Use `diag` to check per-call status.

---

### `mesh_espnow_broadcast()`

```c
mesh_espnow_err_t mesh_espnow_broadcast(
    const uint8_t *data,
    uint16_t len);
```

**Purpose**: Flood a message to every node in the network.

**Parameters**:
- `data` — pointer to payload
- `len` — payload length

**Returns**: Same as `mesh_espnow_send()`.

**Note**: Delivered via flooding. Each node re-broadcasts once with TTL-1. Duplicate suppression prevents loops. No ACK for broadcasts.

---

### `mesh_espnow_send_to_gateway()`

```c
mesh_espnow_err_t mesh_espnow_send_to_gateway(
    const uint8_t *data,
    uint16_t len);
```

**Purpose**: Convenience wrapper — sends to the best-known gateway.

**Returns**: Same as `mesh_espnow_send()` plus:
- `MESH_ESPNOW_ERR_NO_GATEWAY` — no gateway known

---

### `mesh_espnow_discover_route()`

```c
mesh_espnow_err_t mesh_espnow_discover_route(uint32_t dest_id);
```

**Purpose**: Proactively send an RREQ for a destination. Useful when you know you'll need a route soon.

**Parameters**:
- `dest_id` — target node ID

**Returns**: `ESP_OK` or:
- `MESH_ESPNOW_ERR_RATE_LIMITED` — RREQ already sent recently (wait for backoff)
- `MESH_ESPNOW_ERR_INVALID_STATE` — not started

---

## Info Query Functions

### `mesh_espnow_get_node_id()`

```c
uint32_t mesh_espnow_get_node_id(void);
```

**Returns**: This node's 32-bit ID. 0 if not initialized.

### `mesh_espnow_get_state()`

```c
mesh_espnow_state_t mesh_espnow_get_state(void);
```

**Returns**: Current state enum value (UNINITIALIZED, INIT, DISCOVERING, CONNECTED, SLEEPING, ERROR).

### `mesh_espnow_get_stats()`

```c
mesh_espnow_err_t mesh_espnow_get_stats(mesh_espnow_stats_t *stats);
```

Fills the stats struct:

```c
typedef struct {
    uint32_t   uptime_seconds;      // Time since start()
    uint32_t   packets_sent;        // Total DATA packets sent
    uint32_t   packets_received;    // Total DATA packets received
    uint32_t   packets_forwarded;   // Packets forwarded for others
    uint32_t   packets_lost;        // Packets not ACKed after max retries
    uint32_t   packets_decrypt_failed;  // MIC verification failures
    uint32_t   route_discoveries;   // RREQs sent
    uint32_t   route_repairs;       // Backup routes promoted
    int8_t     avg_rssi;            // Average RSSI of all neighbors
    uint16_t   neighbor_count;      // Current neighbor count
    uint16_t   route_count;         // Current route count
    uint32_t   battery_mv;          // Last reported battery mV
    uint8_t    hop_count;           // Hop count to nearest gateway
} mesh_espnow_stats_t;
```

### `mesh_espnow_get_routes()`

```c
uint16_t mesh_espnow_get_routes(
    mesh_espnow_route_info_t *routes,
    uint16_t max);
```

Fills an array of route info:

```c
typedef struct {
    uint32_t dest_id;       // Destination node
    uint32_t next_hop;      // Next hop node ID
    uint8_t  hops;          // Hop count
    uint16_t metric;        // Route metric (lower = better)
    bool     is_backup;     // true if this is the backup, false = primary
} mesh_espnow_route_info_t;
```

**Returns**: Number of routes written (may be less than max).

### `mesh_espnow_get_neighbors()`

```c
uint16_t mesh_espnow_get_neighbors(
    mesh_espnow_neighbor_info_t *neighbors,
    uint16_t max);
```

Fills an array of neighbor info:

```c
typedef struct {
    uint32_t node_id;       // Neighbor's node ID
    int8_t   rssi;          // Last heard RSSI
    uint16_t metric;        // Route metric to this neighbor
    uint8_t  capabilities;  // Advertised capabilities
    uint32_t last_seen_ms;  // Milliseconds since last contact
    uint16_t battery_mv;    // Last reported battery (0 = unknown)
    uint8_t  pdr;           // Packet delivery rate 0-100
} mesh_espnow_neighbor_info_t;
```

**Returns**: Number of neighbors written.

### `mesh_espnow_get_parent()`

```c
uint32_t mesh_espnow_get_parent(void);
```

**Returns**: Node ID of the next hop toward the gateway, or 0 if no gateway route.

### `mesh_espnow_get_gateway()`

```c
uint32_t mesh_espnow_get_gateway(void);
```

**Returns**: Node ID of the nearest gateway, or 0 if none known.

### `mesh_espnow_is_healthy()`

```c
bool mesh_espnow_is_healthy(void);
```

**Returns**: `true` if state is CONNECTED with at least one neighbor and one gateway route. `false` otherwise.

---

## Diagnostics Functions

### `mesh_espnow_last_error()`

```c
const char *mesh_espnow_last_error(void);
```

**Returns**: Description of the last error that occurred internally. Never returns NULL.

### `mesh_espnow_diagnostic_scan()`

```c
void mesh_espnow_diagnostic_scan(void);
```

**Purpose**: Prints a full dump of all internal state to stdout via ESP_LOG. Includes:
- Current state, uptime, node ID
- Neighbor table (all entries with metrics)
- Route table (all entries with metrics, primary/backup)
- Send/receive/loss counts
- Retransmit queue (pending entries)
- Battery, RSSI, hop count

### `mesh_espnow_reset_stats()`

```c
void mesh_espnow_reset_stats(void);
```

**Purpose**: Zeros all statistical counters (packets_sent, received, lost, etc.). Does not affect routes or neighbors.

---

## Power Management Functions

### `mesh_espnow_sleep()`

```c
void mesh_espnow_sleep(void) __attribute__((noreturn));
```

**Purpose**: Enter deep sleep. Does not return.

**Wake behavior depends on power_mode**:
- `DEEP_SLEEP`: wakes after `deep_sleep_interval_ms` (hardware timer)
- `DEEP_SLEEP_ON_DEMAND`: wakes when any ESP-NOW packet arrives for this node. The wakeup packet is received automatically and delivered after re-init.

**What it does**:
1. Broadcasts GOODBYE (best-effort)
2. Saves boot count to NVS
3. Configures wake source (timer or ESP-NOW depending on mode)
4. Calls `esp_deep_sleep_start()`

**Note**: On wake, the ESP32 resets. Your `app_main()` will run again. Call `mesh_espnow_init()` and `mesh_espnow_start()` fresh. The packet that woke the node (if any) is processed during init and delivered via `on_data`.

### `mesh_espnow_update_battery()`

```c
void mesh_espnow_update_battery(uint32_t millivolts);
```

**Purpose**: Report current battery voltage. Used by routing metric calculation (lower battery = worse metric, so traffic avoids this node).

**Parameters**:
- `millivolts` — battery voltage in mV (3000 = 3.0V, 0 = mains-powered)

### `mesh_espnow_estimate_life_s()`

```c
uint64_t mesh_espnow_estimate_life_s(uint32_t battery_capacity_mAh);
```

**Purpose**: Calculate theoretical battery life based on current power mode.

**Parameters**:
- `battery_capacity_mAh` — battery capacity in mAh

**Returns**: Estimated lifetime in seconds.

---

## Logging Functions

### `mesh_espnow_set_log_level()`

```c
void mesh_espnow_set_log_level(const char *subsystem, int level);
```

**Purpose**: Set log verbosity for a specific subsystem.

**Parameters**:
- `subsystem` — one of: `"mesh"`, `"routing"`, `"reliable"`, `"power"`, `"security"`, `"diag"`
- `level` — one of:
  - `MESH_ESPNOW_LOG_NONE` — suppress all output
  - `MESH_ESPNOW_LOG_ERROR` — errors only
  - `MESH_ESPNOW_LOG_WARN` — errors + warnings
  - `MESH_ESPNOW_LOG_INFO` — normal operational messages
  - `MESH_ESPNOW_LOG_DEBUG` — verbose debugging

**Example**:

```c
mesh_espnow_set_log_level("routing", MESH_ESPNOW_LOG_DEBUG);
mesh_espnow_set_log_level("mesh", MESH_ESPNOW_LOG_WARN);
```

---

## Config Validation

### `mesh_espnow_validate_config()`

```c
mesh_espnow_err_t mesh_espnow_validate_config(
    const mesh_espnow_config_t *config);
```

**Purpose**: Pre-flight check without initializing. Useful to verify configuration before deployment.

**Parameters**:
- `config` — pointer to configuration to check

**Returns**: `ESP_OK` if valid, or `MESH_ESPNOW_ERR_CONFIG_INVALID` with details logged.

**Validates**:
- `channel` ∈ [1, 11]
- `beacon_interval_ms` ∈ [100, 60000]
- `neighbor_timeout_ms` ≥ `beacon_interval_ms` × 2
- `route_timeout_ms` ≥ `neighbor_timeout_ms`
- `retransmit_timeout_ms` ∈ [100, 10000]
- `deep_sleep_interval_ms` ∈ [100, 600000]
- `awake_window_ms` ∈ [20, 5000]
- `max_retransmits` ≤ 10
- `ttl` ∈ [1, 64]
- `max_neighbors` ∈ [4, 128]
- `max_routes` ∈ [8, 256]

---

## Send Diagnostics

### `mesh_espnow_diag_t`

```c
typedef struct {
    uint8_t  attempts;        // How many send attempts made
    uint8_t  final_rssi;      // RSSI of last attempt (0 if unknown)
    uint32_t latency_ms;      // Time from first send to ACK (0 if failed)
    bool     acked;           // true if ACK received
    uint8_t  hops;            // Hop count to destination
} mesh_espnow_diag_t;
```

Pass to `mesh_espnow_send()` to get per-call diagnostics.

---

## Thread Safety Notes

- All public API functions are **thread-safe** (protected by a mutex)
- Do **not** call `mesh_espnow_*()` functions from ISR context
- Callbacks fire **inside** `mesh_espnow_process()`, in the caller's thread
- Keep callbacks short — they hold the mutex

---

## Example: Configuring Everything

```c
#include "mesh_espnow.h"

void app_main(void) {
    mesh_espnow_config_t cfg = MESH_ESPNOW_CONFIG_DEFAULT();

    // Identity
    cfg.node_id = 0xA1000042;                 // fixed ID (0 = auto)

    // Network
    cfg.channel = 6;                          // all nodes must match
    cfg.gateway_mode = false;                 // I'm not the root
    memcpy(cfg.pre_shared_key, "CHANGE-ME-KEY!", 16);  // change default!

    // Timing
    cfg.beacon_interval_ms = 5000;
    cfg.neighbor_timeout_ms = 60000;
    cfg.retransmit_timeout_ms = 300;
    cfg.max_retransmits = 5;

    // Capacity
    cfg.max_neighbors = 64;
    cfg.max_routes = 128;

    // Power
    cfg.power_mode = MESH_ESPNOW_POWER_DUTY_CYCLE;
    cfg.capabilities = MESH_ESPNOW_CAP_ROUTER;

    // Security
    cfg.encryption_enabled = true;

    // Callbacks
    cfg.callbacks.on_data = my_data_callback;
    cfg.callbacks.on_node_discovered = my_discover_callback;
    cfg.callbacks.on_network_joined = my_joined_callback;

    ESP_ERROR_CHECK(mesh_espnow_init(&cfg));
    ESP_ERROR_CHECK(mesh_espnow_start());

    while (1) {
        mesh_espnow_process(esp_timer_get_time() / 1000);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
```
