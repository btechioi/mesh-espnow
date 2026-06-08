<p align="center">
  <img src="https://capsule-render.vercel.app/api?type=waving&color=0:ff8c00,100:ff4500&height=180&section=header&text=Deployment%20Guide&fontSize=40&fontAlignY=35&animation=fadeIn&fontColor=ffffff"/>
</p>

# 🚀 Deployment Guide

> **Planning, building, and troubleshooting a real-world ESP-NOW mesh network.**

---

# 🧩 Using with Arduino

The library is fully compatible with Arduino IDE and PlatformIO. All APIs are identical.

- Use `setup()`/`loop()` instead of `app_main()`
- Use `delay(ms)` instead of `vTaskDelay()`
- Use `millis()` instead of `esp_timer_get_time() / 1000`

## Flashing via Arduino IDE

1. Copy `mesh_espnow/` → `~/Arduino/libraries/`
2. **File → Examples → ESP-NOW Mesh Network Library**
3. Select board → **Upload**

## Flashing via PlatformIO

```ini
[env:esp32-c3-devkitm-1]
platform = espressif32
board = esp32-c3-devkitm-1
framework = arduino
lib_deps = https://github.com/btechioi/mesh-espnow
monitor_speed = 115200
```

---

# 🗺️ Network Planning

## 1. Choose Topology

| Topology | Best for | Pros | Cons |
|----------|----------|------|------|
| **Star** | Single gateway, ~50m range | Simple, low latency | Limited range |
| **Tree** | Large area, clear hierarchy | Predictable paths | Single point of failure |
| **Mesh** | Max reliability | Self-healing | Slightly higher latency |

## 2. Choose Node IDs

**Auto** (simpler): `cfg.node_id = 0` → ID from MAC:
```
MAC: AC:67:B2:12:34:56  →  ID: 0xAC67B256
```

**Manual** (production):
```c
#define ID_GATEWAY  0xA1000000
#define ID_ROUTER_1 0xA1000010
#define ID_SENSOR_1 0xA1000101
```

**Suggested scheme:**
```
0xA1 00 00 XX   →  Gateways
0xA1 00 01 XX   →  Routers floor 1
0xA1 01 XX XX   →  Sensors zone 1
```

## 3. Choose Channel

| Channel | Notes |
|---------|-------|
| 1 | Least crowded |
| 6 | Backup if ch1 noisy |
| 11 | Backup if ch1+6 busy |

Use 1, 6, or 11 — they don't overlap.

## 4. Estimate Range

| Environment | Range per hop |
|-------------|--------------|
| Open air, line of sight | ~250m |
| Indoors, same room | ~50m |
| Through 1 wall | ~30m |
| Through 2+ walls | ~10m |
| Through floors | ~5-15m |

Add a **router** when distance exceeds these.

---

# 📋 Configuration Templates

## Mains Gateway

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

## Mains Router

```c
mesh_espnow_config_t cfg = MESH_ESPNOW_CONFIG_DEFAULT();
cfg.channel = 6;
cfg.capabilities = MESH_ESPNOW_CAP_ROUTER;
cfg.power_mode = MESH_ESPNOW_POWER_ALWAYS_ON;
cfg.max_neighbors = 64;
cfg.max_routes = 128;
```

## Battery Sensor (Periodic Deep Sleep)

```c
mesh_espnow_config_t cfg = MESH_ESPNOW_CONFIG_DEFAULT();
cfg.channel = 6;
cfg.capabilities = MESH_ESPNOW_CAP_LEAF | MESH_ESPNOW_CAP_SLEEPY;
cfg.power_mode = MESH_ESPNOW_POWER_DEEP_SLEEP;
cfg.deep_sleep_interval_ms = 60000;
cfg.beacon_interval_ms = 60000;
cfg.max_retransmits = 5;
```

## Battery Sensor (On-Demand Wake)

```c
mesh_espnow_config_t cfg = MESH_ESPNOW_CONFIG_DEFAULT();
cfg.channel = 6;
cfg.capabilities = MESH_ESPNOW_CAP_LEAF | MESH_ESPNOW_CAP_SLEEPY;
cfg.power_mode = MESH_ESPNOW_POWER_DEEP_SLEEP_ON_DEMAND;

void app_main() {
    ESP_ERROR_CHECK(mesh_espnow_init(&cfg));
    ESP_ERROR_CHECK(mesh_espnow_start());
    // do work...
    mesh_espnow_sleep();  // does not return
}
```

## Duty Cycle Sensor

```c
mesh_espnow_config_t cfg = MESH_ESPNOW_CONFIG_DEFAULT();
cfg.channel = 6;
cfg.capabilities = MESH_ESPNOW_CAP_LEAF | MESH_ESPNOW_CAP_SLEEPY;
cfg.power_mode = MESH_ESPNOW_POWER_DUTY_CYCLE;
cfg.beacon_interval_ms = 10000;
cfg.awake_window_ms = 500;
```

---

# 🔋 Power Planning

## Current Draw

| Mode | Avg current | 2×AA (2500mAh) | LiPo (1200mAh) |
|------|-------------|----------------|----------------|
| Always on | 15-20 mA | 5-7 days | 2-3 days |
| Duty cycle 5s | ~130 µA | ~2 years | ~1 year |
| Deep sleep 30s | ~26 µA | ~11 years | ~5 years |
| Deep sleep 60s | ~14 µA | ~20 years | ~10 years |
| On-demand | ~5 µA | ~57 years | ~27 years |

