# Deployment Guide

> **Planning, building, and troubleshooting a real-world ESP-NOW mesh network.**

This guide covers everything from choosing node IDs to field troubleshooting. For basic setup, see [GETTING_STARTED.md](GETTING_STARTED.md).

---

## Network Planning

### 1. Choose your topology

| Topology | Best for | Pros | Cons |
|----------|----------|------|------|
| **Star** | Single gateway, all nodes within ~50m | Simple, low latency, no routing needed | Limited range |
| **Tree** | Large area, clear hierarchy (building floors) | Predictable paths, easy debugging | Single point of failure at each branch |
| **Mesh** | Ad-hoc, maximum reliability | Self-healing, no single point of failure | More complex, slightly higher latency |

### 2. Choose your node IDs

Every node needs a unique 32-bit ID. There are two approaches:

#### Auto-generated (simpler)

Set `cfg.node_id = 0`. The library generates an ID from the ESP32's MAC address:

```
MAC:  AC:67:B2:12:34:56
ID:   0xAC67B256  (last 3 bytes of MAC)
```

IDs are almost certainly unique. But you can't easily tell which node is which.

#### Manual (for production)

Assign IDs based on role:

```c
#define ID_GATEWAY  0xA1000000
#define ID_ROUTER_1 0xA1000010
#define ID_ROUTER_2 0xA1000020
#define ID_SENSOR_1 0xA1000101
#define ID_SENSOR_2 0xA1000102
...
```

**Naming scheme suggestion**:

```
0xA1 00 00 XX   →  Gateways (00-FF)
0xA1 00 01 XX   →  Routers floor 1
0xA1 00 02 XX   →  Routers floor 2
0xA1 01 XX XX   →  Sensors zone 1
0xA1 02 XX XX   →  Sensors zone 2
```

### 3. Choose your channel

| Channel | Notes |
|---------|-------|
| 1 | Least crowded in most environments |
| 6 | Good if channel 1 is noisy |
| 11 | Good if channels 1 and 6 are busy |
| 2-5, 7-10 | May overlap with adjacent channels |

**Rule of thumb**: Use 1, 6, or 11. They don't overlap. If you have existing Wi-Fi, scan which channels are least used:

```bash
# Use a phone Wi-Fi analyzer app, or
# On an ESP32, run the Wi-Fi scan example
```

### 4. Estimate range

| Environment | Typical range (per hop) |
|-------------|------------------------|
| Open air, line of sight | ~250m |
| Indoors, same room | ~50m |
| Indoors, through 1 wall | ~30m |
| Indoors, through 2+ walls | ~10m |
| Through floors | ~5-15m |

**Add a router node** every time the expected distance exceeds these ranges.

---

## Configuration Templates

### Mains-powered gateway (always on)

```c
mesh_espnow_config_t cfg = MESH_ESPNOW_CONFIG_DEFAULT();
cfg.gateway_mode = true;
cfg.channel = 6;
cfg.capabilities = MESH_ESPNOW_CAP_GATEWAY;
cfg.power_mode = MESH_ESPNOW_POWER_ALWAYS_ON;
cfg.beacon_interval_ms = 1000;       // fast beacons as root
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
cfg.beacon_interval_ms = 60000;       // send one beacon per wake cycle
cfg.max_retransmits = 5;              // extra chance before sleep
```

### Battery-powered sensor (on-demand — wake on ESP-NOW packet)

```c
mesh_espnow_config_t cfg = MESH_ESPNOW_CONFIG_DEFAULT();
cfg.channel = 6;
cfg.capabilities = MESH_ESPNOW_CAP_LEAF | MESH_ESPNOW_CAP_SLEEPY;
cfg.power_mode = MESH_ESPNOW_POWER_DEEP_SLEEP_ON_DEMAND;
// No deep_sleep_interval_ms needed — wakes only when another node sends a packet
```

The application flow for on-demand:
```c
void app_main(void) {
    // Init + start (this runs fresh after every wake)
    ESP_ERROR_CHECK(mesh_espnow_init(&cfg));
    ESP_ERROR_CHECK(mesh_espnow_start());

    // The node is awake. Do work, send data, etc.
    // ...

    // When done, go to sleep until another node calls us
    mesh_espnow_sleep();  // does not return
}

void on_data(uint32_t src, const uint8_t *data, uint16_t len, int8_t rssi) {
    // This runs when we wake from ESP-NOW packet
    process_command(src, data, len);

    // If this is a "stay awake for more" command, don't sleep yet
    // Otherwise, the loop will call mesh_espnow_sleep() when work is done
}
```

