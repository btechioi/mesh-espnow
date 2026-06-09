/* mesh_reliable.c: ACK-based reliable delivery with retransmission */

#include "mesh_priv.h"

static const char *TAG = "mesh_rel";

/*============================================================================
 *  Data structures
 *============================================================================*/

typedef struct {
    bool     used;
    uint32_t dest_id;
    uint32_t next_hop;         /* for link quality tracking */
    uint8_t  payload[MESH_PAYLOAD_MAX];
    uint16_t payload_len;
    uint32_t seqno;
    uint32_t tx_time_ms;
    uint8_t  retries_left;
    uint8_t  retries_used;
    uint32_t start_time_ms;
    uint32_t timeout_ms;
} retx_entry_t;

/*============================================================================
 *  Static state
 *============================================================================*/

static struct {
    retx_entry_t *entries;
    uint16_t      max_entries;
    uint32_t      next_seqno;
    uint32_t      last_discovery_ms;
} s_rel;

/*============================================================================
 *  Init / Deinit
 *============================================================================*/

esp_err_t mesh_reliable_init(void) {
    s_rel.max_entries = MESH_ESPNOW_MAX_RETX;
    s_rel.entries = calloc(s_rel.max_entries, sizeof(retx_entry_t));
    if (!s_rel.entries) return ESP_ERR_NO_MEM;
    s_rel.next_seqno = (uint32_t)esp_random();
    s_rel.last_discovery_ms = 0;
    return ESP_OK;
}

void mesh_reliable_deinit(void) {
    free(s_rel.entries);
    s_rel.entries = NULL;
    s_rel.max_entries = 0;
}

/*============================================================================
 *  Send with reliability
 *============================================================================*/

esp_err_t mesh_reliable_send(uint32_t dest_id, const uint16_t *payload, uint16_t len) {
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t next_hop = 0;
    uint8_t  hops = 0;
    int8_t   rssi = 0;
    bool have_route = mesh_routing_find_route(dest_id, &next_hop, &hops, &rssi);

    if (!have_route) {
        if (now_ms - s_rel.last_discovery_ms > MESH_ESPNOW_RATE_LIMIT_MS) {
            s_rel.last_discovery_ms = now_ms;
            esp_err_t rerr = mesh_routing_discover_route(dest_id);
            if (rerr != ESP_OK) return rerr;
            vTaskDelay(pdMS_TO_TICKS(200));
            have_route = mesh_routing_find_route(dest_id, &next_hop, &hops, &rssi);
            if (!have_route) {
                return MESH_ESPNOW_ERR_NO_ROUTE;
            }
        } else {
            return MESH_ESPNOW_ERR_NO_ROUTE;
        }
    }

    /* Allocate retransmission slot */
    retx_entry_t *re = NULL;
    for (uint16_t i = 0; i < s_rel.max_entries; i++) {
        if (!s_rel.entries[i].used) {
            re = &s_rel.entries[i];
            break;
        }
    }

    if (!re) {
        uint32_t oldest = UINT32_MAX;
        for (uint16_t i = 0; i < s_rel.max_entries; i++) {
            if (s_rel.entries[i].start_time_ms < oldest) {
                oldest = s_rel.entries[i].start_time_ms;
                re = &s_rel.entries[i];
            }
        }
        if (re) {
            RELIABLE_LOG(ESP_LOG_DEBUG, "Evicting retx entry for 0x%08X", re->dest_id);
        }
    }
    if (!re) return ESP_ERR_NO_MEM;

    uint32_t seqno = s_rel.next_seqno++;
    uint8_t *data = (uint8_t *)payload;
    esp_err_t err = mesh_core_send_packet_to(dest_id, next_hop, data, len, PKT_DATA, 0, 0);
    if (err != ESP_OK) return err;

    re->used = true;
    re->dest_id = dest_id;
    re->next_hop = next_hop;
    re->payload_len = len > MESH_PAYLOAD_MAX ? MESH_PAYLOAD_MAX : len;
    memcpy(re->payload, data, re->payload_len);
    re->seqno = seqno;
    re->tx_time_ms = now_ms;
    re->retries_left = g_mesh.config.max_retransmits;
    re->retries_used = 0;
    re->start_time_ms = now_ms;
    re->timeout_ms = g_mesh.config.retransmit_timeout_ms;

    RELIABLE_LOG(ESP_LOG_DEBUG, "Sent seq %u to 0x%08X via 0x%08X", seqno, dest_id, next_hop);
    return ESP_OK;
}

/*============================================================================
 *  ACK handler
 *============================================================================*/

void mesh_reliable_handle_ack(uint32_t src, uint32_t ack_seqno) {
    for (uint16_t i = 0; i < s_rel.max_entries; i++) {
        if (s_rel.entries[i].used && s_rel.entries[i].seqno == ack_seqno) {
            uint32_t latency = (uint32_t)(esp_timer_get_time() / 1000) - s_rel.entries[i].tx_time_ms;

            g_mesh.stats.avg_tx_latency_ms =
                (g_mesh.stats.avg_tx_latency_ms * 0.9f) + (latency * 0.1f);
            if (latency > g_mesh.stats.peak_tx_latency_ms) {
                g_mesh.stats.peak_tx_latency_ms = latency;
            }

            /* Report TX success for link quality tracking */
            mesh_routing_record_tx_success(s_rel.entries[i].next_hop);

            RELIABLE_LOG(ESP_LOG_DEBUG, "ACK for seq %u from 0x%08X (%u ms)", ack_seqno, src, latency);
            s_rel.entries[i].used = false;
            return;
        }
    }
}

/*============================================================================
 *  Periodic processing (retransmissions + timeouts)
 *============================================================================*/

void mesh_reliable_process(uint32_t now_ms) {
    for (uint16_t i = 0; i < s_rel.max_entries; i++) {
        if (!s_rel.entries[i].used) continue;

        retx_entry_t *re = &s_rel.entries[i];
        uint32_t elapsed = now_ms - re->tx_time_ms;

        if (elapsed >= re->timeout_ms) {
            if (re->retries_left > 0) {
                re->retries_left--;
                re->retries_used++;
                re->tx_time_ms = now_ms;
                re->timeout_ms = re->timeout_ms * 3 / 2;
                g_mesh.stats.retransmissions++;

                /* Report failure for link quality */
                mesh_routing_record_tx_failure(re->next_hop);

                /* Re-resolve route for retransmit */
                uint32_t nh = re->next_hop;
                uint8_t  nh_hops;
                int8_t   nh_rssi;
                if (mesh_routing_find_route(re->dest_id, &nh, &nh_hops, &nh_rssi)) {
                    re->next_hop = nh;
                }
                mesh_core_send_packet_to(re->dest_id, re->next_hop,
                                          re->payload, re->payload_len, PKT_DATA, 0, 0);
                RELIABLE_LOG(ESP_LOG_DEBUG, "Retx seq %u to 0x%08X (retry %d/%d)",
                             re->seqno, re->dest_id, re->retries_used, g_mesh.config.max_retransmits);
            } else {
                /* Report final failure */
                mesh_routing_record_tx_failure(re->next_hop);

                RELIABLE_LOG(ESP_LOG_WARN, "Gave up on seq %u to 0x%08X", re->seqno, re->dest_id);
                re->used = false;
                g_mesh.stats.dropped++;
            }
        }
    }
}
