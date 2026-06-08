# Architecture Guide

> **How the mesh library works under the hood — state machine, packet flow, module interactions.**

---

## System Overview

```
┌──────────────────────────────────────────────────────────────┐
│                    Application Layer                         │
│  (your app_main.c / Arduino .ino — sensors, actuators, etc) │
├──────────────────────────────────────────────────────────────┤
│                     mesh_espnow.h (API)                      │
├───────────────────────┬──────────────────┬──────────────────┤
│                       │                  │                  │
│  ┌─────────────────┐  │  ┌────────────┐  │  ┌────────────┐  │
│  │  mesh_core.c    │  │  │ mesh_power │  │  │ mesh_diag  │  │
│  │  - state machine│  │  │ - duty cycle│  │  │ - health   │  │
│  │  - beacon loop  │  │  │ - deep sleep│  │  │ - crash det│  │
│  │  - ESP-NOW I/O  │  │  └────────────┘  │  └────────────┘  │
│  └────────┬────────┘  │                  │                  │
│           │            │                  │                  │
│  ┌────────┴────────┐  │  ┌────────────┐  │                  │
│  │ mesh_routing.c  │◀─┼──│mesh_reliable│  │                  │
│  │ - route table   │  │  │ - ACK      │  │                  │
│  │ - neighbor table│  │  │ - retransmit│  │                  │
│  │ - metric        │  │  └────────────┘  │                  │
│  │ - RREQ/RREP     │  │                  │                  │
│  └────────┬────────┘  │                  │                  │
│           │            │                  │                  │
│  ┌────────┴────────┐  │                  │                  │
│  │ mesh_security   │  │                  │                  │
│  │ - AES-128-CCM   │  │                  │                  │
│  │ - encrypt/decrypt│  │                  │                  │
│  └─────────────────┘  │                  │                  │
├────────────────────────┴──────────────────────────────────┤
│                   ESP-NOW (esp_now_*)                      │
├────────────────────────────────────────────────────────────┤
│                   Wi-Fi (station mode)                     │
├────────────────────────────────────────────────────────────┤
│                   FreeRTOS + ESP-IDF                       │
└────────────────────────────────────────────────────────────┘
```

### Arduino compatibility

The library is fully compatible with Arduino ESP32 core (which bundles ESP-IDF
under the hood). All APIs are identical. Just replace:
- `app_main()` → `setup()` / `loop()`
- `vTaskDelay(ms)` → `delay(ms)`
- `esp_timer_get_time() / 1000` → `millis()`

---

## Module Responsibilities

### mesh_core.c — The Brain

- **State machine** — governs lifecycle (see below)
- **ESP-NOW callbacks** — handles send/receive events from the radio
- **Beacon sender** — periodically broadcasts our presence
- **Packet builder/sender** — constructs on-wire packets from API calls
- **Process loop** — `mesh_espnow_process()` handles all deferred work

### mesh_routing.c — The Navigator

- **Neighbor table** — every node within direct radio range
- **Route table** — every known destination (possibly multi-hop)
- **Route metric** — composite score (hops, RSSI, battery, capability, reliability)
- **Multi-path** — primary + backup route per destination
- **RREQ/RREP** — on-demand route discovery
- **Optimization pass** — periodic re-evaluation of all routes

### mesh_reliable.c — The Courier

- **ACK tracking** — maps pending ACKs to sent packets
- **Retransmission** — resends with exponential backoff on timeout
- **TX reporting** — tells routing layer whether each send succeeded or failed

### mesh_security.c — The Guardian

- **AES-128-CCM** — authenticated encryption
- **8-byte MIC** — message integrity code appended to every ciphertext
- **Nonce** — per-packet unique value derived from node ID + random

### mesh_power.c — The Battery Saver

- **Duty cycling** — modem-sleep between active windows
- **Deep sleep orchestration** — saves state, calls `esp_deep_sleep_start()`
- **Battery reporting** — forwards mV readings to routing layer

### mesh_diag.c — The Doctor

- **Boot counting** — NVS persistent counter
- **Crash detection** — RTC memory flag checked at boot
- **Periodic health logging** — prints stats every 30s
- **Diagnostic scan** — full dump of all tables

---

## State Machine

