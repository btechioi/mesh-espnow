# Deployment Guide

> **Planning, building, and troubleshooting a real-world ESP-NOW mesh network.**

---

## Using with Arduino

The library is fully compatible with the Arduino IDE and PlatformIO. All APIs are identical. Use `setup()`/`loop()` instead of `app_main()`, `delay(ms)` instead of `vTaskDelay()`, and `millis()` instead of `esp_timer_get_time() / 1000`.

### Flashing via Arduino IDE

1. Copy `mesh_espnow/` to `~/Arduino/libraries/`
2. Open **File → Examples → ESP-NOW Mesh Network Library**
3. Select your board (e.g., ESP32-C3 Dev Module)
4. Click **Upload**

### Flashing via PlatformIO

In `platformio.ini`:
```ini
[env:esp32-c3-devkitm-1]
platform = espressif32
board = esp32-c3-devkitm-1
framework = arduino
lib_deps = https://github.com/btechioi/mesh-espnow
monitor_speed = 115200
```

---

## Network Planning

### 1. Choose your topology

| Topology | Best for | Pros | Cons |
|----------|----------|------|------|
| **Star** | Single gateway, all nodes within ~50m | Simple, low latency | Limited range |
| **Tree** | Large area, clear hierarchy | Predictable paths, easy debugging | Single point of failure at each branch |
| **Mesh** | Ad-hoc, maximum reliability | Self-healing, no single point of failure | Slightly higher latency |

### 2. Choose your node IDs

Every node needs a unique 32-bit ID.

#### Auto-generated (simpler)

Set `cfg.node_id = 0`. The library generates an ID from the ESP32's MAC address:

```
MAC:  AC:67:B2:12:34:56
ID:   0xAC67B256
```

#### Manual (for production)

```c
#define ID_GATEWAY  0xA1000000
#define ID_ROUTER_1 0xA1000010
#define ID_SENSOR_1 0xA1000101
```

**Naming scheme suggestion**:

```
0xA1 00 00 XX   →  Gateways
0xA1 00 01 XX   →  Routers floor 1
0xA1 01 XX XX   →  Sensors zone 1
```

### 3. Choose your channel

| Channel | Notes |
|---------|-------|
| 1 | Least crowded in most environments |
| 6 | Good if channel 1 is noisy |
| 11 | Good if channels 1 and 6 are busy |

**Rule**: Use 1, 6, or 11. They don't overlap.

### 4. Estimate range

| Environment | Typical range (per hop) |
|-------------|------------------------|
| Open air, line of sight | ~250m |
| Indoors, same room | ~50m |
| Indoors, through 1 wall | ~30m |
| Through floors | ~5-15m |

Add a **router node** every time the expected distance exceeds these ranges.

---

## Configuration Templates

### Mains-powered gateway (always on)

```c
mesh_espnow_config_t cfg = MESH_ESPNOW_CONFIG_DEFAULT();
cfg.gateway_mode = true;
cfg.channel = 6;
cfg.capabilities = MESH_ESPNOW_CAP_GATEWAY;
cfg.power_mode = MESH_ESPNOW_POWER_ALWAYS_ON;
cfg.beacon_interval_ms = 1000;
cfg.max_neighbors = 64;
cfg.max_routes = 128;
```

### Mains-powered router

```c
mesh_espnow_config_t cfg = MESH_ESPNOW_CONFIG_DEFAULT();
cfg.channel = 6;
cfg.capabilities = MESH_ESPNOW_CAP_ROUTER;
cfg.power_mode = MESH_ESPNOW_POWER_ALWAYS_ON;
cfg.max_neighbors = 64;
cfg.max_routes = 128;
```

### Battery-powered sensor (periodic deep sleep)

```c
mesh_espnow_config_t cfg = MESH_ESPNOW_CONFIG_DEFAULT();
cfg.channel = 6;
cfg.capabilities = MESH_ESPNOW_CAP_LEAF | MESH_ESPNOW_CAP_SLEEPY;
cfg.power_mode = MESH_ESPNOW_POWER_DEEP_SLEEP;
cfg.deep_sleep_interval_ms = 60000;   // wake once per minute
cfg.beacon_interval_ms = 60000;
cfg.max_retransmits = 5;
```

### Battery-powered sensor (on-demand — wake on ESP-NOW packet)

```c
mesh_espnow_config_t cfg = MESH_ESPNOW_CONFIG_DEFAULT();
cfg.channel = 6;
cfg.capabilities = MESH_ESPNOW_CAP_LEAF | MESH_ESPNOW_CAP_SLEEPY;
cfg.power_mode = MESH_ESPNOW_POWER_DEEP_SLEEP_ON_DEMAND;
```

Application flow:

