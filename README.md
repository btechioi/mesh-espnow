# ESP-NOW Mesh Network Library v3

**Intelligent, metric-based self-forming/self-healing mesh for ESP32 — no Wi-Fi infrastructure, no IP stack, no ESP-MESH.**

```
┌──────────────┐     ┌──────────────┐     ┌──────────────┐
│ Sensor Leaf  │────▶│  Router      │────▶│  Gateway     │──▶ Internet
│ (battery)    │     │  (mains)     │     │  (root)      │
│ 0xA1000001   │     │  0xA1000005  │     │  0xA1000000  │
└──────────────┘     └──────────────┘     └──────────────┘
       │                    │                    │
       │          ┌─────────┘                    │
       ▼          ▼                              │
┌──────────────┐  │                              │
│ Sensor Leaf  │──┘                              │
│ 0xA1000002   │  Packets auto-route via lowest   │
└──────────────┘  metric path (hops, RSSI, batt)  │
       │                                           │
       ▼                                           │
┌──────────────┐                                   │
│ Sensor Leaf  │───────────────────────────────────┘
│ 0xA1000003   │  (direct to gateway if in range)
└──────────────┘
```

## Features

| Feature | What it means |
|---------|--------------|
| **Intelligent routing** | Composite metric: hop count ×20 + RSSI penalty + battery penalty + capability penalty + reliability penalty |
| **Multi-path (primary + backup)** | Each destination keeps a backup route; auto-promoted on failure without re-discovery |
| **Auto-optimization** | Every 15s, re-evaluates all neighbors for better routes to every destination |
| **Battery-aware** | Prefers mains-powered routers over battery leaf nodes; announced in every beacon |
| **Link quality tracking** | PDR tracked per neighbor via TX success/failure from the reliable delivery layer |
| **Self-healing** | Neighbor gone → backup promoted in <2s; no network-wide flood |
| **Multi-hop** | Up to 32 hops via store-and-forward at each hop |
| **No Wi-Fi needed** | ESP-NOW only — no router, no IP, no ESP-MESH dependency |
| **1000+ nodes** | Linear scaling with memory |
| **Deep sleep** | ~14µA idle — years on a coin cell |
| **Encrypted** | AES-128-CCM every packet, 8-byte MIC |
| **Reliable** | ACK + exponential-backoff retransmission per hop |
| **Config validation** | Every field checked at init; `mesh_espnow_validate_config()` for pre-flight checks |
| **Health monitoring** | Boot count, crash detection via RTC, NVS persistence, periodic diagnostics |
| **Per-subsystem logging** | Individual log levels for: `mesh`, `routing`, `reliable`, `power`, `security`, `diag` |

## Quick Start

### Option A: ESP-IDF

#### 1. Add component to project

```
your_project/
├── CMakeLists.txt
├── main/
│   └── app_main.c
└── components/
    └── mesh_espnow/
        ├── CMakeLists.txt
        ├── Kconfig.projbuild
        ├── idf_component.yml
        ├── include/
        │   └── mesh_espnow.h
        └── src/
            ├── mesh_core.c
            ├── mesh_routing.c
            ├── mesh_reliable.c
            ├── mesh_power.c
            ├── mesh_security.c
            └── mesh_diag.c
```

#### 2. Minimal sensor node

```c
#include "mesh_espnow.h"

static void on_data(uint32_t src, const uint8_t *data, uint16_t len, int8_t rssi) {
    ESP_LOGI("app", "From 0x%08X: %.*s", src, len, data);
}

void app_main(void) {
    mesh_espnow_config_t cfg = MESH_ESPNOW_CONFIG_DEFAULT();

    cfg.channel = 6;
    cfg.callbacks.on_data = on_data;

    ESP_ERROR_CHECK(mesh_espnow_init(&cfg));
    ESP_ERROR_CHECK(mesh_espnow_start());

    while (1) {
        mesh_espnow_process(esp_timer_get_time() / 1000);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
```

#### 3. Gateway node (network root)

