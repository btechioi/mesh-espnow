<p align="center">
  <img src="https://capsule-render.vercel.app/api?type=waving&color=0:ff8c00,100:ff4500&height=180&section=header&text=Architecture&fontSize=40&fontAlignY=35&animation=fadeIn&fontColor=ffffff"/>
</p>

# 🏗️ Architecture Guide

> **How the mesh library works under the hood — state machine, packet flow, module interactions.**

---

# 🧬 System Overview

```mermaid
graph TB
    subgraph App["Application Layer"]
        APP["app_main.c / .ino<br/>sensors, actuators, data processing"]
    end
    API["mesh_espnow.h (API)"]

    subgraph Core["mesh_core.c"]
        SM["State machine"]
        BL["Beacon loop"]
        EN["ESP-NOW I/O"]
        PB["Packet builder"]
    end

    subgraph Routing["mesh_routing.c"]
        RT["Route table"]
        NT["Neighbor table"]
        MET["Metric"]
        RR["RREQ/RREP"]
    end

    subgraph Rel["mesh_reliable.c"]
        ACK["ACK"]
        RTX["Retransmit"]
    end

    subgraph Sec["mesh_security.c"]
        AES["AES-128-CCM"]
        CR["Encrypt/Decrypt"]
    end

    subgraph Pwr["mesh_power.c"]
        DC["Duty cycle"]
        DS["Deep sleep"]
    end

    subgraph Diag["mesh_diag.c"]
        HC["Health"]
        CD["Crash detect"]
    end

    ESPNOW["ESP-NOW (esp_now_*)"]
    WIFI["Wi-Fi (station mode)"]
    OS["FreeRTOS + ESP-IDF"]

    App --> API
    API --> Core
    Core --> Routing
    Routing <--> Rel
    Routing --> Sec
    Sec --> Core
    Core --> Pwr
    Core --> Diag
    Core --> ESPNOW
    ESPNOW --> WIFI
    WIFI --> OS
```

## Arduino Compatibility

All APIs are identical. Just replace:
- `app_main()` → `setup()` / `loop()`
- `vTaskDelay(ms)` → `delay(ms)`
- `esp_timer_get_time() / 1000` → `millis()`

---

# 🧠 Module Responsibilities

## `mesh_core.c` — The Brain

- **State machine** — governs lifecycle
- **ESP-NOW callbacks** — radio send/receive
- **Beacon sender** — periodic "I'm here"
- **Packet builder** — constructs on-wire packets
- **Process loop** — `mesh_espnow_process()` handles all deferred work

## `mesh_routing.c` — The Navigator

- **Neighbor table** — direct radio range nodes
- **Route table** — known destinations (multi-hop)
- **Route metric** — composite: hops, RSSI, battery, capability, reliability
- **Multi-path** — primary + backup per destination
- **RREQ/RREP** — on-demand route discovery
- **Optimization pass** — periodic re-evaluation (every 15s)

## `mesh_reliable.c` — The Courier

- **ACK tracking** — maps ACKs to sent packets
- **Retransmission** — exponential backoff on timeout
- **TX reporting** — success/failure per neighbor for PDR

## `mesh_security.c` — The Guardian

- **AES-128-CCM** — authenticated encryption
- **8-byte MIC** — message integrity code
- **Nonce** — unique per packet (node ID + seq + random)

## `mesh_power.c` — The Battery Saver

- **Duty cycling** — modem-sleep between active windows
- **Deep sleep** — saves state, calls `esp_deep_sleep_start()`
- **Battery reporting** — mV readings to routing layer

## `mesh_diag.c` — The Doctor

- **Boot counting** — NVS persistent counter
- **Crash detection** — RTC memory flag
- **Periodic health logging** — stats every 30s
- **Diagnostic scan** — full table dump

---

# 🔄 State Machine

```mermaid
stateDiagram-v2
    [*] --> UNINITIALIZED
    UNINITIALIZED --> INIT : init()
    INIT --> DISCOVERING : start()
    DISCOVERING --> CONNECTED : gateway found
    CONNECTED --> DISCOVERING : gateway lost
    CONNECTED --> INIT : stop()
    INIT --> ERROR : fatal error
    INIT --> UNINITIALIZED : deinit()
    ERROR --> UNINITIALIZED : deinit()

    state SLEEPING {
        [*] --> Wake : timer
        Wake --> [*] : deep sleep
    }
    INIT --> SLEEPING : sleep()
    SLEEPING --> UNINITIALIZED : full reset
```

## Transitions

| Transition | Trigger | What happens |
|-----------|---------|-------------|
| UNINIT → INIT | `init()` | Wi-Fi, ESP-NOW, NVS init |
| INIT → DISCOVERING | `start()` | Beacon timer starts |
| DISCOVERING → CONNECTED | Gateway route found | Via beacon or RREP |
| CONNECTED → DISCOVERING | No gateway routes | Timeout or GOODBYE |
| any → INIT | `stop()` | Goodbye + radio deinit |
| any → ERROR | Fatal error | `on_fatal_error()` fires |
| any → UNINIT | `deinit()` | All memory freed |