### Sensor with duty cycling

```c
mesh_espnow_config_t cfg = MESH_ESPNOW_CONFIG_DEFAULT();
cfg.channel = 6;
cfg.capabilities = MESH_ESPNOW_CAP_LEAF | MESH_ESPNOW_CAP_SLEEPY;
cfg.power_mode = MESH_ESPNOW_POWER_DUTY_CYCLE;
cfg.beacon_interval_ms = 10000;       // beacon every 10s
cfg.awake_window_ms = 500;            // listen for 500ms then sleep radio
```

---

## Power Planning

### Current draw estimates

| Mode | Configuration | Avg current | 2×AA (2500mAh) | LiPo (1200mAh) | D-cell (17000mAh) |
|------|--------------|-------------|----------------|----------------|-------------------|
| Always on | Gateway/router | 15-20 mA | 5-7 days | 2-3 days | 35-47 days |
| Duty cycle | 5s interval | ~130 µA | ~2 years | ~1 year | ~15 years |
| Duty cycle | 30s interval | ~45 µA | ~6 years | ~3 years | ~43 years |
| Deep sleep | 30s interval | ~26 µA | ~11 years | ~5 years | ~75 years |
| Deep sleep | 60s interval | ~14 µA | ~20 years | ~10 years | ~139 years |
| Deep sleep on-demand | ESP-NOW wake | ~5 µA | ~57 years | ~27 years | ~388 years |

### Battery type recommendations

| Battery type | Best for | Notes |
|-------------|----------|-------|
| **Lithium-ion 18650** | Routers (always on) | 2500-3500mAh, rechargeable |
| **LiPo pouch** | Portable sensors | 500-2000mAh, lightweight |
| **Alkaline AA** | Leaf nodes | 2000-3000mAh, cheap, disposable |
| **Lithium AA** | Cold environments | 3000-3500mAh, works to -40°C |

### Measuring battery voltage

ESP32 has an internal ADC that can measure battery voltage through a voltage divider:

```
Battery+ ──┬── 100kΩ ──┬── GPIO34 (ADC)
           │           │
           └── 100kΩ ──┘
                          └── GND
```

For a 2:1 divider on a 4.2V LiPo:
- ADC reads 0-2.1V (half of battery voltage)
- ESP32 ADC reference is ~1.1V (attenuation needed)
- Use `adc1_config_channel_atten(ADC1_CHANNEL_X, ADC_ATTEN_DB_11)` for 0-3.6V range

```c
// Read battery (simplified — calibrate for your board)
uint32_t read_battery_mv(void) {
    int raw = adc1_get_raw(ADC1_CHANNEL_6);  // GPIO34
    // Calibration values depend on your voltage divider
    uint32_t mv = raw * 3300 / 4095 * 2;      // *2 for 2:1 divider
    return mv;
}

// Report to mesh
mesh_espnow_update_battery(read_battery_mv());
```

---

## Flashing Multiple Nodes

### Mass programming

For 10+ nodes, use the `esptool.py` command line directly:

```bash
# Build once
idf.py build

# Flash to many boards (change PORT for each)
for PORT in /dev/ttyACM{0,1,2,3,4}; do
    esptool.py --port $PORT write_flash \
        0x10000 build/ mesh_espnow.bin \
        0x8000 build/partition_table/partition-table.bin \
        0x1000 build/bootloader/bootloader.bin
done
```

### Per-node configuration at build time

Use Kconfig to set node-specific values without editing source:

```c
// In main.c
#include "sdkconfig.h"

mesh_espnow_config_t cfg = MESH_ESPNOW_CONFIG_DEFAULT();
cfg.node_id = CONFIG_MESH_NODE_ID;
cfg.channel = CONFIG_MESH_CHANNEL;
```

Then set via menuconfig or sdkconfig:

```bash
idf.py menuconfig
# Navigate to Mesh ESP-NOW configuration → set Node ID, Channel, etc.
```

For production, pre-generate sdkconfig files per node:

```bash
# node1_sdkconfig
CONFIG_MESH_NODE_ID=0xA1000001
CONFIG_MESH_CHANNEL=6

# node2_sdkconfig
CONFIG_MESH_NODE_ID=0xA1000002
CONFIG_MESH_CHANNEL=6

# Build node 1
cp node1_sdkconfig sdkconfig
idf.py build -B build_node1
cp build_node1/mesh-example.bin firmware_node1.bin

# Build node 2
cp node2_sdkconfig sdkconfig
idf.py build -B build_node2
cp build_node2/mesh-example.bin firmware_node2.bin
```