```c
mesh_espnow_config_t cfg = MESH_ESPNOW_CONFIG_DEFAULT();
cfg.gateway_mode = true;
cfg.channel = 6;
cfg.power_mode = MESH_ESPNOW_POWER_ALWAYS_ON;

ESP_ERROR_CHECK(mesh_espnow_init(&cfg));
ESP_ERROR_CHECK(mesh_espnow_start());
```

#### 4. Build & flash

```bash
cd examples/01_sensor_node
idf.py set-target esp32c3
idf.py build flash monitor
```

### Option B: Arduino IDE

#### 1. Install the library

Copy the `mesh_espnow/` folder to `~/Arduino/libraries/`, or in PlatformIO add to `platformio.ini`:
```ini
lib_deps = https://github.com/btechioi/mesh-espnow
```

#### 2. Open an example

**File → Examples → ESP-NOW Mesh Network Library → 01_sensor_node** or **02_gateway_node**.

#### 3. Upload

Select your board (ESP32, ESP32-C3, ESP32-S3, etc.) and port, then click **Upload**.

The library uses the same C API (`mesh_espnow.h`). Use `setup()`/`loop()` instead of `app_main()`, and `delay(ms)`/`millis()` instead of FreeRTOS equivalents.

## API Reference

### Configuration

```c
mesh_espnow_config_t cfg = MESH_ESPNOW_CONFIG_DEFAULT();
```

All fields with their defaults, valid ranges, and descriptions:

| Field | Default | Range | Description |
|-------|---------|-------|-------------|
| `node_id` | 0 (MAC→ID) | any 32-bit | 0 = auto-generate from MAC OUI |
| `gateway_mode` | false | bool | true = root node, announces CAP_GATEWAY |
| `capabilities` | ROUTER\|SLEEPY | bitmask | Advertised in every beacon |
| `channel` | 1 | 1-11 | **All nodes must match** |
| `beacon_interval_ms` | 3000 | 100-60000 | How often we broadcast "I'm here" |
| `neighbor_timeout_ms` | 30000 | 5000-300000 | Forgetting silent neighbors |
| `route_timeout_ms` | 60000 | 10000-600000 | Expire unused routes (backup tried first) |
| `retransmit_timeout_ms` | 500 | 100-10000 | Per-hop ACK timeout |
| `power_mode` | DUTY_CYCLE | enum | ALWAYS_ON / DUTY_CYCLE / DEEP_SLEEP / DEEP_SLEEP_ON_DEMAND |
| `deep_sleep_interval_ms` | 5000 | 100-600000 | Deep sleep duration (timer mode only) |
| `awake_window_ms` | 200 | 20-5000 | Per-cycle awake time |
| `max_retransmits` | 3 | 0-10 | Per-hop retries before giving up |
| `ttl` | 32 | 1-64 | Max forwarding distance |
| `max_neighbors` | 32 | 4-128 | Neighbor table size (evicts worst-metric) |
| `max_routes` | 64 | 8-256 | Route table size (evicts highest-metric) |
| `encryption_enabled` | true | bool | AES-128-CCM per-packet |
| `pre_shared_key[16]` | "MESH-ESPNOW-MESH" | 16 bytes | Network key (all nodes must match) |
| `enable_health_monitor` | true | bool | Boot/crash tracking via NVS |
| `callbacks` | all NULL | struct | Event handlers (see below) |

### Lifecycle

| Function | State Transition | Notes |
|----------|-----------------|-------|
| `mesh_espnow_init(&cfg)` | UNINIT → INIT | Validates config, inits Wi-Fi/ESP-NOW/NVS/subsystems |
| `mesh_espnow_start()` | INIT → DISCOVERING | Starts beaconing, neighbor discovery |
| `mesh_espnow_stop()` | any → INIT | Sends goodbye, stops beaconing |
| `mesh_espnow_deinit()` | any → UNINIT | Full teardown, free all memory |
| `mesh_espnow_factory_reset()` | any → reboot | Erases NVS, calls `esp_restart()` |
| `mesh_espnow_process(ms)` | — | Call every 50-100ms in main loop |

### Send API