---

# 📦 Packet Flow

## Send Path

```mermaid
flowchart TD
    A["app calls mesh_espnow_send(dest, data, len)"] --> B["mesh_core_send_packet()"]
    B --> C{"mesh_routing_lookup(dest)"}
    C -->|Route found| D["use it"]
    C -->|No route| E["broadcast RREQ"]
    D --> F["mesh_security_encrypt()<br/>→ ciphertext + MIC"]
    E --> F
    F --> G["mesh_reliable_start_tx()"]
    G --> H["Save in retransmit queue"]
    H --> I["esp_now_send(next_hop, packet)"]
    I --> J["Start ACK timer"]
    J --> K["return (async)"]
```

## Receive Path

```mermaid
flowchart TD
    ISR["esp_now_recv_cb() [ISR — minimal work]"] --> MAIN["mesh_espnow_process() [main loop]"]
    MAIN --> DEC["mesh_security_decrypt()"]
    DEC --> TYPE{"Check type"}
    TYPE -->|DATA| DEL["deliver or forward"]
    TYPE -->|BROADCAST| BRD["deliver + re-broadcast"]
    TYPE -->|ACK| ACKM["match to pending"]
    TYPE -->|RREQ/RREP| RRT["routing table updates"]
    TYPE -->|BEACON| NBR["neighbor table updates"]
    TYPE -->|GOODBYE| GB["remove neighbor, fix routes"]
    DEL --> TIM["ACK timeout + retransmission"]
    BRD --> TIM
    ACKM --> TIM
    RRT --> TIM
    NBR --> TIM
    GB --> TIM
    TIM --> OPT["Route optimization, expiry, health logging"]
```

---

# 💾 Memory Model

## Static Allocation (no malloc after init)

| Table | Entry size | Default count | Memory |
|-------|-----------|---------------|--------|
| Neighbor table | ~36 bytes | 32 | ~1.2 KB |
| Route table | ~20 bytes | 64 | ~1.3 KB |
| Retransmit queue | ~300 bytes | 16 | ~4.8 KB |
| Duplicate cache | ~24 bytes | 128 | ~3.1 KB |
| **Total** | | | **~10 KB** |

## Eviction Policy

- **Neighbor table**: highest-metric neighbor replaced
- **Route table**: highest-metric route replaced
- **Retransmit queue**: oldest pending dropped

---

# 🧵 Thread Safety

- Single `SemaphoreHandle_t` protects all shared state
- Public API holds mutex; ISR callback defers work
- Mutex **never** held across blocking calls

---

# ⏱️ Timer Architecture (Cooperative)

| Timer | Period | Managed by | Purpose |
|-------|--------|-----------|---------|
| Beacon | `beacon_interval_ms` | `mesh_core.c` | "I'm here" broadcast |
| Route opt | 15s | `mesh_routing.c` | Re-evaluate all routes |
| Neighbor expiry | per process | `mesh_routing.c` | Remove stale neighbors |
| Route expiry | per process | `mesh_routing.c` | Remove stale routes |
| ACK timeout | per packet | `mesh_reliable.c` | Retransmit if no ACK |
| Health log | 30s | `mesh_diag.c` | Periodic stats dump |

---

# 🎯 Key Design Decisions

## Why not ESP-MESH?

| Feature | This library | ESP-MESH |
|---------|-------------|----------|
| Wi-Fi usage | ESP-NOW only | AP + ESP-NOW |
| Max nodes | 1000+ | ~300 |
| Battery support | Deep sleep, duty cycle | Poor |
| Complexity | Simple API | Complex |

## Why fixed-size arrays?

- Predictable memory — no OOM
- No fragmentation
- O(1) indexed lookup
- Configurable via `cfg.max_neighbors` / `cfg.max_routes`

## Why polling instead of tasks?

- Single-core friendly
- App controls timing
- Simpler debugging
- Works with deep sleep (no task to resume)

---

# 🔁 Forwarding (Multi-Hop)

When a node receives DATA not for it:

1. Decrement TTL
2. Look up destination in route table
3. If route exists → re-encrypt + forward to next hop
4. If no route → silently drop

Automatic. Don't set `CAP_LEAF` if you want to forward.

---

# ⚖️ Comparison

| Feature | This library | ESP-MESH | Thread/Matter | LoRa |
|---------|-------------|----------|---------------|------|
| Latency | ~10ms/hop | ~50ms/hop | ~100ms | ~100ms+ |
| Throughput | ~1 Mbps | ~10 Mbps | ~250 Kbps | ~50 Kbps |
| Range indoor | ~50m/hop | ~50m/hop | ~30m | ~1000m |
| Battery life | Years | Days | Months | Years |
| Cost per node | $3-5 | $3-5 | $10-20 | $10-20 |

---

<p align="center">
  <img src="https://capsule-render.vercel.app/api?type=waving&color=0:ff8c00,100:ff4500&height=120&section=footer"/>
</p>