```
      ┌─────────────────────────────────────────────────────┐
      │                                                     │
      │  MESH_ESPNOW_STATE_UNINITIALIZED                    │
      │     │                                               │
      │     │ mesh_espnow_init()                            │
      │     ▼                                               │
      │  MESH_ESPNOW_STATE_INIT                             │
      │     │                                               │
      │     │ mesh_espnow_start()                           │
      │     ▼                                               │
      │  MESH_ESPNOW_STATE_DISCOVERING ◀──────────────────┐ │
      │     │                    │                         │ │
      │     │ gateway found     │ gateway lost             │ │
      │     ▼                    │                         │ │
      │  MESH_ESPNOW_STATE_CONNECTED ─────────────────────┘ │
      │     │                    │                           │
      │     │ mesh_espnow_stop() │ fatal error              │
      │     ▼                    ▼                           │
      │  MESH_ESPNOW_STATE_INIT  MESH_ESPNOW_STATE_ERROR   │ │
      │     │                                               │ │
      │     │ mesh_espnow_deinit()                          │ │
      │     ▼                                               │ │
      │  MESH_ESPNOW_STATE_UNINITIALIZED ◀──────────────────┘ │
      │                                                     │
      │  MESH_ESPNOW_STATE_SLEEPING                         │
      │     (entered via mesh_espnow_sleep(),                │
      │      exits via full reset on wake)                  │
      └─────────────────────────────────────────────────────┘
```

### Transition Details

| Transition | Trigger | What happens |
|-----------|---------|-------------|
| UNINIT → INIT | `init()` called | Wi-Fi + ESP-NOW + NVS initialized; config validated; subsystems inited |
| INIT → DISCOVERING | `start()` called | Beacon timer starts; we begin listening + announcing |
| DISCOVERING → CONNECTED | Gateway route found | Via beacon from gateway, or RREP from a node that knows a path |
| CONNECTED → DISCOVERING | All gateway routes lost | Neighbor/route timeout, or GOODBYE from last gateway hop |
| any → INIT | `stop()` called | Goodbye beacon sent; all timers stopped; radio deinitialized |
| any → ERROR | Fatal internal failure | All subsystems stopped; `on_fatal_error()` invoked |
| any → UNINIT | `deinit()` called | Full teardown; all memory freed; NVS closed |

---

## Packet Flow

### Send Path (application → radio)

```
app calls mesh_espnow_send(dest, data, len)
    │
    ▼
mesh_core_send_packet()  [core]
    │
    ├── mesh_routing_lookup(dest)   →   next_hop, primary_route
    │       │
    │       ├── Route found?        →   use it
    │       └── No route?           →   mesh_routing_send_rreq(dest)
    │                                      │
    │                                      └── broadcast RREQ, start backoff
    │
    ├── mesh_security_encrypt(data) →   ciphertext + MIC tag
    │
    ├── mesh_reliable_start_tx(next_hop, packet)
    │       │
    │       ├── Save in retransmit queue
    │       ├── esp_now_send(next_hop_mac, encrypted_packet, len)
    │       └── Start ACK timer (retransmit_timeout_ms)
    │
    └── return (async; result via callback or retry)
```

### Receive Path (radio → application)

```
esp_now_recv_cb()  [ESP-NOW ISR]
    │  (minimal work — just flags the event)
    │
    ▼
mesh_espnow_process()  [called from app main loop]
    │
    ├── Handle received packet:
    │       │
    │       ├── mesh_security_decrypt()   →   plaintext or fail
    │       │
    │       ├── Check packet type:
    │       │   ├── TYPE_DATA      →   for us? deliver via on_data
    │       │   │                       not for us? forward to next hop
    │       │   ├── TYPE_BROADCAST →   on_broadcast + re-broadcast (TTL-1)
    │       │   ├── TYPE_ACK      →   mesh_reliable_handle_ack()
    │       │   ├── TYPE_RREQ     →   mesh_routing_handle_rreq()
    │       │   ├── TYPE_RREP     →   mesh_routing_handle_rrep()
    │       │   ├── TYPE_BEACON   →   mesh_routing_handle_beacon()
    │       │   └── TYPE_GOODBYE  →   remove neighbor, invalidate routes
    │       │
    │       └── Update stats (packets received, per-type counters)
    │
    ├── Handle ACK timeout & retransmission
    │
    └── Handle deferred work:
            ├── Route optimization pass
            ├── Neighbor / route expiry
            └── Diagnostic logging
```