---

## Security Recommendations

### Before deploying

1. **Change the default PSK**

```c
// Default (DON'T USE IN PRODUCTION):
// "MESH-ESPNOW-MESH"  <-- in the default config macro

// Your production key (exactly 16 bytes):
memcpy(cfg.pre_shared_key, "5up3rS3cr3tK3y!", 16);
```

2. **Validate all nodes have the same key**

3. **Plan for key rotation**
   - Flash new firmware with updated key
   - Or use a rolling key scheme (future feature)

### What encryption protects

| Protected | Not protected |
|-----------|--------------|
| Application data payloads | Packet header (src_id, dest_id, hops) |
| Broadcast content | Beacon content (capabilities, battery) |
| ACK payload | Node existence on the network |

The packet header stays in plaintext because intermediate nodes need to read `dest_id` to forward packets. If an attacker can hear your packets, they can see `node A sent a packet to node B` but not what the packet says.

---

## Testing Your Network

### Stage 1: Two-node test

Flash the sensor and gateway examples. Place them 1m apart. Verify:
- Both discover each other (`on_node_discovered` fires)
- Gateway receives data from sensor
- Sensor receives ACK

### Stage 2: Range test

Move the sensor further away while monitoring RSSI:

```
RSSI -30 to -50:   Excellent (close range)
RSSI -50 to -70:   Good
RSSI -70 to -80:   Marginal (some packet loss)
RSSI -80 to -90:   Poor (significant loss)
RSSI below -90:    Likely no connection
```

Use `mesh_espnow_diagnostic_scan()` or the `on_data` callback's `rssi` parameter.

### Stage 3: Multi-hop test

Add a router node between the sensor and gateway. Verify:
- Sensor discovers both gateway and router
- Sensor's route to gateway goes through router
- Disconnect router → sensor re-routes directly or finds alternate path

### Stage 4: Stress test

| Test | How | Expected |
|------|-----|----------|
| Packet loss at range | Move node to marginal RSSI | Some retransmissions, eventual delivery |
| Node failure | Unplug a router | Routes re-form within 30-60s |
| Battery reporting | Change battery voltage in code | Routes shift away from low-battery nodes |
| Broadcast flood | Send 100 broadcasts in 10s | All nodes receive all (duplicates suppressed) |
| Deep sleep cycle (timer) | Set DEEP_SLEEP, 10s interval | Node wakes, sends, sleeps, repeats |
| Deep sleep on-demand | Set DEEP_SLEEP_ON_DEMAND, call sleep() | Node sleeps; remote send wakes it |

---

## Troubleshooting

### "Nodes don't discover each other"

- **Same channel?** Every node must use the same `cfg.channel`
- **Same PSK?** Every node must use the same `cfg.pre_shared_key`
- **Beacon mismatch?** A node with `beacon_interval_ms = 30000` takes 30s to appear
- **Distance?** Start at 1m and move apart
- **ESP-NOW not initialized?** Check `init()` and `start()` return ESP_OK
- **Check the state**: `mesh_espnow_get_state()` should return `CONNECTED` after discovery

### "Packets get through sometimes but not always"

- **RSSI too low**: Check the RSSI value. Move nodes closer or add a router.
- **Channel interference**: Try channel 1, 6, or 11. Use a Wi-Fi scanner to find quiet channels.
- **Too many neighbors**: If `max_neighbors` is full, new nodes are rejected. Increase it.
- **Retransmit too low**: Try `cfg.max_retransmits = 5` and `cfg.retransmit_timeout_ms = 300`.

### "Node joins but then disappears"

- **Power saving**: If using DUTY_CYCLE, the node is only listening during `awake_window_ms`
- **Deep sleep**: The node literally resets. It will appear again after the next wake cycle.
- **Neighbor timeout**: If `neighbor_timeout_ms` = 10s but beacon interval = 30s, the node will time out between beacons. Set `neighbor_timeout_ms ≥ 3 × beacon_interval_ms`.

### "RREQ rate-limited" errors

This is normal. The exponential backoff starts at 1s and doubles after each attempt (1s → 2s → 4s → 8s → 10s cap). If a route isn't found after 5 tries, the destination is likely offline.

### "Decryption failed" errors

**Root cause**: The receiving node's `pre_shared_key` doesn't match the sender's.

Check every node:

```c
// Read back what key is actually set
mesh_espnow_config_t active;
ESP_ERROR_CHECK(mesh_espnow_get_config(&active));
ESP_LOG_BUFFER_HEX("key", active.pre_shared_key, 16);
```

### "esp_now_send returned ESP_ERR_ESPNOW_NOT_INIT"

`mesh_espnow_start()` needs to be called after `mesh_espnow_init()`. Check the return values:

```c
ESP_ERROR_CHECK(mesh_espnow_init(&cfg));
ESP_ERROR_CHECK(mesh_espnow_start());
```

### Node reboots in a loop

1. **Power**: Bad USB cable or weak power supply. Use a good quality cable.
2. **Watchdog**: Your main loop might be blocking too long. Make sure `mesh_espnow_process()` is called at least every 100ms.
3. **Stack overflow**: Reduce stack usage in your application or increase `CONFIG_MAIN_TASK_STACK_SIZE`.

### Packet loss in specific locations

| Environment | Likely cause | Fix |
|-------------|-------------|-----|
| Near a microwave | 2.4GHz interference | Move away or add shielding |
| Through multiple walls | Signal attenuation | Add a router in the hallway |
| Near a metal beam | Signal reflection | Relocate node |
| Outdoors, rainy | Rain attenuation | Rare issue at 2.4GHz; add a closer hop |

---

## Field Diagnostics

Run `mesh_espnow_diagnostic_scan()` from any node to get a full state dump:

```
=== MESH DIAGNOSTIC SCAN ===
State: CONNECTED  Uptime: 3600s  Node: 0xA1000042  Ver: 0x02

--- Neighbors (3/32) ---
  0xA1000000  RSSI:-55  Metric:30   CAPS:GW   Batt:0     PDR:98%  Seen:1.2s
  0xA1000010  RSSI:-62  Metric:50   CAPS:ROUT Batt:3200  PDR:92%  Seen:3.1s
  0xA1000020  RSSI:-80  Metric:85   CAPS:LEAF Batt:2800  PDR:45%  Seen:8.5s

--- Routes (2/64) ---
  Dest:0xA1000000  Next:0xA1000000  Hops:1  Metric:30   PRIMARY
  Dest:0xA1000050  Next:0xA1000010  Hops:2  Metric:70   PRIMARY
  Dest:0xA1000050  Next:0xA1000020  Hops:3  Metric:115  BACKUP

--- Stats ---
  Sent:254  Recv:3892  Fwd:1024  Lost:12  DecryptFail:0
  RREQs:3  RouteRepairs:1  AvgRSSI:-65

--- Retransmit Queue (0/8) ---
  (empty)

--- Health ---
  Boots:47  LastError:"none"
```

Use this to understand the network state at a glance.

---

## Scaling Beyond 100 Nodes

### Memory planning

| Table | Bytes per entry | Memory for 256 entries |
|-------|----------------|----------------------|
| Neighbors | 32 | 8 KB |
| Routes | 24 | 6 KB |
| Retransmit | 300 | 2.4 KB (8 entries) |
| Duplicate cache | 20 | 3.2 KB (16 entries) |
| **Total** | | **~20 KB** (out of ~300 KB available) |

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
cfg.beacon_interval_ms = 15000;    // reduce airtime
cfg.neighbor_timeout_ms = 120000;  // 2 min
cfg.route_timeout_ms = 300000;     // 5 min
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

// Routers (same for all 5)
cfg.channel = 6;
cfg.capabilities = MESH_ESPNOW_CAP_ROUTER;
cfg.power_mode = MESH_ESPNOW_POWER_ALWAYS_ON;
cfg.max_neighbors = 64;

// Sensors (periodic reporting, same for all 44)
cfg.channel = 6;
cfg.capabilities = MESH_ESPNOW_CAP_LEAF | MESH_ESPNOW_CAP_SLEEPY;
cfg.power_mode = MESH_ESPNOW_POWER_DEEP_SLEEP;
cfg.deep_sleep_interval_ms = 300000;   // report every 5 minutes
cfg.beacon_interval_ms = 300000;
cfg.max_retransmits = 5;

// OR: Sensors (on-demand, wake only when gateway sends command)
// cfg.power_mode = MESH_ESPNOW_POWER_DEEP_SLEEP_ON_DEMAND;
```

### Expected behavior

- Sensors wake every 5 minutes, send reading, go back to sleep
- Routers forward traffic 24/7
- If a router fails (unplugged), sensors auto-route through another router
- Gateway logs all sensor data
- Battery life: ~5 years for sensors (2× AA alkaline)