## Battery Voltage Measurement

```
Battery+ ──┬── 100kΩ ──┬── GPIO34 (ADC)
           │           │
           └── 100kΩ ──┘
                          └── GND
```

```c
uint32_t read_battery_mv(void) {
    int raw = adc1_get_raw(ADC1_CHANNEL_6);
    uint32_t mv = raw * 3300 / 4095 * 2;   // 2:1 divider
    return mv;
}
mesh_espnow_update_battery(read_battery_mv());
```

---

# 💾 Flashing Multiple Nodes

## ESP-IDF Mass Programming

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

## Arduino Per-Node Config

Edit values directly in the `.ino` file before uploading.

---

# 🛡️ Security Recommendations

## Before Deploying

1. **Change the default PSK:**
   ```c
   memcpy(cfg.pre_shared_key, "5up3rS3cr3tK3y!", 16);
   ```

2. **Validate all nodes have the same key.**

## What Encryption Protects

| Protected | Not protected |
|-----------|--------------|
| Application payloads | Header (src, dest, hops) |
| Broadcast content | Beacon fields (caps, battery) |
| ACK payload | Node existence |

Headers stay in plaintext so intermediate nodes can forward packets.

---

# 🧪 Testing Your Network

## Stage 1: Two-Node Test

Flash sensor + gateway 1m apart. Verify:
- Both discover each other
- Gateway receives data
- Sensor receives ACK

## Stage 2: Range Test

```
RSSI -30 to -50:   Excellent
RSSI -50 to -70:   Good
RSSI -70 to -80:   Marginal
RSSI -80 to -90:   Poor
RSSI below -90:    No connection
```

## Stage 3: Multi-Hop Test

Add a router. Verify route goes through it. Disconnect → auto-repair.

## Stage 4: Stress Test

| Test | How | Expected |
|------|-----|----------|
| Packet loss | Move to marginal RSSI | Retransmissions |
| Node failure | Unplug router | Routes re-form in 30-60s |
| Battery shift | Change mV | Routes avoid low-battery |
| Broadcast flood | 100 in 10s | All received, no duplicates |
| Deep sleep | 10s cycle | Wake, send, sleep, repeat |
| On-demand | DEEP_SLEEP_ON_DEMAND | Remote send wakes node |

---

# 🔧 Troubleshooting

## "Nodes don't discover each other"

- **Same channel?** `cfg.channel` must match on all nodes
- **Same PSK?** `cfg.pre_shared_key` must match
- **Beacon interval?** Long intervals = slow discovery
- **Distance?** Start at 1m
- **State?** `mesh_espnow_get_state()` should be `CONNECTED`

## "Packets get through sometimes"

- RSSI too low — move closer or add router
- Channel interference — use 1, 6, or 11
- Too many neighbors — increase `max_neighbors`
- Retransmit too low — try `max_retransmits = 5`

## "Node joins then disappears"

- DUTY_CYCLE only listens during `awake_window_ms`
- Deep sleep = full reset; re-appears next cycle
- Set `neighbor_timeout_ms ≥ 3 × beacon_interval_ms`

## "RREQ rate-limited"

Normal. Backoff: 1s → 2s → 4s → 8s → 10s cap.

## "Decryption failed"

PSK mismatch on sender vs receiver.

## Reboot loop

- Bad USB cable / weak power
- `mesh_espnow_process()` not called often enough (need every 100ms)
- Stack overflow — increase `CONFIG_MAIN_TASK_STACK_SIZE`

---

# 🩺 Field Diagnostics

```c
mesh_espnow_diagnostic_scan();
```

Output:
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

# 📈 Scaling Beyond 100 Nodes

## Memory Planning

| Table | Bytes/entry | 256 entries |
|-------|------------|-------------|
| Neighbors | 36 | 9 KB |
| Routes | 20 | 5 KB |
| Retransmit | 300 | 4.8 KB |
| Dup cache | 24 | 3.1 KB |
| **Total** | | **~22 KB** |

## Performance

| Nodes | Neighbors/node | Beacon interval | Latency |
|-------|---------------|-----------------|---------|
| 10 | 3-9 | 3s | 10-30ms |
| 50 | 5-15 | 5s | 20-100ms |
| 100 | 10-30 | 10s | 30-200ms |
| 500 | 20-50 | 30s | 50-500ms |
| 1000 | 30-80 | 60s | 100ms-1s |

## Large Network Config

```c
cfg.beacon_interval_ms = 15000;
cfg.neighbor_timeout_ms = 120000;
cfg.route_timeout_ms = 300000;
cfg.max_neighbors = 64;
cfg.max_routes = 128;
```

---

# 🏢 Example: 50-Node Building

## Hardware

- 1× Gateway (ESP32, mains, near internet router)
- 5× Routers (ESP32, mains, one per floor hallway)
- 44× Sensors (ESP32-C3, battery, in rooms)

## Configuration

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
cfg.deep_sleep_interval_ms = 300000;
cfg.beacon_interval_ms = 300000;
cfg.max_retransmits = 5;
```

## Expected Behavior

- Sensors wake every 5 min, send reading, go back to sleep
- Routers forward traffic 24/7
- Failed router → auto-route through another
- Battery life: ~5 years (2× AA alkaline)

---

<p align="center">
  <img src="https://capsule-render.vercel.app/api?type=waving&color=0:ff8c00,100:ff4500&height=120&section=footer"/>
</p>