| Function | Description |
|----------|-------------|
| `mesh_espnow_send(dest, data, len, &diag)` | Unicast with ACK+retransmit; auto route discovery |
| `mesh_espnow_broadcast(data, len)` | Network-wide flood with duplicate suppression |
| `mesh_espnow_send_to_gateway(data, len)` | Auto-routed to best gateway by metric |
| `mesh_espnow_discover_route(dest)` | Proactive RREQ (rate-limited with exponential backoff) |

### Info & Diagnostics

| Function | Returns |
|----------|---------|
| `mesh_espnow_get_node_id()` | uint32_t — this node's ID |
| `mesh_espnow_get_state()` | UNINIT / INIT / DISCOVERING / CONNECTED / SLEEPING / ERROR |
| `mesh_espnow_get_stats(&s)` | esp_err_t — fills `mesh_espnow_stats_t` |
| `mesh_espnow_get_routes(buf, max)` | uint16_t — count written |
| `mesh_espnow_get_neighbors(buf, max)` | uint16_t — count written |
| `mesh_espnow_get_parent()` | uint32_t — next hop to gateway (0 if none) |
| `mesh_espnow_get_gateway()` | uint32_t — gateway ID (0 if none) |
| `mesh_espnow_is_healthy()` | bool — true if CONNECTED with neighbors and gateway |
| `mesh_espnow_last_error()` | const char* — last error description |
| `mesh_espnow_err_to_str(err)` | const char* — any error to string |
| `mesh_espnow_diagnostic_scan()` | — prints full state table to stdout |
| `mesh_espnow_reset_stats()` | — zeros all counters |

### Power Management

| Function | Description |
|----------|-------------|
| `mesh_espnow_sleep()` | Deep sleep (sends goodbye, enters deep sleep; wake via timer or ESP-NOW packet depending on mode) |
| `mesh_espnow_update_battery(mV)` | Update battery reading (routing uses it) |
| `mesh_espnow_estimate_life_s(capacity_mAh)` | Theoretical battery life estimate |

### Callbacks

Set on `cfg.callbacks` before `mesh_espnow_init()`:

```c
void on_data(uint32_t src, const uint8_t *data, uint16_t len, int8_t rssi);
    // Unicast data arrived for this node

void on_broadcast(uint32_t src, const uint8_t *data, uint16_t len);
    // Network-wide broadcast

void on_node_discovered(uint32_t node_id, int8_t rssi);
    // New node in direct radio range

void on_node_lost(uint32_t node_id);
    // Neighbor timed out (gone for neighbor_timeout_ms)

void on_network_joined(uint32_t gateway_id);
    // We now have a route to a gateway (state → CONNECTED)

void on_network_lost(void);
    // No gateway routes remain (state → DISCOVERING)

void on_route_changed(uint32_t dest, uint32_t next_hop, uint8_t hops);
    // Route installed, switched, or metric changed

void on_fatal_error(esp_err_t err, const char *msg);
    // Internal error — should trigger reinit
```

### Logging

```c
mesh_espnow_set_log_level("routing", MESH_ESPNOW_LOG_DEBUG);
mesh_espnow_set_log_level("mesh",    MESH_ESPNOW_LOG_WARN);
```

Subsystems: `"mesh"`, `"routing"`, `"reliable"`, `"power"`, `"security"`, `"diag"`

## Intelligent Routing Architecture

### Route Metric Formula

```
metric = (hop_count × 20)
       + (RSSI_penalty × 2)        // 0 at -50dBm, 100 at -100dBm
       + (battery_penalty × 10)    // per 100mV below 3300
       + capability_penalty         // -30 for gateway, +30 for leaf-only
       + reliability_penalty        // +25 if PDR < 50%, +12 if PDR < 80%
```

**Lower is better.** Gateways get a bonus; battery-powered leaf nodes get a penalty. High PDR (packet delivery ratio) earns better scores.

### Multi-Path (Primary + Backup)

