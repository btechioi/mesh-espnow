# ESP-NOW Mesh Protocol Specification

> **The on-wire format — every byte of every packet, explained.**

---

## Overview

All packets are sent via ESP-NOW and share a common header. The maximum ESP-NOW payload is **240 bytes**.

### Packet Layout

```
┌───────────────────────────────────────────────────────┐
│ Common Header (14 bytes)                               │
├───────────────────────────────────────────────────────┤
│ Payload (variable, type-dependent)                     │
├───────────────────────────────────────────────────────┤
│ [MIC Tag (8 bytes)] — only if encryption is enabled    │
└───────────────────────────────────────────────────────┘

Total: 14 + payload + (8 if encrypted) ≤ 240 bytes
Max payload: 226 bytes (encrypted) or 234 bytes (plain)
```

---

## Common Header (14 bytes)

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
├───────┼───────┼───────┼───────┼───────┼───────┼───────┼───────┤
│ proto_ver │ packet_type  │ reserved_1  │     ttl     │ hops   │
├───────┼───────┼───────┼───────┼───────┼───────┼───────┼───────┤
│                         src_id                              │
├───────┼───────┼───────┼───────┼───────┼───────┼───────┼───────┤
│                         dest_id                             │
└───────┴───────┴───────┴───────┴───────┴───────┴───────┴───────┘
```

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 1 | `proto_ver` | Protocol version (`0x02`). Packets with mismatched version are silently dropped. |
| 1 | 1 | `packet_type` | See [Packet Types](#packet-types) below |
| 2 | 1 | `reserved_1` | Reserved for future use. Must be `0x00`. |
| 3 | 1 | `ttl` | Time-To-Live. Decremented at each hop. Packet dropped when it reaches 0. |
| 4 | 1 | `hops` | Number of hops traversed so far. Incremented at each hop. |
| 5 | 4 | `src_id` | Source node ID (32-bit, network byte order). |
| 9 | 4 | `dest_id` | Destination node ID (32-bit, network byte order). |

### Header structure (mesh_priv.h)

```c
typedef struct __attribute__((packed)) {
    uint8_t  proto_ver;
    uint8_t  packet_type;
    uint8_t  reserved_1;
    uint8_t  ttl;
    uint8_t  hops;
    uint32_t src_id;
    uint32_t dest_id;
} mesh_packet_header_t;
```

---

## Packet Types

| Value | Name | Direction | Description |
|-------|------|-----------|-------------|
| `0x01` | `MESH_PACKET_TYPE_DATA` | unicast | Application data with ACK |
| `0x02` | `MESH_PACKET_TYPE_BROADCAST` | flood | Network-wide broadcast |
| `0x03` | `MESH_PACKET_TYPE_ACK` | unicast | Acknowledgment for DATA |
| `0x04` | `MESH_PACKET_TYPE_RREQ` | broadcast | Route request |
| `0x05` | `MESH_PACKET_TYPE_RREP` | unicast | Route reply |
| `0x06` | `MESH_PACKET_TYPE_BEACON` | broadcast | Periodic announcement |
| `0x07` | `MESH_PACKET_TYPE_GOODBYE` | broadcast | Graceful departure |

---

## Payload Formats

### DATA Packet (type `0x01`)

```
┌───────────────────────────────────────────────────────┐
│ Header (14 bytes)                                      │
│   packet_type = 0x01                                   │
│   dest_id = final destination                          │
├───────────────────────────────────────────────────────┤
│ Application payload (1 - 222 bytes)                    │
└───────────────────────────────────────────────────────┘
```

- Sent via unicast (ESP-NOW peer = next hop, not necessarily dest_id)
- Every hop re-encrypts and re-sends to the next hop
- Requires ACK from the **next hop** (per-hop reliability)
- Application payload is whatever you pass to `mesh_espnow_send()`

### Broadcast Packet (type `0x02`)

```
┌───────────────────────────────────────────────────────┐
│ Header (14 bytes)                                      │
│   packet_type = 0x02                                   │
│   dest_id = 0xFFFFFFFF (broadcast)                     │
├───────────────────────────────────────────────────────┤
│ Application payload (1 - 222 bytes)                    │
├───────────────────────────────────────────────────────┤
│ seq_num (4 bytes)  ─ duplicate suppression ID          │
└───────────────────────────────────────────────────────┘
```

- Flooded through the network: every node re-broadcasts once (TTL decremented, dup check)
- Duplicate suppression via `(src_id, seq_num)` cache
- Common uses: OTA triggers, alarm signals, configuration updates

### ACK Packet (type `0x03`)

```
┌───────────────────────────────────────────────────────┐
│ Header (14 bytes)                                      │
│   packet_type = 0x03                                   │
│   dest_id = original sender's src_id                   │
├───────────────────────────────────────────────────────┤
│ ack_seq (4 bytes)  ─ sequence number being ACKed      │
└───────────────────────────────────────────────────────┘
```

- Minimal 4-byte payload (just the sequence number being acknowledged)
- Always sent unicast back to the sender
- **Not encrypted** (must be verifiable even by intermediate nodes)
- **Note**: The reliable layer handles ACK matching. If no ACK arrives within `retransmit_timeout_ms`, the sender retransmits.

### RREQ Packet (type `0x04`)

```
┌───────────────────────────────────────────────────────┐
│ Header (14 bytes)                                      │
│   packet_type = 0x04                                   │
│   dest_id = 0xFFFFFFFF (broadcast)                     │
├───────────────────────────────────────────────────────┤
│ target_id (4 bytes)  ─ destination we're looking for  │
│ metric (2 bytes)     ─ current metric to source        │
└───────────────────────────────────────────────────────┘
```

- Broadcast to all neighbors
- Every node that receives it **learns a reverse route** back to `src_id`
- If this node **is** `target_id` or **has a route** to `target_id`, it replies with RREP
- Otherwise, re-broadcasts (if TTL > 0 and not already seen this RREQ)
- **Rate-limited**: max one RREQ per destination per second (exponential backoff)

### RREP Packet (type `0x05`)

```
┌───────────────────────────────────────────────────────┐
│ Header (14 bytes)                                      │
│   packet_type = 0x05                                   │
│   dest_id = original RREQ sender (src_id from RREQ)    │
├───────────────────────────────────────────────────────┤
│ target_id (4 bytes)  ─ the destination                  │
│ hop_count (1 byte)   ─ hops from replying node to dest │
│ metric (2 bytes)     ─ metric from replying node to dest│
└───────────────────────────────────────────────────────┘
```

- Sent unicast back along the reverse route established by the RREQ
- Each hop installs a forward route to `target_id`
- `hop_count` + `metric` let the RREQ originator compare multiple route options
- The first RREP wins (for primary route); subsequent RREPs may become the backup

### Beacon Packet (type `0x06`)

```
┌───────────────────────────────────────────────────────┐
│ Header (14 bytes)                                      │
│   packet_type = 0x06                                   │
│   dest_id = 0xFFFFFFFF (broadcast)                     │
├───────────────────────────────────────────────────────┤
│ capabilities (1 byte)   ─ bitmask of node capabilities │
│ gw_hops (1 byte)        ─ hop count to nearest gateway│
│ gw_id (4 bytes)         ─ gateway node ID (0 if none)  │
│ uptime_s (4 bytes)      ─ seconds since boot           │
│ battery_mv (4 bytes)    ─ battery in millivolts        │
└───────────────────────────────────────────────────────┘
```

**Total beacon payload: 14 bytes**

Broadcast every `beacon_interval_ms`. This is how nodes discover each other and learn the network topology.

#### Capabilities byte

| Bit | Flag | Meaning |
|-----|------|---------|
| 0 | `MESH_ESPNOW_CAP_GATEWAY` | This node is a network root |
| 1 | `MESH_ESPNOW_CAP_ROUTER` | This node forwards traffic |
| 2 | `MESH_ESPNOW_CAP_LEAF` | This node does NOT forward (battery saver) |
| 3 | `MESH_ESPNOW_CAP_SLEEPY` | This node may sleep at any time |

### GOODBYE Packet (type `0x07`)

```
┌───────────────────────────────────────────────────────┐
│ Header (14 bytes)                                      │
│   packet_type = 0x07                                   │
│   dest_id = 0xFFFFFFFF (broadcast)                     │
├───────────────────────────────────────────────────────┤
│ (no payload)                                           │
└───────────────────────────────────────────────────────┘
```

- Broadcast before stopping or sleeping
- Recipients immediately remove this node from their neighbor table and invalidate any routes through it
- 0 bytes of payload (just the header)

---

## Sequence Numbers

- Monotonically increasing per node (starts at 1)
- 32-bit, wraps around safely
- Used for:
  - **ACK matching**: sender matches ACK to original packet
  - **Duplicate suppression**: broadcast flood prevention
  - **Ordering**: application can detect out-of-order delivery

---

## Encryption (AES-128-CCM)

### When enabled

The entire packet (header + payload) is encrypted:

```
Plaintext on wire:
┌─────────────┬──────────────┬──────────────┐
│ Header (14) │ Payload (N)  │ MIC tag (8)  │
└─────────────┴──────────────┴──────────────┘