```c
void app_main(void) {   // or setup()
    ESP_ERROR_CHECK(mesh_espnow_init(&cfg));
    ESP_ERROR_CHECK(mesh_espnow_start());
    // ... do work ...
    mesh_espnow_sleep();  // does not return
}
```

### Sensor with duty cycling

```c
mesh_espnow_config_t cfg = MESH_ESPNOW_CONFIG_DEFAULT();
cfg.channel = 6;
cfg.capabilities = MESH_ESPNOW_CAP_LEAF | MESH_ESPNOW_CAP_SLEEPY;
cfg.power_mode = MESH_ESPNOW_POWER_DUTY_CYCLE;
cfg.beacon_interval_ms = 10000;
cfg.awake_window_ms = 500;
```

---

## Power Planning

### Current draw estimates

| Mode | Configuration | Avg current | 2×AA (2500mAh) | LiPo (1200mAh) |
|------|--------------|-------------|----------------|----------------|
| Always on | Gateway/router | 15-20 mA | 5-7 days | 2-3 days |
| Duty cycle | 5s interval | ~130 µA | ~2 years | ~1 year |
| Deep sleep | 30s interval | ~26 µA | ~11 years | ~5 years |
| Deep sleep | 60s interval | ~14 µA | ~20 years | ~10 years |
| On-demand | ESP-NOW wake | ~5 µA | ~57 years | ~27 years |

### Measuring battery voltage

```
Battery+ ──┬── 100kΩ ──┬── GPIO34 (ADC)
           │           │
           └── 100kΩ ──┘
                          └── GND
```

```c
uint32_t read_battery_mv(void) {
    int raw = adc1_get_raw(ADC1_CHANNEL_6);
    uint32_t mv = raw * 3300 / 4095 * 2;   // *2 for 2:1 divider
    return mv;
}
mesh_espnow_update_battery(read_battery_mv());
```

---

## Flashing Multiple Nodes

### ESP-IDF mass programming

```bash
# Build once
idf.py build

# Flash to many boards
for PORT in /dev/ttyACM{0,1,2,3,4}; do
    esptool.py --port $PORT write_flash \
        0x10000 build/mesh_espnow.bin \
        0x8000 build/partition_table/partition-table.bin \
        0x1000 build/bootloader/bootloader.bin
done
```

### Arduino IDE per-node config

For per-node configuration in Arduino, use build flags or edit config values directly in the `.ino` file before uploading.

---

## Security Recommendations

### Before deploying

1. **Change the default PSK**

```c
// Default: "MESH-ESPNOW-MESH" — DON'T USE IN PRODUCTION
memcpy(cfg.pre_shared_key, "5up3rS3cr3tK3y!", 16);
```

2. **Validate all nodes have the same key**

### What encryption protects

| Protected | Not protected |
|-----------|--------------|
| Application data payloads | Packet header (src_id, dest_id, etc.) |
| Broadcast content | Beacon content (capabilities, battery) |
| ACK payload | Node existence on the network |

---

## Testing Your Network

### Stage 1: Two-node test

Flash sensor + gateway examples 1m apart. Verify:
- Both discover each other
- Gateway receives data from sensor
- Sensor receives ACK

### Stage 2: Range test

Move the sensor further away while monitoring RSSI:

```
RSSI -30 to -50:   Excellent
RSSI -50 to -70:   Good
RSSI -70 to -80:   Marginal
RSSI -80 to -90:   Poor
RSSI below -90:    Likely no connection
```

Use `mesh_espnow_diagnostic_scan()` or the `on_data` callback's `rssi` parameter.

### Stage 3: Multi-hop test

Add a router between sensor and gateway. Verify:
- Sensor discovers both gateway and router
- Route goes through router
- Disconnect router → sensor finds alternate path

### Stage 4: Stress test

| Test | How | Expected |
|------|-----|----------|
| Packet loss at range | Move to marginal RSSI | Some retransmissions, eventual delivery |
| Node failure | Unplug a router | Routes re-form within 30-60s |
| Battery reporting | Change battery in code | Routes shift away from low-battery nodes |
| Broadcast flood | Send 100 broadcasts in 10s | All nodes receive all |
| Deep sleep cycle | DEEP_SLEEP, 10s interval | Node wakes, sends, sleeps, repeats |
| On-demand wake | DEEP_SLEEP_ON_DEMAND, sleep() | Node sleeps; remote send wakes it |

---

## Troubleshooting

### "Nodes don't discover each other"

- **Same channel?** Every node must use the same `cfg.channel`
- **Same PSK?** Every node must use the same `cfg.pre_shared_key`
- **Beacon mismatch?** A node with `beacon_interval_ms = 30000` takes 30s to appear
- **Distance?** Start at 1m and move apart
- **Check state**: `mesh_espnow_get_state()` should return `CONNECTED`

### "Packets get through sometimes but not always"