---

## Memory Model

### Static allocation (no malloc after init)

All major data structures are **fixed-size arrays** allocated at init time:

| Table | Entry size | Default count | Total memory |
|-------|-----------|---------------|-------------|
| Neighbor table | ~36 bytes | 32 | ~1.2 KB |
| Route table | ~20 bytes | 64 | ~1.3 KB |
| Retransmit queue | ~300 bytes | 16 | ~4.8 KB |
| Duplicate cache | ~24 bytes | 128 | ~3.1 KB |
| **Total** | | | **~10 KB** |

Configurable at init via `cfg.max_neighbors` and `cfg.max_routes`.

### Eviction policy

When tables fill up, the worst entry is evicted:
- **Neighbor table**: highest-metric neighbor is replaced
- **Route table**: highest-metric route is replaced
- **Retransmit queue**: oldest pending transmission is dropped

---

## Thread Safety

### Global mutex

A single `SemaphoreHandle_t` protects all shared state:

- **Held by**: all public API functions (`mesh_espnow_send()`, etc.)
- **NOT held in**: ESP-NOW callbacks (ISR context) — they use a minimal lock-free path that defers work to `mesh_espnow_process()`
- **Never held across blocking calls**

### Lock hierarchy

```
Application task → mesh_espnow_*() → mutex → access shared state
ESP-NOW callback (ISR) → flag event → return
Process task → mutex → process deferred events
```

---

## Timer Architecture

All timers are **cooperative** — polled in `mesh_espnow_process()`. No dynamic timers.

| Timer | Period | Managed by | Purpose |
|-------|--------|-----------|---------|
| Beacon | `beacon_interval_ms` | `mesh_core.c` | Periodic "I'm here" broadcast |
| Route optimization | 15 seconds | `mesh_routing.c` | Re-evaluate all routes |
| Neighbor expiry | every process call | `mesh_routing.c` | Remove stale neighbors |
| Route expiry | every process call | `mesh_routing.c` | Remove stale routes |
| ACK timeout | `retransmit_timeout_ms` per packet | `mesh_reliable.c` | Retransmit if no ACK |
| Health log | 30 seconds | `mesh_diag.c` | Periodic stats dump |

---

## Key Design Decisions

### Why not ESP-MESH?

| Feature | This library | ESP-MESH |
|---------|-------------|----------|
| Wi-Fi channel usage | ESP-NOW (no AP) | AP + ESP-NOW |
| Max nodes | 1000+ | ~300 |
| Battery support | Deep sleep, duty cycle | Poor |
| Dependency | ESP-NOW only | Full Wi-Fi stack |
| Complexity | Simple API | Complex |

### Why fixed-size arrays?

- **Predictable memory** — no OOM at runtime
- **No fragmentation** — heap stays clean for application use
- **Fast access** — O(1) indexed lookup
- **Configurable** — `max_neighbors` and `max_routes` are user-configured

### Why process() polling instead of tasks?

- **Single-core friendly** — no task switching overhead
- **Application controls timing** — you decide how often to call it
- **Simpler debugging** — no inter-task synchronization issues
- **Works with deep sleep** — no task to resume after wake

---

## Forwarding (multi-hop)

When a node receives a DATA packet **not** addressed to it:

1. Decrement TTL
2. If TTL > 0, look up destination in route table
3. If route exists, re-encrypt and forward to next hop
4. If no route, silently drop

Forwarding is automatic. Don't set `CAP_LEAF` if you want to forward.

---

## Comparison to Alternatives

| Feature | This library | ESP-MESH | Thread/Matter | Custom LoRa |
|---------|-------------|----------|---------------|-------------|
| Latency | ~10ms per hop | ~50ms per hop | ~100ms | ~100ms+ |
| Throughput | ~1 Mbps | ~10 Mbps | ~250 Kbps | ~50 Kbps |
| Range (indoor) | ~50m per hop | ~50m per hop | ~30m | ~1000m |
| Battery life | Years (sleep) | Days | Months | Years |
| Complexity | Simple | Complex | Very complex | Custom |
| Cost | $3-5 per node | $3-5 per node | $10-20 | $10-20 |
