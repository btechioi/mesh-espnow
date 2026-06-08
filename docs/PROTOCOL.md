# ESP-NOW Mesh Protocol Specification

> **The on-wire format — every byte of every packet, explained.**

---

## Overview

All packets are sent via ESP-NOW and share a common header. The maximum ESP-NOW payload is **250 bytes**; the mesh uses a max of **240 bytes** per packet.

### Packet Layout

```
┌───────────────────────────────────────────────────────┐
│ Common Header (24 bytes)                               │
├───────────────────────────────────────────────────────┤
│ Payload (variable, type-dependent)                     │
├───────────────────────────────────────────────────────┤
│ [MIC Tag (8 bytes)] — only if encryption is enabled    │
└───────────────────────────────────────────────────────┘

Total: 24 + payload + (8 if encrypted) ≤ 240 bytes
Max payload: 208 bytes (always, independent of encryption)
```

---

## Common Header (24 bytes)

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
| 0 | 1 | `proto_ver` | Protocol version (`0x03`). Mismatched versions are silently dropped. |
| 1 | 1 | `type` | Packer type (see below) |
| 2 | 1 | `ttl` | Time-To-Live. Decremented at each hop. Dropped at 0. |
| 3 | 4 | `src_id` | Source node ID (32-bit) |
| 7 | 4 | `dest_id` | Destination node ID (32-bit) |
| 11 | 4 | `seqno` | Monotonically increasing sequence number |
| 15 | 4 | `ack_seqno` | Sequence number being acknowledged (ACK packets) |
| 19 | 2 | `payload_len` | Length of payload in bytes |
| 21 | 1 | `flags` | Bitmask of packet flags (see below) |
| 22 | 2 | `subnet_id` | Source's sub-network ID (0 = global) |

### Header structure (from mesh_priv.h)

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

### Flags byte

| Bit | Constant | Meaning |
|-----|----------|---------|
| 0 | `MESH_FLAG_RREQ` | Packet is a route request (in DATA) |
| 1 | `MESH_FLAG_RREP` | Packet is a route reply (in DATA) |
| 2 | `MESH_FLAG_ACK` | This is an ACK packet |
| 3 | `MESH_FLAG_CROSS_SUBNET` | Packet has crossed subnets (loop prevention) |
| 7 | `MESH_FLAG_ENC` | Payload is encrypted |

---

## Packet Types

| Value | Name | Direction | Description |
|-------|------|-----------|-------------|
| `0x01` | `PKT_BEACON` | broadcast | Periodic "I'm here" announcement |
| `0x02` | `PKT_DATA` | unicast | Application data with hop-by-hop ACK |
| `0x03` | `PKT_DATA_ACK` | unicast | Acknowledgment for DATA |
| `0x04` | `PKT_RREQ` | broadcast | Route request |
| `0x05` | `PKT_RREP` | unicast | Route reply |
| `0x06` | `PKT_BROADCAST` | flood | Network-wide flood |
| `0x07` | `PKT_GOODBYE` | broadcast | Graceful departure |

---

## Payload Formats

### Beacon Packet (type `0x01`)

Broadcast every `beacon_interval_ms`. This is how nodes discover each other.

```
┌───────────────────────────────────────────────────────┐
│ Header (24 bytes)                                      │
│   type = 0x01   dest_id = 0xFFFFFFFF                   │
├───────────────────────────────────────────────────────┤
│ capabilities (1 byte)   — bitmask of node capabilities │
│ gw_hops (1 byte)        — hop count to nearest gateway │
│ gw_id (4 bytes)         — gateway node ID (0 if none)  │
│ uptime_s (4 bytes)      — seconds since boot           │
│ battery_mv (4 bytes)    — battery in millivolts        │
└───────────────────────────────────────────────────────┘
```

**Total beacon payload: 14 bytes**

#### Capabilities byte

| Bit | Flag | Meaning |
|-----|------|---------|
| 0 | `MESH_ESPNOW_CAP_GATEWAY` | This node is a network root |
| 1 | `MESH_ESPNOW_CAP_ROUTER` | This node forwards traffic |
| 2 | `MESH_ESPNOW_CAP_LEAF` | Battery-powered, doesn't forward |
| 3 | `MESH_ESPNOW_CAP_SLEEPY` | May sleep at any time |
| 4 | `MESH_ESPNOW_CAP_STORE_FWD` | Store-and-forward capable |
| 5 | `MESH_ESPNOW_CAP_BRIDGE` | Forwards between sub-networks |

---

### DATA Packet (type `0x02`)

```
┌───────────────────────────────────────────────────────┐
│ Header (24 bytes)                                      │
│   type = 0x02   dest_id = final destination            │
├───────────────────────────────────────────────────────┤
│ Application payload (1 - 208 bytes)                    │
└───────────────────────────────────────────────────────┘
```

- Sent via unicast (ESP-NOW peer = next hop, not necessarily dest_id)
- Every hop re-encrypts and forwards to the next hop
- Requires ACK from the **next hop** (per-hop reliability)

---

### ACK Packet (type `0x03`)

```
┌───────────────────────────────────────────────────────┐
│ Header (24 bytes)                                      │
│   type = 0x03   dest_id = original sender's src_id     │
│   ack_seqno = sequence number being ACKed              │
├───────────────────────────────────────────────────────┤
│ (no payload — ack_seqno is in the header)              │
└───────────────────────────────────────────────────────┘
```

- 0 bytes of payload (acknowledgment info is in the header)
- Always sent unicast back to the sender
- **Not encrypted** (must be verifiable by intermediate nodes)
- If no ACK within `retransmit_timeout_ms`, sender retransmits

