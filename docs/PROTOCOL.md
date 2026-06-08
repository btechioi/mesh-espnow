<p align="center">
  <img src="https://capsule-render.vercel.app/api?type=waving&color=0:ff8c00,100:ff4500&height=180&section=header&text=Protocol%20Spec&fontSize=40&fontAlignY=35&animation=fadeIn&fontColor=ffffff"/>
</p>

# 📡 ESP-NOW Mesh Protocol Specification

> **The on-wire format — every byte of every packet, explained.**

---

# 📦 Overview

All packets share a common header. Maximum ESP-NOW payload is **250 bytes**; the mesh uses **240 bytes max**.

```
┌────────────────────────────────────┐
│ Common Header (24 bytes)           │
├────────────────────────────────────┤
│ Payload (variable)                  │
├────────────────────────────────────┤
│ [MIC Tag (8 bytes)] — if encrypted │
└────────────────────────────────────┘

Total: 24 + payload + (8 if enc) ≤ 240 bytes
Max payload: 208 bytes (always)
```

---

# 🧱 Common Header (24 bytes)

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
├───────┼───────┼───────┼───────┼───────┼───────┼───────┼───────┤
│ proto_ver │ type  │  ttl  │            src_id               │
├───────┼───────┼───────┼───────┼───────┼───────┼───────┼───────┤
│                       dest_id                               │
├───────┼───────┼───────┼───────┼───────┼───────┼───────┼───────┤
│                       seqno                                 │
├───────┼───────┼───────┼───────┼───────┼───────┼───────┼───────┤
│                      ack_seqno                              │
├───────┼───────┼───────┼───────┼───────┼───────┼───────┼───────┤
│  payload_len  │ flags │     subnet_id       │
└───────┴───────┴───────┴───────┴───────┴───────┴───────┴───────┘
```

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 1 | `proto_ver` | Protocol version (`0x03`). Mismatch → silently dropped. |
| 1 | 1 | `type` | Packet type (see below) |
| 2 | 1 | `ttl` | Time-To-Live. Decremented each hop. |
| 3 | 4 | `src_id` | Source node ID (32-bit) |
| 7 | 4 | `dest_id` | Destination node ID |
| 11 | 4 | `seqno` | Monotonically increasing sequence number |
| 15 | 4 | `ack_seqno` | Sequence being acknowledged (ACK packets) |
| 19 | 2 | `payload_len` | Payload length in bytes |
| 21 | 1 | `flags` | Bitmask (see below) |
| 22 | 2 | `subnet_id` | Source's subnet (0 = global) |

## C Struct

```c
typedef struct __attribute__((packed)) {
    uint8_t  proto_ver;
    uint8_t  type;
    uint8_t  ttl;
    uint32_t src_id;
    uint32_t dest_id;
    uint32_t seqno;
    uint32_t ack_seqno;
    uint16_t payload_len;
    uint8_t  flags;
    uint16_t subnet_id;
} mesh_espnow_header_t;
```

## Flags

| Bit | Constant | Meaning |
|-----|----------|---------|
| 0 | `MESH_FLAG_RREQ` | Route request (in DATA) |
| 1 | `MESH_FLAG_RREP` | Route reply (in DATA) |
| 2 | `MESH_FLAG_ACK` | This is an ACK |
| 3 | `MESH_FLAG_CROSS_SUBNET` | Crossed subnets (loop prevention) |
| 7 | `MESH_FLAG_ENC` | Payload encrypted |

---

# 📋 Packet Types

| Value | Name | Direction | Description |
|-------|------|-----------|-------------|
| `0x01` | `PKT_BEACON` | broadcast | Periodic "I'm here" |
| `0x02` | `PKT_DATA` | unicast | Application data with ACK |
| `0x03` | `PKT_DATA_ACK` | unicast | Acknowledgment |
| `0x04` | `PKT_RREQ` | broadcast | Route request |
| `0x05` | `PKT_RREP` | unicast | Route reply |
| `0x06` | `PKT_BROADCAST` | flood | Network-wide flood |
| `0x07` | `PKT_GOODBYE` | broadcast | Graceful departure |

---

# 📄 Payload Formats

## Beacon (type `0x01`)

Broadcast every `beacon_interval_ms`. Nodes discover each other via beacons.

```
Header (24B) + capabilities(1) + gw_hops(1) + gw_id(4)
            + uptime_s(4) + battery_mv(4) = 14B payload