- **RSSI too low**: Move nodes closer or add a router
- **Channel interference**: Use 1, 6, or 11
- **Too many neighbors**: Increase `cfg.max_neighbors`
- **Retransmit too low**: `cfg.max_retransmits = 5`, `cfg.retransmit_timeout_ms = 300`

### "Node joins but then disappears"

- **Power saving**: DUTY_CYCLE only listens during `awake_window_ms`
- **Deep sleep**: Node resets. Reappears after next wake cycle.
- **Neighbor timeout**: Set `neighbor_timeout_ms ≥ 3 × beacon_interval_ms`

### "RREQ rate-limited" errors

Normal. Exponential backoff: 1s → 2s → 4s → 8s → 10s cap. If not found after 5 tries, destination is likely offline.

### "Decryption failed" errors

The receiving node's `pre_shared_key` doesn't match the sender's. Check every node uses the same key.

### "esp_now_send returned ESP_ERR_ESPNOW_NOT_INIT"

`mesh_espnow_start()` must be called after `mesh_espnow_init()`.

```c
ESP_ERROR_CHECK(mesh_espnow_init(&cfg));
ESP_ERROR_CHECK(mesh_espnow_start());
```

### Node reboots in a loop

1. **Power**: Bad USB cable or weak power supply
2. **Watchdog**: Call `mesh_espnow_process()` at least every 100ms
3. **Stack overflow**: Increase `CONFIG_MAIN_TASK_STACK_SIZE` (ESP-IDF) or use default stack (Arduino)

---

## Field Diagnostics

Run `mesh_espnow_diagnostic_scan()` from any node:

```
=== MESH DIAGNOSTIC SCAN ===
State: CONNECTED  Uptime: 3600s  Node: 0xA1000042  Ver: 0x03

--- Neighbors (3/32) ---
  0xA1000000  RSSI:-55  CAPS:GW   Batt:0     Seen:1.2s
  0xA1000010  RSSI:-62  CAPS:ROUT Batt:3200  Seen:3.1s

--- Routes (2/64) ---
  Dest:0xA1000000  Next:0xA1000000  Hops:1  PRIMARY
  Dest:0xA1000050  Next:0xA1000010  Hops:2  PRIMARY

--- Stats ---
  Sent:254  Recv:3892  Fwd:1024  Lost:12
  RREQs:3  RouteRepairs:1  AvgRSSI:-65
```

---

## Scaling Beyond 100 Nodes

### Memory planning

| Table | Bytes per entry | Memory for 256 entries |
|-------|----------------|----------------------|
| Neighbors | 36 | 9 KB |
| Routes | 20 | 5 KB |
| Retransmit | 300 | 4.8 KB |
| Duplicate cache | 24 | 3.1 KB |
| **Total** | | **~22 KB** (out of ~300 KB available) |

### Performance at scale

| Nodes | Neighbor count per node | Beacon interval | Typical latency |
|-------|------------------------|-----------------|----------------|
| 10 | 3-9 | 3s | 10-30ms |
| 50 | 5-15 | 5s | 20-100ms |
| 100 | 10-30 | 10s | 30-200ms |
| 500 | 20-50 | 30s | 50-500ms |
| 1000 | 30-80 | 60s | 100ms-1s |

### Large network configuration

```c
cfg.beacon_interval_ms = 15000;
cfg.neighbor_timeout_ms = 120000;
cfg.route_timeout_ms = 300000;
cfg.max_neighbors = 64;
cfg.max_routes = 128;
```

---

## Example: 50-Node Building Deployment

### Hardware

- 1× Gateway (ESP32, mains, near internet router)
- 5× Routers (ESP32, mains, one per floor hallway)
- 44× Sensors (ESP32-C3, battery, in rooms)

### Configuration

```c
// Gateway
cfg.gateway_mode = true;
cfg.beacon_interval_ms = 1000;

// Routers (all 5)
cfg.channel = 6;
cfg.capabilities = MESH_ESPNOW_CAP_ROUTER;
cfg.power_mode = MESH_ESPNOW_POWER_ALWAYS_ON;
cfg.max_neighbors = 64;

// Sensors (all 44)
cfg.channel = 6;
cfg.capabilities = MESH_ESPNOW_CAP_LEAF | MESH_ESPNOW_CAP_SLEEPY;
cfg.power_mode = MESH_ESPNOW_POWER_DEEP_SLEEP;
cfg.deep_sleep_interval_ms = 300000;   // report every 5 minutes
cfg.beacon_interval_ms = 300000;
cfg.max_retransmits = 5;
```

### Expected behavior

- Sensors wake every 5 minutes, send reading, go back to sleep
- Routers forward traffic 24/7
- If a router fails, sensors auto-route through another router
- Gateway logs all sensor data
- Battery life: ~5 years for sensors (2× AA alkaline)