---

### RREQ Packet (type `0x04`)

```
┌───────────────────────────────────────────────────────┐
│ Header (24 bytes)                                      │
│   type = 0x04   dest_id = 0xFFFFFFFF (broadcast)      │
├───────────────────────────────────────────────────────┤
│ target_id (4 bytes)  — destination we're looking for  │
│ metric (2 bytes)     — current metric to source        │
└───────────────────────────────────────────────────────┘
```

- Broadcast to all neighbors
- Every node that receives it **learns a reverse route** back to `src_id`
- If this node **is** `target_id` or **has a route** to it, it replies with RREP
- Otherwise, re-broadcasts (if TTL > 0 and not already seen)
- **Rate-limited**: exponential backoff (1s → 2s → 4s → 8s → 10s cap)

---

### RREP Packet (type `0x05`)

```
┌───────────────────────────────────────────────────────┐
│ Header (24 bytes)                                      │
│   type = 0x05   dest_id = original RREQ sender         │
├───────────────────────────────────────────────────────┤
│ target_id (4 bytes)  — the destination                  │
│ hop_count (1 byte)   — hops from replying node to dest │
│ metric (2 bytes)     — metric from replying node to dest│
└───────────────────────────────────────────────────────┘
```

- Sent unicast back along the reverse route established by the RREQ
- Each hop installs a forward route to `target_id`
- First RREP wins (primary); subsequent RREPs may become the backup

---

### Broadcast Packet (type `0x06`)

```
┌───────────────────────────────────────────────────────┐
│ Header (24 bytes)                                      │
│   type = 0x06   dest_id = 0xFFFFFFFF                   │
├───────────────────────────────────────────────────────┤
│ Application payload (1 - 208 bytes)                    │
└───────────────────────────────────────────────────────┘
```

- Flooded through the network: every node re-broadcasts once (TTL-1, dup check)
- Duplicate suppression via `(src_id, seqno)` cache (128 slots)
- Common uses: OTA triggers, alarm signals, configuration updates

---

### GOODBYE Packet (type `0x07`)

```
┌───────────────────────────────────────────────────────┐
│ Header (24 bytes)                                      │
│   type = 0x07   dest_id = 0xFFFFFFFF                   │
├───────────────────────────────────────────────────────┤
│ (no payload)                                           │
└───────────────────────────────────────────────────────┘
```

- Broadcast before stopping or sleeping
- Recipients immediately remove this node from neighbor table and invalidate routes
- 0 bytes of payload

---

## Sequence Numbers

- Monotonically increasing per node (starts at 1)
- 32-bit, wraps around safely
- Used for:
  - **ACK matching**: sender matches ACK to original packet via `ack_seqno`
  - **Duplicate suppression**: broadcast flood prevention
  - **Ordering**: application can detect out-of-order delivery

---

## Encryption (AES-128-CCM)

### Scope

```
Wire format (encryption enabled):
┌─────────────┬─────────────────────────┬──────────────┐
│ Header (24) │ Ciphertext (payload)    │ MIC tag (8)  │
│ plaintext   │ encrypted with AES-128  │              │
└─────────────┴─────────────────────────┴──────────────┘

Wire format (encryption disabled):
┌─────────────┬───────────────────────┐
│ Header (24) │ Payload (plaintext)   │
└─────────────┴───────────────────────┘
```

Only the payload is encrypted, not the header. Intermediate nodes need the header in plaintext for forwarding.

### MIC scope

The MIC tag authenticates: `header + ciphertext`. If MIC doesn't match at the destination:
1. The packet is silently dropped
2. `on_data` is never called

### Nonce construction

```
nonce = node_id (4 bytes) || seq_num (4 bytes) || random (4 bytes)
Total nonce: 12 bytes (standard for CCM)
```

### Key

- 16-byte pre-shared key
- Same key on every node in the mesh
- Default: `"MESH-ESPNOW-MESH"` — **change for production**

---

## Route Discovery Flow

```
Node A wants to send to Node D (not in route table)
    │
    ├── Check rate limit for D
    │   └── OK (first attempt)
    │
    ├── Broadcast RREQ: target=D, metric=0
    │
    │   Neighbor B receives RREQ:
    │   ├── Learns reverse route to A via B's own MAC
    │   ├── B knows route to D? YES
    │   │   └── Unicast RREP back to A:
    │   │       └── target=D, hop_count=2, metric=55
    │   └── B does NOT re-broadcast (replies instead)
    │
    │   Neighbor C receives RREQ:
    │   ├── Learns reverse route to A via C's own MAC
    │   ├── C knows route to D? NO
    │   └── C re-broadcasts RREQ (TTL-1)
    │       └── ... eventually reaches D or a node with a route
    │
    ├── RREP arrives at A from B
    │   ├── Installs primary route to D: next_hop=B
    │   └── State: now has route to D
    │
    └── A can now send DATA to D via next hop B
```

---

## Error Handling

### Packet drop reasons

| Reason | Log tag | Action |
|--------|---------|--------|
| Version mismatch | mesh | Silently drop |
| TTL exceeded | mesh | Silently drop |
| Decryption failure (MIC) | security | Silently drop |
| Duplicate broadcast | mesh | Silently drop |
| No route for forwarding | routing | Silently drop |
| Neighbor table full | routing | Evict worst neighbor |
| Route table full | routing | Evict worst route |
| Retransmit queue full | reliable | Drop oldest pending |

---

## Compatibility

| Protocol version | Library version | Notes |
|-----------------|----------------|-------|
| `0x03` | v3.x | Current. All nodes must use same version. |

Nodes with mismatched `proto_ver` **silently ignore each other's packets**.
