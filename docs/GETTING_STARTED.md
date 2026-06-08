# Getting Started with ESP-NOW Mesh

> **No Wi-Fi network. No router. No internet. Just ESP32s talking to each other.**

This guide gets a pair of ESP32 boards sending messages back and forth in under 10 minutes.

---

## What You Need

### Hardware

| Item | Notes |
|------|-------|
| **2× ESP32 boards** | ESP32, ESP32-S3, ESP32-C3, ESP32-C6 — any ESP-NOW capable chip |
| **USB cables** | One per board, for power + flashing |

Any ESP32 works. No external Wi-Fi router needed. The boards talk direct radio-to-radio.

### Software — Pick One

#### Option A: ESP-IDF (traditional)

| Tool | Why you need it | Install |
|------|----------------|---------|
| **ESP-IDF v4.4+** | Build system + toolchain | [docs.espressif.com](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/index.html) |
| **Python 3.6+** | ESP-IDF needs it | Comes with ESP-IDF |
| **git** | Version control | `sudo apt install git` (Linux) |

#### Option B: Arduino IDE (simpler)

| Tool | Why you need it | Install |
|------|----------------|---------|
| **Arduino IDE 2.x** | The editor | [arduino.cc](https://www.arduino.cc/en/software) |
| **ESP32 Arduino Core** | Board support | Follow [espressif/arduino-esp32 install guide](https://espressif.github.io/arduino-esp32/package_esp32_index.json) |

### Confirmed working boards

- ESP32-DevKitC (ESP-WROOM-32)
- ESP32-C3-DevKitM-1
- ESP32-S3-DevKitC-1
- ESP32-C6-DevKitM-1

---

## Quick Start (5 Minutes)

### 1. Get the code

```bash
git clone https://github.com/btechioi/mesh-espnow.git mesh-project
cd mesh-project
```

### 2a. ESP-IDF — Set up environment

```bash
# Source ESP-IDF (run in every new terminal)
. ~/esp/esp-idf/export.sh
```

### 2b. Arduino IDE — Install the library

1. **Arduino IDE 2**: Copy `mesh_espnow/` folder into `~/Arduino/libraries/`
2. **PlatformIO**: Add this to `platformio.ini`:
   ```ini
   lib_deps = https://github.com/btechioi/mesh-espnow
   ```
3. Restart the IDE. Check that **File → Examples → ESP-NOW Mesh Network Library** appears.

### 3. Flash the gateway node

The gateway is the "root" of the mesh — it's always on, always listening.

#### ESP-IDF
```bash
cd examples/02_gateway_node
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

#### Arduino IDE
Open **File → Examples → ESP-NOW Mesh Network Library → 02_gateway_node**.
Select your board and port, then click **Upload**.

**Keep this terminal/serial monitor open.** The gateway is now running.

### 4. Flash the sensor node

Connect your second ESP32 board.

#### ESP-IDF
```bash
# Open a second terminal, source ESP-IDF
. ~/esp/esp-idf/export.sh
cd examples/01_sensor_node
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/ttyACM1 flash monitor
```

#### Arduino IDE
Open **File → Examples → ESP-NOW Mesh Network Library → 01_sensor_node**.
Select your second board's port, then click **Upload**.

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

Create `examples/03_router/main/router.c` (or `03_router/03_router.ino` for Arduino):

```c
#include "mesh_espnow.h"

void app_main(void) {   // or setup() for Arduino
    mesh_espnow_config_t cfg = MESH_ESPNOW_CONFIG_DEFAULT();
    cfg.channel = 6;
    cfg.capabilities = MESH_ESPNOW_CAP_ROUTER;  // will forward traffic
    cfg.power_mode = MESH_ESPNOW_POWER_ALWAYS_ON;

    ESP_ERROR_CHECK(mesh_espnow_init(&cfg));
    ESP_ERROR_CHECK(mesh_espnow_start());

    while (1) {   // or loop() for Arduino
        mesh_espnow_process(esp_timer_get_time() / 1000);
        vTaskDelay(pdMS_TO_TICKS(100));   // or delay(100)
    }
}
```

No special forwarding code needed. The mesh library handles routing automatically.

---

## Making Changes

### Change the channel

All boards must use the same channel:

```c
cfg.channel = 6;    // valid: 1-11
```

Use 1, 6, or 11 — these don't overlap with each other.

### Change how often data is sent

In the sensor example, adjust the loop counter threshold to change send frequency.

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
| "esp_err_t not found" | ESP-IDF not set up | Run `. ~/esp/esp-idf/export.sh` or switch to Arduino IDE |
| "mesh_espnow.h not found" | Library not installed | Copy `mesh_espnow/` to Arduino `libraries/` folder |
| Can't open port | Wrong port or permission | `ls /dev/tty*` to find your board; `sudo usermod -a -G dialout $USER` then log out/in |
| Boards are 10m+ apart | ESP-NOW range is ~50m indoors | Move them closer or add a router node |
| "Decryption failed" | Different PSK on different boards | Every board must use the same `pre_shared_key` |
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

**Still stuck?** Open an issue at https://github.com/btechioi/mesh-espnow/issues.