```

### Capabilities byte

| Bit | Flag | Meaning |
|-----|------|---------|
| 0 | `CAP_GATEWAY` | Network root |
| 1 | `CAP_ROUTER` | Forwards traffic |
| 2 | `CAP_LEAF` | Battery, no forwarding |
| 3 | `CAP_SLEEPY` | May sleep anytime |
| 4 | `CAP_STORE_FWD` | Store-and-forward |
| 5 | `CAP_BRIDGE` | Cross-subnet forwarding |

## DATA (type `0x02`)

```
Header (24B) + Application payload (1-208B) + [MIC 8B]
```

- Sent unicast to next hop (ESP-NOW peer)
- Re-encrypted at each hop
- Requires ACK from next hop

## ACK (type `0x03`)

```
Header (24B) — ack_seqno set, payload empty (0B)
```

- Unicast back to sender
- **Not encrypted**
- No ACK within `retransmit_timeout_ms` → retransmit

## RREQ (type `0x04`)

```
Header (24B) + target_id(4) + metric(2) = 6B payload
```

- Broadcast to all neighbors
- Receivers learn reverse route to `src_id`
- If receiver is `target_id` or has route → replies with RREP
- Otherwise re-broadcasts (TTL-1, dup check)
- **Exponential backoff**: 1s → 2s → 4s → 8s → 10s cap

## RREP (type `0x05`)

```
Header (24B) + target_id(4) + hop_count(1) + metric(2) = 7B payload
```

- Unicast back along reverse route
- Each hop installs forward route to `target_id`
- First RREP = primary; later RREPs may become backup

## Broadcast (type `0x06`)

```
Header (24B) + Application payload (1-208B)
```

- Flooded: every node re-broadcasts once (TTL-1)
- Duplicate suppression via `(src_id, seqno)` cache (128 slots)

## GOODBYE (type `0x07`)

```
Header (24B) — 0B payload
```

- Broadcast before stop/sleep
- Recipients immediately remove node from neighbor table

---

# 🔐 Encryption (AES-128-CCM)

## Scope

```
Encrypted:                     [Header(24) plain] [Payload ciphertext] [MIC(8)]
Encryption disabled:           [Header(24) plain] [Payload plaintext]
```

Only payload is encrypted. Headers stay in plaintext for forwarding.

## MIC

Authenticates `header + ciphertext`. Mismatch → packet silently dropped.

## Nonce

```
nonce = node_id(4) || seq_num(4) || random(4) = 12 bytes
```

## Key

- 16-byte pre-shared key — same on every node
- Default: `"MESH-ESPNOW-MESH"` — **change for production**

---

# 🔄 Route Discovery Flow

```
Node A wants to send to Node D (no route)
    │
    ├── Broadcast RREQ: target=D, metric=0
    │
    │   Neighbor B receives:
    │   ├── Learns reverse route to A
    │   ├── Has route to D? YES → unicast RREP back
    │   └── Does NOT re-broadcast
    │
    │   Neighbor C receives:
    │   ├── Learns reverse route to A
    │   ├── Has route to D? NO → re-broadcasts RREQ
    │   └── ... eventually reaches D or someone with route
    │
    ├── RREP arrives at A from B
    │   ├── Installs primary route: next_hop=B, hops=3
    │   └── Route ready
    │
    └── A sends DATA to D via B
```

---

# 🚫 Packet Drop Reasons

| Reason | Log tag | Action |
|--------|---------|--------|
| Version mismatch | mesh | Silently drop |
| TTL exceeded | mesh | Silently drop |
| Decryption failure | security | Silently drop |
| Duplicate broadcast | mesh | Silently drop |
| No route for forwarding | routing | Silently drop |
| Neighbor table full | routing | Evict worst |
| Route table full | routing | Evict worst |
| Retransmit queue full | reliable | Drop oldest |

---

# 🔗 Compatibility

| Protocol version | Library version |
|-----------------|----------------|
| `0x03` | v3.x |

Nodes with mismatched `proto_ver` silently ignore each other.

---

<p align="center">
  <img src="https://capsule-render.vercel.app/api?type=waving&color=0:ff8c00,100:ff4500&height=120&section=footer"/>
</p>
