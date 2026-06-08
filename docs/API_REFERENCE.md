<p align="center">
  <img src="https://capsule-render.vercel.app/api?type=waving&color=0:ff8c00,100:ff4500&height=180&section=header&text=API%20Reference&fontSize=40&fontAlignY=35&animation=fadeIn&fontColor=ffffff"/>
</p>

# 📖 API Reference

> **Every function, type, macro, enum, and error code in the mesh library.**

---

# 🎯 Header

```c
#include "mesh_espnow.h"
```

---

# 🧩 Using with Arduino

The library is fully compatible with Arduino IDE and PlatformIO.

### Install
1. Copy `mesh_espnow/` → `~/Arduino/libraries/`
2. Or in PlatformIO: `lib_deps = https://github.com/btechioi/mesh-espnow`

### Sketch structure

```cpp
#include <Arduino.h>
#include "mesh_espnow.h"

void on_data(uint32_t src, const uint8_t *data, uint16_t len, int8_t rssi) {
    Serial.printf("From 0x%08X: %.*s\n", src, len, data);
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

Use `setup()`/`loop()` instead of `app_main()`. Use `delay()`/`millis()` instead of FreeRTOS.

---

# ❌ Error Codes

Returned by most API functions. `ESP_OK` (0) = success.

| Constant | Value | Meaning |
|----------|-------|---------|
| `MESH_ESPNOW_ERR_INVALID_STATE` | 0x0061 | Wrong state (e.g., send before start) |
| `MESH_ESPNOW_ERR_NO_ROUTE` | 0x0062 | No path to destination |
| `MESH_ESPNOW_ERR_RATE_LIMITED` | 0x0063 | RREQ sent too recently |
| `MESH_ESPNOW_ERR_NO_GATEWAY` | 0x0064 | No gateway known |
| `MESH_ESPNOW_ERR_DUPLICATE` | 0x0065 | Duplicate broadcast |
| `MESH_ESPNOW_ERR_INVALID_PARAM` | 0x0066 | NULL or out-of-range |
| `MESH_ESPNOW_ERR_PAYLOAD_TOO_BIG` | 0x0067 | Exceeds 208 bytes |
| `MESH_ESPNOW_ERR_NOT_INITIALIZED` | 0x0068 | `init()` not called |
| `MESH_ESPNOW_ERR_ALREADY_INIT` | 0x0069 | Already initialized |
| `MESH_ESPNOW_ERR_DECRYPT_FAILED` | 0x006A | MIC mismatch |
| `MESH_ESPNOW_ERR_CONFIG_INVALID` | 0x006B | Config rejected |

```c
const char *msg = mesh_espnow_err_to_str(err);
```

---

# ℹ️ Version Info

```c
const mesh_espnow_version_t* mesh_espnow_get_version(void);
```

```c
typedef struct {
    uint8_t  major;          // 3
    uint8_t  minor;          // 0
    uint8_t  patch;          // 0
    uint32_t build_time;     // Unix timestamp
    char     git_sha[16];    // Git commit hash
    char     idf_ver[32];    // ESP-IDF version
} mesh_espnow_version_t;
```

---

# 📋 Enums

## `mesh_espnow_state_t`

```c
typedef enum {
    MESH_ESPNOW_STATE_UNINITIALIZED = 0,
    MESH_ESPNOW_STATE_INIT,
    MESH_ESPNOW_STATE_DISCOVERING,
    MESH_ESPNOW_STATE_CONNECTED,
    MESH_ESPNOW_STATE_SLEEPING,
    MESH_ESPNOW_STATE_ERROR
} mesh_espnow_state_t;

const char *mesh_espnow_state_str(mesh_espnow_state_t state);
```

## `mesh_espnow_power_mode_t`

```c
typedef enum {
    MESH_ESPNOW_POWER_ALWAYS_ON          = 0,
    MESH_ESPNOW_POWER_DUTY_CYCLE,
    MESH_ESPNOW_POWER_DEEP_SLEEP,
    MESH_ESPNOW_POWER_DEEP_SLEEP_ON_DEMAND
} mesh_espnow_power_mode_t;