After encryption:
┌─────────────────────────┬──────────────┐
│ Ciphertext (14 + N)     │ MIC tag (8)  │
└─────────────────────────┴──────────────┘
```

Wait — actually, looking at the code more carefully, only the payload is encrypted, not the header. The header is sent in plaintext so intermediate nodes can read `dest_id` for forwarding. Let me correct this:

### Encryption scope

```
Wire format (encryption enabled):
┌─────────────┬─────────────────────────┬──────────────┐
│ Header (14) │ Ciphertext (payload)    │ MIC tag (8)  │
│ plaintext   │ encrypted with AES-128  │              │
└─────────────┴─────────────────────────┴──────────────┘

Wire format (encryption disabled):
┌─────────────┬───────────────────────┐
│ Header (14) │ Payload (plaintext)   │
│ plaintext   │                       │
└─────────────┴───────────────────────┘
```

### MIC scope

The MIC tag authenticates: `header + ciphertext`. If the MIC doesn't match at the destination:
1. The packet is silently dropped
2. `on_data` is never called
3. Destination may send a NACK (future feature)

### Nonce construction

```
nonce = node_id (4 bytes) || seq_num (4 bytes) || random (4 bytes)
Total nonce: 12 bytes (standard for CCM)
```

### Key

- 16-byte pre-shared key
- Same key on every node in the mesh
- Default: `"MESH-ESPNOW-MESH"`
- **Change for production**

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
    │   └── B re-broadcasts RREQ? NO (has route, so replies instead)
    │
    │   Neighbor C receives RREQ:
    │   ├── Learns reverse route to A via C's own MAC
    │   ├── C knows route to D? NO
    │   └── C re-broadcasts RREQ (TTL-1)
    │       └── ... eventually reaches D or a node with a route to D
    │
    ├── RREP arrives at A from B
    │   ├── Installs primary route to D: next_hop=B, hops=3 (A→B→x→D)
    │   └── State: now has route to D
    │
    └── A can now send DATA to D via next hop B
```

---

## Error Handling

### Packet drop reasons

| Reason | Log tag | Action |
|--------|---------|--------|
| Version mismatch | `mesh` | Silently drop |
| Hop limit exceeded | `mesh` | Silently drop |
| Decryption failure (MIC) | `security` | Silently drop |
| Duplicate broadcast | `mesh` | Silently drop |
| No route for forwarding | `routing` | Silently drop |
| Neighbor table full | `routing` | Evict worst neighbor |
| Route table full | `routing` | Evict worst route |
| Retransmit queue full | `reliable` | Drop oldest pending |

---

## Compatibility

| Protocol version | Library version | Notes |
|-----------------|----------------|-------|
| `0x02` | v3.x | Current. All nodes must use same version. |

Nodes with mismatched `proto_ver` **silently ignore each other's packets**. This means you can have separate meshes on the same channel with different protocol versions.