Every destination can have two routes:
- **Primary** — the lowest-metric path (used for all traffic)
- **Backup** — the second-best path (automatically promoted if primary's next hop fails)

When a neighbor is lost, all routes through that neighbor immediately switch to their backup. If no backup exists, the route is invalidated and re-discovery happens on demand.

### Periodic Optimization (Route Aging + Re-evaluation)

Every 15 seconds, the routing engine:
1. Expires neighbors silent for `neighbor_timeout_ms`
2. Expires routes unused for `route_timeout_ms` (tries backup first)
3. **Re-evaluates**: for each known destination, checks if any direct neighbor now offers a better metric than the current primary
4. If a better path exists, demotes primary → backup and installs new primary

This handles topology changes (node moves, interference change, battery status change) without any protocol overhead.

### Route Discovery (RREQ/RREP)

When no route exists:
1. Broadcast RREQ carrying: `[dest_id (4)] [metric (2)]`
2. Every neighbor learns a reverse route to the source
3. If a neighbor **is** the destination or **has a route** to it, it replies with an RREP
4. RREP travels back via reverse path, installing forward routes
5. **Exponential backoff**: RREQ retry waits `1s, 2s, 4s...` per destination (capped at 10s)

### Beacon Protocol (14 bytes)

Every `beacon_interval_ms`:

```
[0]   capabilities (1 byte)
[1]   hop count to gateway (1 byte)
[2-5] gateway node ID (4 bytes)
[6-9] uptime seconds (4 bytes)
[10-13] battery mV (4 bytes)
```

Battery information enables power-aware routing across the network without additional packets.

## Power Guide

| Mode | Avg Current | 250mAh Life | 3400mAh Life |
|------|-------------|-------------|-------------|
| `ALWAYS_ON` (mains router) | ~15 mA | 17 hours | 9 days |
| `DUTY_CYCLE` (5s interval) | ~130 µA | 80 days | 3 years |
| `DEEP_SLEEP` (30s timer) | ~26 µA | 400 days | 15 years |
| `DEEP_SLEEP` (60s timer) | ~14 µA | 2 years | 28 years |
| `DEEP_SLEEP_ON_DEMAND` | ~5 µA | 5.7 years | 78 years |

### Battery Node Best Practices

- Use `MESH_ESPNOW_POWER_DEEP_SLEEP` with `deep_sleep_interval_ms ≥ 30000` for periodic reporting
- Use `MESH_ESPNOW_POWER_DEEP_SLEEP_ON_DEMAND` for event-triggered nodes that wake only when another node sends them a packet
- Set capabilities: `MESH_ESPNOW_CAP_LEAF | MESH_ESPNOW_CAP_SLEEPY`
- Call `mesh_espnow_update_battery(mV)` before sleep
- On wake: call `init()`, `start()`, send data, call `sleep()`
- The node does NOT forward traffic for others — saves battery

### Remote Wake (On-Demand Deep Sleep)

A node in `DEEP_SLEEP_ON_DEMAND` mode sleeps indefinitely until another node sends it a packet:

```c
// Sleeping node configuration
cfg.power_mode = MESH_ESPNOW_POWER_DEEP_SLEEP_ON_DEMAND;
cfg.capabilities = MESH_ESPNOW_CAP_LEAF | MESH_ESPNOW_CAP_SLEEPY;

// After start(), the node stays awake until it calls:
mesh_espnow_sleep();  // enters deep sleep, waits for ESP-NOW packet
```

```c
// Another node wakes it by sending a normal DATA packet:
mesh_espnow_send(0xA1000042, "wake up!", 8, NULL);
```

The sleeping node wakes, the packet is delivered via `on_data` callback, and `app_main()` runs fresh. The node can then process the command and go back to sleep.

## Troubleshooting

| Problem | Likely Fix |
|---------|-----------|
| No neighbors found | **Same channel?** All nodes must match `cfg.channel` |
| Can't reach gateway | Gateway must be on + within range of at least one node |
| High packet loss | Try channel 1, 6, or 11 (non-overlapping); reduce distance |
| Route flapping | Check battery levels on intermediate routers |
| Decryption errors | **Same `pre_shared_key`** on every node |
| Node won't join after deep sleep | That's expected — deep sleep = full reset; just re-init |
| On-demand node doesn't wake | Sender must send to the sleeping node's exact node ID. ESP-NOW packets wake the receiver automatically. |
| RREQ rate-limited | Normal; backoff doubles each attempt (1s → 2s → 4s → 8s) |
| Build errors | Need ESP-IDF ≥ 4.4 (v5.x recommended) |

## Security

- Every DATA and BROADCAST packet encrypted with **AES-128-CCM**
- 8-byte MIC (message integrity code) appended to each ciphertext
- All nodes share a 16-byte `pre_shared_key`
- **Change the default PSK in production** — it's compiled into the binary
- The `MESH_ESPNOW_CONFIG_DEFAULT()` key is literally `"MESH-ESPNOW-MESH"`

## Error Codes

| Code | Value | Meaning |
|------|-------|---------|
| `MESH_ESPNOW_ERR_INVALID_STATE` | 0x0061 | Wrong state for operation (e.g., send before start) |
| `MESH_ESPNOW_ERR_NO_ROUTE` | 0x0062 | No path to destination |
| `MESH_ESPNOW_ERR_RATE_LIMITED` | 0x0063 | RREQ sent too recently; exponential backoff |
| `MESH_ESPNOW_ERR_NO_GATEWAY` | 0x0064 | No gateway known to network |
| `MESH_ESPNOW_ERR_DUPLICATE` | 0x0065 | Packet already seen (dup cache) |
| `MESH_ESPNOW_ERR_INVALID_PARAM` | 0x0066 | Null pointer or bad argument |
| `MESH_ESPNOW_ERR_PAYLOAD_TOO_BIG` | 0x0067 | Exceeds 240 bytes |
| `MESH_ESPNOW_ERR_NOT_INITIALIZED` | 0x0068 | `init()` not called |
| `MESH_ESPNOW_ERR_ALREADY_INIT` | 0x0069 | `init()` already called |
| `MESH_ESPNOW_ERR_DECRYPT_FAILED` | 0x006A | MIC mismatch — wrong key or corruption |
| `MESH_ESPNOW_ERR_CONFIG_INVALID` | 0x006B | Config rejected by validator |

Convert to string: `mesh_espnow_err_to_str(err)`.

## Performance Tuning

### For Max Throughput

```c
cfg.beacon_interval_ms = 500;    // fast neighbor discovery
cfg.retransmit_timeout_ms = 200; // aggressive retry
cfg.max_retransmits = 5;         // reliability at cost of latency
cfg.power_mode = MESH_ESPNOW_POWER_ALWAYS_ON;
```

### For Max Battery Life (Periodic)

```c
cfg.beacon_interval_ms = 30000;  // rarely beacon
cfg.power_mode = MESH_ESPNOW_POWER_DEEP_SLEEP;
cfg.deep_sleep_interval_ms = 60000; // sleep 1 minute
cfg.capabilities = MESH_ESPNOW_CAP_LEAF; // won't route others
```

### For Event-Triggered (Deep Sleep On-Demand)

```c
cfg.power_mode = MESH_ESPNOW_POWER_DEEP_SLEEP_ON_DEMAND;
cfg.capabilities = MESH_ESPNOW_CAP_LEAF;
// Node sleeps until another node sends it a packet.
// Call mesh_espnow_sleep() when ready to sleep.
```

### For Large Networks (100+ nodes)

```c
cfg.max_neighbors = 64;
cfg.max_routes = 128;
cfg.neighbor_timeout_ms = 120000;  // 2 min
cfg.route_timeout_ms = 300000;     // 5 min
```

## State Machine

```
UNINITIALIZED ──init()──▶ INIT ──start()──▶ DISCOVERING ──gateway found──▶ CONNECTED
     ▲                        ▲                 │                            │
     │                        │                 │ gateway lost               │
     └──deinit()──┘◀──stop()──┘◀────────────────┘◀───────────────────────────┘
                        │                          │
                        │ sleep()                  │ error
                        ▼                          ▼
                     SLEEPING                   ERROR ──deinit()──▶ UNINITIALIZED
```

## Examples

### 01_sensor_node

Battery-powered leaf node that reads simulated temperature/humidity and reports to gateway every 30 seconds. Demonstrates:
- Callbacks: `on_data`, `on_broadcast`, `on_node_discovered`, `on_node_lost`, `on_network_joined`, `on_network_lost`
- DUTY_CYCLE power mode
- `mesh_espnow_send_to_gateway()`
- `mesh_espnow_update_battery()`
- `mesh_espnow_diagnostic_scan()`

```bash
cd examples/01_sensor_node
idf.py set-target esp32c3
idf.py build flash monitor
```

### 02_gateway_node

Mains-powered root node with frequent beacons. Demonstrates:
- `gateway_mode = true`
- `MESH_ESPNOW_POWER_ALWAYS_ON`
- Fixed node ID
- Route change tracking
- Periodic stats logging

```bash
cd examples/02_gateway_node
idf.py set-target esp32c3
idf.py build flash monitor
```

## Network Topology Design

### Star (single gateway, all nodes within 1 hop)

Best for: small networks (< 50 nodes), low latency.

```
        ┌── Leaf ──┐
     ┌──┤          ├──┐
  Leaf  │ Gateway  │  Leaf
     └──┤          ├──┘
        └── Leaf ──┘
```

### Tree (gateway + routers + leaves)

Best for: large areas, multi-floor buildings.

```
        Gateway
          │
     ──── Router ────
     │       │       │
   Leaf   Router   Leaf
             │
           Leaf
```

### Mesh (every node routes)

Best for: maximum reliability, ad-hoc deployment.

```
  Leaf ── Router ── Leaf
    │       │        │
  Router ──Gateway── Router
    │       │        │
  Leaf ── Router ── Leaf
```

**Route metric** ensures traffic naturally flows through routers (not battery leaves). The auto-optimization pass continuously validates choices.

## Internal Architecture

```
┌──────────────────────────────────────────────┐
│  mesh_espnow.h (public API)                   │
│  mesh_priv.h  (internal shared state)         │
├──────────────────────────────────────────────┤
│                                                │
│  mesh_core.c ───── State machine, ESP-NOW      │
│       │          callbacks, packet builder     │
│       │                                        │
│  ┌────┴────────┐  ┌──────────────────┐        │
│  │ mesh_routing│  │ mesh_reliable    │        │
│  │ - metric    │  │ - ACK tracking   │        │
│  │ - multi-path│  │ - retransmission │        │
│  │ - optimize  │  │ - latency stats  │        │
│  │ - PDR track │  │ - backoff        │        │
│  └────┬────────┘  └───────┬──────────┘        │
│       │                   │                    │
│  ┌────┴────────┐  ┌───────┴──────────┐        │
│  │ mesh_power  │  │ mesh_security    │        │
│  │ - duty cycle│  │ - AES-128-CCM   │        │
│  │ - deep sleep│  │ - encrypt/decrypt│        │
│  └────┬────────┘  └──────────────────┘        │
│       │                                        │
│  ┌────┴────────┐                               │
│  │ mesh_diag   │                               │
│  │ - boot count│                               │
│  │ - crash det │                               │
│  │ - NVS persis│                               │
│  └─────────────┘                               │
└──────────────────────────────────────────────┘
```

### Key Design Decisions

- **No dynamic allocation after init**: neighbor/route/reliable tables are fixed-size arrays. Eviction uses metric-based worst-entry replacement.
- **No blocking calls in RX path**: ESP-NOW receive callback does minimal work; heavy processing deferred to `mesh_espnow_process()`.
- **Global mutex**: all public APIs lock before touching shared state. ISR context uses a separate minimal path.
- **On-wire format**: 24-byte header with 32-bit node IDs, sequence numbers, and subnet support. MIC tag adds 8 bytes when encryption is enabled.
- **Encryption is optional**: if disabled, 8-byte MIC overhead is eliminated and throughput increases ~5%.

## License

MIT — do whatever you want.