const char *mesh_espnow_power_mode_str(mesh_espnow_power_mode_t mode);
```

## `mesh_espnow_log_level_t`

```c
typedef enum {
    MESH_ESPNOW_LOG_NONE    = 0,
    MESH_ESPNOW_LOG_ERROR   = 1,
    MESH_ESPNOW_LOG_WARN    = 2,
    MESH_ESPNOW_LOG_INFO    = 3,
    MESH_ESPNOW_LOG_DEBUG   = 4,
    MESH_ESPNOW_LOG_VERBOSE = 5,
} mesh_espnow_log_level_t;
```

## Capability Flags

```c
#define MESH_ESPNOW_CAP_GATEWAY   (1 << 0)
#define MESH_ESPNOW_CAP_ROUTER    (1 << 1)
#define MESH_ESPNOW_CAP_LEAF      (1 << 2)
#define MESH_ESPNOW_CAP_SLEEPY    (1 << 3)
#define MESH_ESPNOW_CAP_STORE_FWD (1 << 4)
#define MESH_ESPNOW_CAP_BRIDGE    (1 << 5)
```

---

# ⚙️ Configuration

## `mesh_espnow_config_t`

```c
mesh_espnow_config_t cfg = MESH_ESPNOW_CONFIG_DEFAULT();
```

| Field | Type | Default | Range | Description |
|-------|------|---------|-------|-------------|
| `node_id` | `uint32_t` | 0 | any | 0 = auto from MAC |
| `gateway_mode` | `bool` | false | bool | true = root node |
| `capabilities` | `uint8_t` | ROUTER\|SLEEPY | bitmask | Advertised in beacons |
| `channel` | `uint8_t` | 1 | 1-11 | **All nodes must match** |
| `beacon_interval_ms` | `uint32_t` | 3000 | 100-60000 | Beacon period |
| `neighbor_timeout_ms` | `uint32_t` | 30000 | 5000-300000 | Forget silent neighbors |
| `route_timeout_ms` | `uint32_t` | 60000 | 10000-600000 | Expire unused routes |
| `retransmit_timeout_ms` | `uint32_t` | 500 | 100-10000 | ACK timeout |
| `power_mode` | enum | DUTY_CYCLE | 0-3 | Power strategy |
| `deep_sleep_interval_ms` | `uint32_t` | 5000 | 100-600000 | Sleep duration |
| `awake_window_ms` | `uint32_t` | 200 | 20-5000 | Duty cycle awake window |
| `max_retransmits` | `uint8_t` | 3 | 0-10 | Per-hop retries |
| `ttl` | `uint8_t` | 32 | 1-64 | Max hops |
| `max_neighbors` | `uint16_t` | 32 | 4-128 | Neighbor table size |
| `max_routes` | `uint16_t` | 64 | 8-256 | Route table size |
| `encryption_enabled` | `bool` | true | bool | AES-128-CCM |
| `pre_shared_key` | `uint8_t[16]` | "MESH-ESPNOW-MESH" | 16B | Network key |
| `subnet_id` | `uint8_t` | 0 | 0-255 | Logical subnet |
| `subnet_channel` | `uint8_t` | 0 | 0-11 | Subnet channel |
| `bridge_subnets` | `uint8_t[4]` | {0} | — | Bridged subnets |
| `bridge_interval_ms` | `uint16_t` | 0 | any | Channel hop interval |
| `enable_health_monitor` | `bool` | true | bool | Boot/crash tracking |
| `callbacks` | struct | all NULL | — | Event handlers |

## Callbacks

```c
typedef struct {
    void (*on_data)(uint32_t src, const uint8_t *data, uint16_t len, int8_t rssi);
    void (*on_broadcast)(uint32_t src, const uint8_t *data, uint16_t len);
    void (*on_node_discovered)(uint32_t node_id, int8_t rssi);
    void (*on_node_lost)(uint32_t node_id);
    void (*on_network_joined)(uint32_t gateway_id);
    void (*on_network_lost)(void);
    void (*on_route_changed)(uint32_t dest, uint32_t next_hop, uint8_t hops);
    void (*on_fatal_error)(esp_err_t err, const char *msg);
} mesh_espnow_callbacks_t;
```

## Config Validation

```c
esp_err_t mesh_espnow_validate_config(const mesh_espnow_config_t *cfg, const char **err);
```

---

# 🔄 Lifecycle Functions

| Function | Transition | Description |
|----------|-----------|-------------|
| `mesh_espnow_init(&cfg)` | UNINIT → INIT | Validates config, inits Wi-Fi/ESP-NOW/NVS |
| `mesh_espnow_start()` | INIT → DISCOVERING | Starts beaconing |
| `mesh_espnow_stop()` | any → INIT | Sends goodbye, stops |
| `mesh_espnow_deinit()` | any → UNINIT | Full teardown |
| `mesh_espnow_factory_reset()` | any → reboot | Erase NVS + restart |
| `mesh_espnow_process(now_ms)` | — | Call every 50-100ms |
| `mesh_espnow_process_from_isr()` | — | ISR-safe minimal path |

---

# 📤 Send API

| Function | Description |
|----------|-------------|
| `mesh_espnow_send(dest, data, len, &diag)` | Unicast with ACK + auto route discovery |
| `mesh_espnow_broadcast(data, len)` | Network-wide flood with dup suppression |
| `mesh_espnow_send_to_gateway(data, len)` | Auto-routed to best gateway |
| `mesh_espnow_discover_route(dest)` | Proactive RREQ |
| `mesh_espnow_send_to_subnet(dest, subnet, data, len)` | Send to specific subnet |

## Send Diagnostics

```c
typedef struct {
    uint32_t dest_id;
    esp_err_t result;
    uint32_t discovery_time_ms;
    uint32_t tx_time_ms;
    uint8_t  retries_used;
    uint8_t  hops_taken;
    int8_t   final_rssi;
} mesh_espnow_tx_diag_t;
```

---

# 🌉 Bridge API

| Function | Description |
|----------|-------------|
| `mesh_espnow_bridge_add_subnet(subnet_id, channel)` | Add subnet to bridge |
| `mesh_espnow_bridge_remove_subnet(subnet_id)` | Remove bridged subnet |
| `mesh_espnow_get_subnet()` | Get this node's subnet ID |

---

# 📊 Data Structures

## `mesh_espnow_route_t`

```c
typedef struct {
    uint32_t node_id;
    uint32_t next_hop;
    uint8_t  hop_count;
    int8_t   rssi;
    uint32_t last_seen_ms;
} mesh_espnow_route_t;
```

## `mesh_espnow_neighbor_t`

```c
typedef struct {
    uint32_t node_id;
    int8_t   rssi, rssi_min, rssi_max;
    uint32_t last_seen_ms;
    uint8_t  hop_count, capabilities;
    uint32_t uptime_s;
    uint8_t  subnet_id, subnet_channel;
} mesh_espnow_neighbor_t;
```

## `mesh_espnow_stats_t`

```c
typedef struct {
    uint32_t uptime_ms, tx_packets, tx_bytes;
    uint32_t rx_packets, rx_bytes, forwarded, dropped;
    uint32_t retransmissions, ack_sent, ack_received;
    uint32_t duplicates_detected;
    uint32_t rreqs_sent, rreqs_received, rreps_sent, rreps_received;
    uint16_t neighbor_count, route_count;
    int8_t   avg_rssi;
    uint8_t  avg_hop_count;
    uint32_t gateway_id, parent_id;
    uint32_t battery_mv, heap_free;
    uint32_t boot_count, crash_count;
    float    avg_tx_latency_ms;
    uint32_t peak_tx_latency_ms;
} mesh_espnow_stats_t;
```

---

# 🔍 Info Query Functions

| Function | Returns |
|----------|---------|
| `mesh_espnow_get_node_id()` | `uint32_t` |
| `mesh_espnow_get_state()` | `mesh_espnow_state_t` |
| `mesh_espnow_get_stats(&s)` | `esp_err_t` |
| `mesh_espnow_get_routes(buf, max)` | `uint16_t` (count) |
| `mesh_espnow_get_neighbors(buf, max)` | `uint16_t` (count) |
| `mesh_espnow_get_parent()` | `uint32_t` |
| `mesh_espnow_get_gateway()` | `uint32_t` |
| `mesh_espnow_is_healthy()` | `bool` |
| `mesh_espnow_last_error()` | `const char*` |

---

# 🩺 Diagnostics

| Function | Description |
|----------|-------------|
| `mesh_espnow_diagnostic_scan()` | Full state dump |
| `mesh_espnow_reset_stats()` | Zero all counters |

---

# 🔋 Power Management

| Function | Description |
|----------|-------------|
| `mesh_espnow_sleep()` | Enter deep sleep (does not return) |
| `mesh_espnow_update_battery(mV)` | Report battery voltage |
| `mesh_espnow_estimate_life_s(capacity_mAh)` | Battery life estimate |

---

# 📝 Logging

```c
void mesh_espnow_set_log_level(const char *subsystem, mesh_espnow_log_level_t level);
```

Subsystems: `"mesh"`, `"routing"`, `"reliable"`, `"power"`, `"security"`, `"diag"`

---

# 🧵 Thread Safety

- All public API functions are **thread-safe** (mutex-guarded)
- Do **not** call from ISR (use `mesh_espnow_process_from_isr()`)
- Callbacks fire inside `mesh_espnow_process()` — keep them short

---

<p align="center">
  <img src="https://capsule-render.vercel.app/api?type=waving&color=0:ff8c00,100:ff4500&height=120&section=footer"/>
</p>
