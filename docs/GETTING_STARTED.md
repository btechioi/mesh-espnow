# Getting Started with ESP-NOW Mesh

> **No Wi-Fi network. No router. No internet. Just ESP32s talking to each other.**

This guide gets a pair of ESP32 boards sending messages back and forth in under 10 minutes. If you've never used ESP-IDF before, start here.

---

## What You Need

### Hardware

| Item | Notes |
|------|-------|
| **2× ESP32 boards** | ESP32, ESP32-S3, ESP32-C3, ESP32-C6 — any ESP-NOW capable chip |
| **USB cables** | One per board, for power + flashing |
| **Breadboard + jumper wires** | Optional, for connecting sensors |

Any ESP32 works. No external Wi-Fi router needed. The boards talk direct radio-to-radio.

### Software

| Tool | Why you need it | Install |
|------|----------------|---------|
| **ESP-IDF v4.4+** | Build system + toolchain | [docs.espressif.com](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/index.html) |
| **Python 3.6+** | ESP-IDF needs it | Comes with ESP-IDF |
| **git** | Version control | `sudo apt install git` (Linux) |

### Test with a known-working setup

These boards are confirmed to work:
- ESP32-DevKitC (ESP-WROOM-32)
- ESP32-C3-DevKitM-1
- ESP32-S3-DevKitC-1
- ESP32-C6-DevKitM-1

---

## Quick Start (5 Minutes)

### 1. Get the code

```bash
git clone <your-repo-url> mesh-project
cd mesh-project
```

### 2. Set up ESP-IDF

If you haven't set up ESP-IDF yet:

```bash
# Clone ESP-IDF (do this once)
mkdir ~/esp
cd ~/esp
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32c3   # or your target chip

# Source the environment (run this in every new terminal)
. ~/esp/esp-idf/export.sh
```

### 3. Flash the gateway node

The gateway is the "root" of the mesh — it's always on, always listening.

```bash
cd examples/02_gateway_node

# Set your target chip
idf.py set-target esp32c3

# Build
idf.py build

# Connect your ESP32, then flash + monitor
idf.py -p /dev/ttyACM0 flash monitor
```

**Keep this terminal open.** The gateway is now running.

### 4. Flash the sensor node

Open a **second terminal**. Connect your second ESP32.

```bash
# Source ESP-IDF in this terminal too
. ~/esp/esp-idf/export.sh

cd examples/01_sensor_node

# Same chip as gateway
idf.py set-target esp32c3

# Build
idf.py build

# Flash the second board (different port!)
idf.py -p /dev/ttyACM1 flash monitor
```

### 5. Watch them talk

In the **sensor node** terminal, you'll see:

```
I (5000) sensor: Network joined! Gateway: 0xA1000000
I (8000) sensor: Sending to gateway: {"temp":24.5,"hum":55.2}
I (8300) sensor: Gateway ACK received
```

In the **gateway** terminal, you'll see:

```
I (5000) gateway: New node discovered: 0xA1000001
I (8000) gateway: From 0xA1000001: {"temp":24.5,"hum":55.2}
```

**Congratulations — you have a working mesh network!**

---

## How It Works (The 30-Second Version)

```
Sensor Node                    Gateway
    │                            │
    │── BEACON (every 3s) ──────▶│  "I'm here, ID 0xA1000001"
    │◀── BEACON (every 1s) ──────│  "I'm here, I'm the gateway"
    │                            │
    │── DATA (sensor reading) ──▶│  "temp: 24.5, hum: 55.2"
    │◀── ACK ────────────────────│  "Got it!"
```

- Both boards broadcast **beacons** so they discover each other
- The gateway sends beacons more often and advertises itself as root
- Data packets get an **ACK** (acknowledgment) for reliability
- If no ACK comes back, the sensor retries up to 3 times

---

## Adding a Third Node (Multi-Hop)

Distance too far for direct radio? Add a **router** in the middle.

### Router example

Create `examples/03_router/main/router.c`:

```c
#include "mesh_espnow.h"

void app_main(void) {
    mesh_espnow_config_t cfg = MESH_ESPNOW_CONFIG_DEFAULT();
    cfg.channel = 6;
    cfg.capabilities = MESH_ESPNOW_CAP_ROUTER;  // will forward traffic
    cfg.power_mode = MESH_ESPNOW_POWER_ALWAYS_ON;

    ESP_ERROR_CHECK(mesh_espnow_init(&cfg));
    ESP_ERROR_CHECK(mesh_espnow_start());

    while (1) {
        mesh_espnow_process(esp_timer_get_time() / 1000);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
```

No special forwarding code needed. The mesh library handles routing automatically. Flash this between the sensor and gateway, and traffic routes through it.

---

## Making Changes

### Change the channel

All boards must use the same channel. Set it in `app_main()`:

```c
cfg.channel = 6;    // valid: 1-11
```

Use 1, 6, or 11 — these don't overlap with each other.

### Change how often data is sent

In `sensor_node.c`, find:

```c
vTaskDelay(pdMS_TO_TICKS(30000));  // 30 seconds
```

Change to whatever you want (5000 = 5 seconds).

### Change the network password

The default PSK is `"MESH-ESPNOW-MESH"`. Change it on **every node**:

```c
// Must be exactly 16 bytes!
memcpy(cfg.pre_shared_key, "MY-SECRET-KEY!!", 16);
```

---

## Common Pitfalls

| Problem | Why | Fix |
|---------|-----|-----|
| "No neighbors found" | Wrong channel | All boards must use `cfg.channel = X` **with the same X** |
| "esp_err_t not found" | ESP-IDF not set up | Run `. ~/esp/esp-idf/export.sh` |
| "Can't open port /dev/ttyACM0" | Wrong port or permission | Run `ls /dev/tty*` to find your board; `sudo usermod -a -G dialout $USER` then log out/in |
| Boards are 10m+ apart | ESP-NOW range is ~50m indoors | Move them closer or add a router node |
| "Decryption failed" errors | Different PSK on different boards | Every board must use the same `pre_shared_key` |
| ESP32 reboots in a loop | Power supply too weak | Use a good USB cable; avoid cheap power banks |

---

## What's Next?

| Guide | What it covers |
|-------|---------------|
| [ARCHITECTURE.md](ARCHITECTURE.md) | How the mesh works internally |
| [API_REFERENCE.md](API_REFERENCE.md) | Every function and configuration option |
| [PROTOCOL.md](PROTOCOL.md) | On-wire packet format |
| [DEPLOYMENT.md](DEPLOYMENT.md) | Planning a real-world deployment |

---

## Terminology

| Term | Meaning |
|------|---------|
| **Node** | One ESP32 board running mesh firmware |
| **Gateway** | The root node (usually connected to the internet) |
| **Leaf** | A battery-powered node that doesn't forward traffic |
| **Router** | A mains-powered node that forwards others' traffic |
| **Beacon** | A short "I'm here" broadcast every few seconds |
| **ACK** | Acknowledgment — "I got your message" |
| **Hop** | One radio link. Two hops = sent through one intermediate node |
| **Metric** | A number measuring how "good" a route is (lower = better) |
| **Node ID** | A unique 32-bit number identifying each board |

---

**Still stuck?** Open an issue at <repo-url>/issues.
