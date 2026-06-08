<p align="center">
  <img src="https://capsule-render.vercel.app/api?type=waving&color=0:ff8c00,100:ff4500&height=180&section=header&text=Getting%20Started&fontSize=40&fontAlignY=35&animation=fadeIn&fontColor=ffffff"/>
</p>

# 🚀 Getting Started with ESP-NOW Mesh

> **No Wi-Fi network. No router. No internet. Just ESP32s talking to each other.**

---

# 📦 What You Need

## Hardware

| Item | Notes |
|------|-------|
| **2× ESP32 boards** | ESP32, ESP32-S3, ESP32-C3, ESP32-C6 — any ESP-NOW capable chip |
| **USB cables** | One per board, for power + flashing |

Any ESP32 works. No external Wi-Fi router needed. The boards talk direct radio-to-radio.

## Software — Pick One

### Option A: ESP-IDF

| Tool | Why | Install |
|------|-----|---------|
| **ESP-IDF v4.4+** | Build system + toolchain | [docs.espressif.com](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/index.html) |
| **Python 3.6+** | Comes with ESP-IDF | — |
| **git** | Version control | `sudo apt install git` |

### Option B: Arduino IDE

| Tool | Why | Install |
|------|-----|---------|
| **Arduino IDE 2.x** | Editor | [arduino.cc](https://www.arduino.cc/en/software) |
| **ESP32 Arduino Core** | Board support | See [espressif/arduino-esp32](https://espressif.github.io/arduino-esp32/package_esp32_index.json) |

### Confirmed boards

- ESP32-DevKitC (ESP-WROOM-32)
- ESP32-C3-DevKitM-1
- ESP32-S3-DevKitC-1
- ESP32-C6-DevKitM-1

---

# ⚡ Quick Start (5 Minutes)

## 1. Get the code

```bash
git clone https://github.com/btechioi/mesh-espnow.git mesh-project
cd mesh-project
```

## 2a. ESP-IDF — Set up

```bash
. ~/esp/esp-idf/export.sh
```

## 2b. Arduino — Install

1. Copy `mesh_espnow/` → `~/Arduino/libraries/`
2. **File → Examples → ESP-NOW Mesh Network Library** appears
3. Or in PlatformIO:
   ```ini
   lib_deps = https://github.com/btechioi/mesh-espnow
   ```

## 3. Flash the gateway

```
Gateway (always on, root of mesh)
```

### ESP-IDF
```bash
cd examples/02_gateway_node
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

### Arduino
Open **02_gateway_node** example → Select board → Upload.

## 4. Flash the sensor node

Connect your second ESP32.

### ESP-IDF
```bash
# Second terminal
. ~/esp/esp-idf/export.sh
cd examples/01_sensor_node
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/ttyACM1 flash monitor
```

### Arduino
Open **01_sensor_node** example → Select port → Upload.

## 5. Watch them talk

**Sensor terminal:**
```
I (5000) sensor: Network joined! Gateway: 0xA1000000
I (8000) sensor: Sending to gateway: {"temp":24.5,"hum":55.2}
I (8300) sensor: Gateway ACK received
```

**Gateway terminal:**
```
I (5000) gateway: New node discovered: 0xA1000001
I (8000) gateway: From 0xA1000001: {"temp":24.5,"hum":55.2}
```

**Congratulations — you have a working mesh network!**

---

# 🧠 How It Works

```
Sensor Node                    Gateway
    │                            │
    │── BEACON (every 3s) ──────▶│  "I'm here, ID 0xA1000001"
    │◀── BEACON (every 1s) ──────│  "I'm here, I'm the gateway"
    │                            │
    │── DATA (sensor reading) ──▶│  "temp: 24.5, hum: 55.2"
    │◀── ACK ────────────────────│  "Got it!"
```

- Both boards broadcast **beacons** to discover each other
- Gateway beacons more often, advertises itself as root
- Data packets get an **ACK** — retry up to 3 times if missing

---

# 🌲 Adding a Third Node (Multi-Hop)

Distance too far? Add a **router** in between.

```c
#include "mesh_espnow.h"

void app_main(void) {   // or setup() for Arduino
    mesh_espnow_config_t cfg = MESH_ESPNOW_CONFIG_DEFAULT();
    cfg.channel = 6;
    cfg.capabilities = MESH_ESPNOW_CAP_ROUTER;
    cfg.power_mode = MESH_ESPNOW_POWER_ALWAYS_ON;

    ESP_ERROR_CHECK(mesh_espnow_init(&cfg));
    ESP_ERROR_CHECK(mesh_espnow_start());

    while (1) {   // or loop()
        mesh_espnow_process(esp_timer_get_time() / 1000);
        vTaskDelay(pdMS_TO_TICKS(100));   // or delay(100)
    }
}
```

No special forwarding code — the mesh handles routing automatically.

---

# 🛠️ Making Changes

## Change the channel

```c
cfg.channel = 6;    // 1-11, all nodes must match
```

Use 1, 6, or 11 (non-overlapping).

## Change the network key

```c
memcpy(cfg.pre_shared_key, "MY-SECRET-KEY!!", 16);  // exactly 16 bytes
```

---

# 🔧 Troubleshooting

| Problem | Why | Fix |
|---------|-----|-----|
| No neighbors found | Wrong channel | All boards: `cfg.channel = X` with same X |
| `esp_err_t` not found | ESP-IDF not set up | Run `. ~/esp/esp-idf/export.sh` or use Arduino |
| `mesh_espnow.h` not found | Library not installed | Copy to `~/Arduino/libraries/` |
| Can't open port | Wrong port / permission | `ls /dev/tty*`; `sudo usermod -a -G dialout $USER` |
| Boards 10m+ apart | Range issue | Move closer or add a router |
| Decryption failed | PSK mismatch | Same `pre_shared_key` on every board |
| Reboot loop | Weak power | Use a good USB cable |

---

# 📚 What's Next?

| Guide | Covers |
|-------|--------|
| [API_REFERENCE.md](API_REFERENCE.md) | All functions, types, config |
| [ARCHITECTURE.md](ARCHITECTURE.md) | State machine, packet flow |
| [PROTOCOL.md](PROTOCOL.md) | On-wire format |
| [DEPLOYMENT.md](DEPLOYMENT.md) | Production planning + scaling |

---

# 📖 Terminology

| Term | Meaning |
|------|---------|
| **Node** | One ESP32 running mesh firmware |
| **Gateway** | Root node (internet-connected) |
| **Leaf** | Battery node, doesn't forward |
| **Router** | Mains node, forwards traffic |
| **Beacon** | Periodic "I'm here" broadcast |
| **ACK** | Acknowledgment |
| **Hop** | One radio link |
| **Metric** | Route quality score (lower = better) |
| **Node ID** | Unique 32-bit ID per board |

---

<p align="center">
  <img src="https://capsule-render.vercel.app/api?type=waving&color=0:ff8c00,100:ff4500&height=120&section=footer"/>
</p>
