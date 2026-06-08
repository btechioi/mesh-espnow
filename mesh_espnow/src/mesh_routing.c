/* mesh_routing.c — Intelligent metric-based AODV routing
 *
 * Design:
 *   - Route metric = weighted sum of hop count, RSSI, battery, capabilities, reliability
 *   - Lower metric = better route
 *   - Each destination can have a primary + backup route (multi-path)
 *   - Automatic periodic route optimization
 *   - Link quality tracked per neighbor (success/fail ratio)
 *   - Battery-aware: prefer mains-powered routers over battery leaf nodes
 *   - Proactive repair: try backup before initiating new RREQ
 */

#include "mesh_priv.h"

static const char *TAG = "mesh_route";

/*============================================================================
 *  Metric weights (tunable constants)
 *============================================================================*/

#define METRIC_HOP_WT        20    /* penalty per hop */
#define METRIC_RSSI_WT        2    /* penalty per dBm above -50 */
#define METRIC_BATTERY_WT    10    /* per 100mV below 3300 */
#define METRIC_CAP_LEAF_WT   30    /* penalty for leaf nodes */
#define METRIC_RELIABILITY_WT 25   /* penalty if PDR < 50% */
#define METRIC_MAX          65535  /* unreachable */

/*============================================================================
 *  Data structures
 *============================================================================*/

typedef struct {
    bool     used;
    uint32_t node_id;

    /* Signal */
    int8_t   rssi;
    int8_t   rssi_min;
    int8_t   rssi_max;
    uint32_t last_seen_ms;

    /* Topology */
    uint8_t  hop_count;       /* their hops to gateway */
    uint8_t  capabilities;    /* bitmask */
    uint32_t uptime_s;

    /* Battery awareness */
    uint32_t battery_mv;      /* their reported battery (0 = unknown) */

    /* Link quality tracking */
    uint32_t tx_attempts;     /* packets we sent to them */
    uint32_t tx_successes;    /* ACKs received back */
    uint32_t rx_count;        /* packets received from them */

    /* Routing info announced in beacon */
    uint32_t announced_gateway;
    uint8_t  announced_hops;

    /* Sub-network */
    uint8_t  subnet_id;          /* their subnet (0 = global) */
    uint8_t  subnet_channel;     /* their subnet's channel */
} neighbor_entry_t;

typedef struct {
    bool     used;

    /* Destination */
    uint32_t dest;

    /* Primary path */
    uint32_t next_hop;
    uint8_t  hop_count;
    int8_t   rssi;
    uint32_t last_seen_ms;

    /* Backup path (multi-path) */
    bool     backup_valid;
    uint32_t backup_next_hop;
    uint8_t  backup_hop_count;
    int8_t   backup_rssi;

    /* Metric (lower = better) */
    uint16_t metric;

    /* Sub-network */
    uint8_t  subnet_id;      /* destination's subnet */

    /* Bridge */
    bool     via_bridge;     /* true if this route goes through a bridge to another subnet */

    /* Rate limiting */
    uint32_t last_rreq_ms;
    uint8_t  rreq_attempts;
} route_entry_t;

/*============================================================================
 *  Static state
 *============================================================================*/

static struct {
    neighbor_entry_t *neighbors;
    route_entry_t    *routes;
    uint16_t          neighbor_max;
    uint16_t          route_max;

    /* Gateway tracking */
    uint32_t          gateway_id;
    uint32_t          parent_id;
    uint16_t          gateway_metric;

    /* Optimization timer */
    uint32_t          last_optimize_ms;
    uint32_t          optimize_interval_ms;

    /* Stats */
    uint32_t          route_repairs;
    uint32_t          route_switches;
} s_routing;

/*============================================================================
 *  Forward declarations
 *============================================================================*/

static uint16_t compute_metric(uint8_t hops, int8_t rssi, uint8_t caps, uint32_t battery_mv,
                               uint32_t tx_attempts, uint32_t tx_successes, bool same_subnet);
static void     route_reevaluate_gateway(void);

/*============================================================================
 *  Init / Deinit
 *============================================================================*/

esp_err_t mesh_routing_init(void) {
    s_routing.neighbor_max = g_mesh.config.max_neighbors;
    s_routing.route_max    = g_mesh.config.max_routes;

    s_routing.neighbors = calloc(s_routing.neighbor_max, sizeof(neighbor_entry_t));
    s_routing.routes    = calloc(s_routing.route_max, sizeof(route_entry_t));
    if (!s_routing.neighbors || !s_routing.routes) {
        free(s_routing.neighbors);
        free(s_routing.routes);
        s_routing.neighbors = NULL;
        s_routing.routes = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_routing.gateway_id          = 0;
    s_routing.parent_id           = 0;
    s_routing.gateway_metric      = METRIC_MAX;
    s_routing.last_optimize_ms    = 0;
    s_routing.optimize_interval_ms = 15000; /* every 15s */
    s_routing.route_repairs       = 0;
    s_routing.route_switches      = 0;

    ROUTE_LOG(ESP_LOG_INFO, "Intelligent routing: %u neighbors, %u routes, optimize=%ums",
              s_routing.neighbor_max, s_routing.route_max, s_routing.optimize_interval_ms);
    return ESP_OK;
}

void mesh_routing_deinit(void) {
    free(s_routing.neighbors);
    free(s_routing.routes);
    s_routing.neighbors = NULL;
    s_routing.routes = NULL;
    s_routing.neighbor_max = 0;
    s_routing.route_max = 0;
    s_routing.gateway_id = 0;
    s_routing.parent_id = 0;
}

/*============================================================================
 *  Metric computation
 *
 *  Lower is better. Combines multiple factors into a single score.
 *============================================================================*/

static uint16_t compute_metric(uint8_t hops, int8_t rssi, uint8_t caps, uint32_t battery_mv,
                                uint32_t tx_attempts, uint32_t tx_successes, bool same_subnet) {
    uint32_t m = 0;

    /* 1. Hop count — primary cost */
    m += (uint32_t)hops * METRIC_HOP_WT;

    /* 2. RSSI — signal quality penalty
     *    Perfect RSSI = -50 or better: 0 penalty
     *    At -100: 100 penalty (very poor)
     */
    {
        int rssi_penalty = (-rssi - 50);
        if (rssi_penalty < 0)   rssi_penalty = 0;
        if (rssi_penalty > 100) rssi_penalty = 100;
        m += (uint32_t)rssi_penalty * METRIC_RSSI_WT;
    }

    /* 3. Subnet affinity — prefer same-subnet neighbors (avoids bridge hops) */
    if (same_subnet) {
        m -= METRIC_CAP_LEAF_WT;  /* -30 bonus for same subnet */
    } else {
        m += METRIC_CAP_LEAF_WT;  /* +30 penalty for cross-subnet */
    }

    /* 4. Capability — prefer routers over leaf nodes */
    if (caps & MESH_ESPNOW_CAP_GATEWAY) {
        m -= METRIC_CAP_LEAF_WT;  /* bonus for gateways */
    } else if (!(caps & MESH_ESPNOW_CAP_ROUTER)) {
        m += METRIC_CAP_LEAF_WT;  /* penalty for leaf-only */
    }

    /* 5. Battery — prefer mains-powered */
    if (battery_mv > 0 && battery_mv < 3300) {
        uint32_t batt_penalty = (3300 - battery_mv) / 100;
        m += batt_penalty * METRIC_BATTERY_WT;
    }

    /* 6. Link reliability — packet delivery ratio */
    if (tx_attempts > 5) {
        uint32_t pdr = (tx_successes * 100) / tx_attempts;
        if (pdr < 50) {
            m += METRIC_RELIABILITY_WT;
        } else if (pdr < 80) {
            m += METRIC_RELIABILITY_WT / 2;
        }
    }

    if (m > METRIC_MAX) m = METRIC_MAX;
    return (uint16_t)m;
}

/*============================================================================
 *  Neighbor management
 *============================================================================*/

static neighbor_entry_t* neighbor_find(uint32_t node_id) {
    for (uint16_t i = 0; i < s_routing.neighbor_max; i++) {
        if (s_routing.neighbors[i].used && s_routing.neighbors[i].node_id == node_id) {
            return &s_routing.neighbors[i];
        }
    }
    return NULL;
}

static neighbor_entry_t* neighbor_alloc(void) {
    /* Find worst neighbor to evict by metric */
    uint16_t worst_metric = 0;
    neighbor_entry_t *worst = &s_routing.neighbors[0];

    for (uint16_t i = 0; i < s_routing.neighbor_max; i++) {
        if (!s_routing.neighbors[i].used) return &s_routing.neighbors[i];

        uint16_t m = compute_metric(
            s_routing.neighbors[i].hop_count,
            s_routing.neighbors[i].rssi,
            s_routing.neighbors[i].capabilities,
            s_routing.neighbors[i].battery_mv,
            s_routing.neighbors[i].tx_attempts,
            s_routing.neighbors[i].tx_successes,
            s_routing.neighbors[i].subnet_id == g_mesh.config.subnet_id
        );
        if (m >= worst_metric) {
            worst_metric = m;
            worst = &s_routing.neighbors[i];
        }
    }
    return worst;
}

void mesh_routing_add_neighbor(uint32_t node_id, uint8_t hops, uint8_t caps, int8_t rssi,
                                uint8_t subnet_id, uint8_t subnet_channel) {
    if (node_id == 0 || node_id == g_mesh.config.node_id) return;

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    neighbor_entry_t *n = neighbor_find(node_id);

    if (!n) {
        n = neighbor_alloc();
        bool was_used = n->used;

        if (was_used) {
            ROUTE_LOG(ESP_LOG_DEBUG, "Evict neighbor 0x%08X for 0x%08X (metric-based)", n->node_id, node_id);
        }

        n->node_id      = node_id;
        n->used         = true;
        n->rssi_min     = rssi;
        n->rssi_max     = rssi;
        n->tx_attempts  = 0;
        n->tx_successes = 0;
        n->rx_count     = 0;
        n->battery_mv   = 0;
        n->subnet_id    = subnet_id;
        n->subnet_channel = subnet_channel;

        if (!was_used) {
            ROUTE_LOG(ESP_LOG_INFO, "New neighbor: 0x%08X (RSSI %d, subnet %d)", node_id, rssi, subnet_id);
            if (g_mesh.config.callbacks.on_node_discovered) {
                g_mesh.config.callbacks.on_node_discovered(node_id, rssi);
            }
        }
    }

    n->last_seen_ms  = now_ms;
    n->subnet_id     = subnet_id;
    n->subnet_channel = subnet_channel;
    n->rssi          = (n->rssi * 3 + rssi) / 4; /* EWMA with stronger smoothing */
    if (rssi < n->rssi_min) n->rssi_min = rssi;
    if (rssi > n->rssi_max) n->rssi_max = rssi;
    n->capabilities  = caps;
    n->rx_count++;

    if (hops != 0xFF) {
        n->hop_count      = hops;
        n->announced_hops = hops;
    }
}

void mesh_routing_remove_neighbor(uint32_t node_id) {
    neighbor_entry_t *n = neighbor_find(node_id);
    if (n) {
        ROUTE_LOG(ESP_LOG_INFO, "Neighbor lost: 0x%08X", node_id);
        n->used = false;
        n->node_id = 0;
    }

    /* Demote routes through this neighbor, try backups */
    for (uint16_t i = 0; i < s_routing.route_max; i++) {
        route_entry_t *r = &s_routing.routes[i];
        if (!r->used) continue;

        if (r->next_hop == node_id) {
            if (r->backup_valid) {
                /* Promote backup to primary */
                ROUTE_LOG(ESP_LOG_INFO, "Route repair: 0x%08X via backup 0x%08X", r->dest, r->backup_next_hop);
                r->next_hop     = r->backup_next_hop;
                r->hop_count    = r->backup_hop_count;
                r->rssi         = r->backup_rssi;
                r->backup_valid = false;
                s_routing.route_repairs++;
            } else {
                r->used = false;
                ROUTE_LOG(ESP_LOG_INFO, "Route to 0x%08X invalidated (no backup)", r->dest);
            }
        }

        if (r->backup_valid && r->backup_next_hop == node_id) {
            r->backup_valid = false;
        }
    }

    if (s_routing.parent_id == node_id) {
        s_routing.parent_id = 0;
    }
    if (s_routing.gateway_id == node_id) {
        s_routing.gateway_id = 0;
    }

    route_reevaluate_gateway();
}

/*============================================================================
 *  Route management (multi-path)
 *============================================================================*/

static route_entry_t* route_find(uint32_t dest) {
    for (uint16_t i = 0; i < s_routing.route_max; i++) {
        if (s_routing.routes[i].used && s_routing.routes[i].dest == dest) {
            return &s_routing.routes[i];
        }
    }
    return NULL;
}

static route_entry_t* route_alloc(void) {
    /* Evict highest-metric route */
    uint16_t worst_metric = 0;
    route_entry_t *worst = &s_routing.routes[0];

    for (uint16_t i = 0; i < s_routing.route_max; i++) {
        if (!s_routing.routes[i].used) return &s_routing.routes[i];
        if (s_routing.routes[i].metric >= worst_metric) {
            worst_metric = s_routing.routes[i].metric;
            worst = &s_routing.routes[i];
        }
    }
    return worst;
}

static void route_set(route_entry_t *r, uint32_t next_hop, uint8_t hops,
                      int8_t rssi, uint16_t metric, uint32_t now_ms,
                      uint8_t subnet_id, bool via_bridge) {
    r->next_hop     = next_hop;
    r->hop_count    = hops;
    r->rssi         = rssi;
    r->metric       = metric;
    r->last_seen_ms = now_ms;
    r->subnet_id    = subnet_id;
    r->via_bridge   = via_bridge;
}

void mesh_routing_update_route(uint32_t dest, uint32_t next_hop, uint8_t hops, int8_t rssi) {
    if (dest == 0 || dest == g_mesh.config.node_id) return;
    if (next_hop == 0) return;

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    neighbor_entry_t *n = neighbor_find(next_hop);

    uint8_t caps = n ? n->capabilities : 0;
    uint32_t batt = n ? n->battery_mv : 0;
    uint32_t tx_att = n ? n->tx_attempts : 0;
    uint32_t tx_ok  = n ? n->tx_successes : 0;

    bool same_subnet = (n && (g_mesh.config.subnet_id == 0 || n->subnet_id == g_mesh.config.subnet_id));
    uint8_t r_subnet_id  = n ? n->subnet_id : 0;
    bool    r_via_bridge = (n && g_mesh.config.subnet_id != 0 && n->subnet_id != g_mesh.config.subnet_id);
    uint16_t new_metric = compute_metric(hops, rssi, caps, batt, tx_att, tx_ok, same_subnet);
    route_entry_t *r = route_find(dest);

    if (!r) {
        /* New route — install as primary */
        r = route_alloc();
        r->dest         = dest;
        r->used         = true;
        r->backup_valid = false;
        r->last_rreq_ms = 0;
        r->rreq_attempts = 0;

        route_set(r, next_hop, hops, rssi, new_metric, now_ms, r_subnet_id, r_via_bridge);

        ROUTE_LOG(ESP_LOG_INFO, "New route: 0x%08X → 0x%08X (%uhops, metric=%u)",
                  dest, next_hop, hops, new_metric);

        if (g_mesh.config.callbacks.on_route_changed) {
            g_mesh.config.callbacks.on_route_changed(dest, next_hop, hops);
        }

        goto check_gateway;
    }

    /* Route exists — compare metrics */
    if (new_metric < r->metric) {
        /* New path is better — demote current to backup, install new primary */
        if (r->next_hop != next_hop) {
            r->backup_valid    = true;
            r->backup_next_hop = r->next_hop;
            r->backup_hop_count = r->hop_count;
            r->backup_rssi     = r->rssi;
            s_routing.route_switches++;
            ROUTE_LOG(ESP_LOG_INFO, "Route switch: 0x%08X via 0x%08X (metric %u < %u)",
                      dest, next_hop, new_metric, r->metric);
        }

        route_set(r, next_hop, hops, rssi, new_metric, now_ms, r_subnet_id, r_via_bridge);

        if (g_mesh.config.callbacks.on_route_changed) {
            g_mesh.config.callbacks.on_route_changed(dest, next_hop, hops);
        }
    } else if (next_hop != r->next_hop && next_hop != r->backup_next_hop) {
        /* Not better than primary, but keep as backup */
        r->backup_valid     = true;
        r->backup_next_hop  = next_hop;
        r->backup_hop_count = hops;
        r->backup_rssi      = rssi;
        ROUTE_LOG(ESP_LOG_DEBUG, "Backup route: 0x%08X → 0x%08X (metric %u)", dest, next_hop, new_metric);
    } else if (next_hop == r->next_hop) {
        /* Update existing primary */
        uint32_t old_metric = r->metric;
        route_set(r, next_hop, hops, rssi, new_metric, now_ms, r_subnet_id, r_via_bridge);
        if (new_metric != old_metric) {
            ROUTE_LOG(ESP_LOG_DEBUG, "Route metric update: 0x%08X %u→%u", dest, old_metric, new_metric);
        }
    }

check_gateway:
    route_reevaluate_gateway();
}

/*============================================================================
 *  Gateway management
 *============================================================================*/

static void route_reevaluate_gateway(void) {
    uint32_t best_gw = 0;
    uint16_t best_metric = METRIC_MAX;
    uint32_t best_parent = 0;

    if (mesh_core_is_gateway()) {
        s_routing.gateway_id = g_mesh.config.node_id;
        s_routing.parent_id  = g_mesh.config.node_id;
        return;
    }

    /* Check if any neighbor is a gateway */
    for (uint16_t i = 0; i < s_routing.neighbor_max; i++) {
        neighbor_entry_t *n = &s_routing.neighbors[i];
        if (!n->used) continue;

        bool same_subnet = (g_mesh.config.subnet_id == 0 || n->subnet_id == g_mesh.config.subnet_id);

        if (n->capabilities & MESH_ESPNOW_CAP_GATEWAY) {
            uint16_t m = compute_metric(1, n->rssi, n->capabilities, n->battery_mv,
                                        n->tx_attempts, n->tx_successes, same_subnet);
            if (m < best_metric) {
                best_metric = m;
                best_gw     = n->node_id;
                best_parent = n->node_id;
            }
        }

        /* Neighbor reports gateway route */
        if (n->announced_gateway != 0 && n->announced_hops < 0xFF) {
            uint16_t m = compute_metric(n->announced_hops + 1, n->rssi, n->capabilities,
                                        n->battery_mv, n->tx_attempts, n->tx_successes, same_subnet);
            if (m < best_metric) {
                best_metric = m;
                best_gw     = n->announced_gateway;
                best_parent = n->node_id;
            }
        }
    }

    /* Also scan route table for gateway routes */
    for (uint16_t i = 0; i < s_routing.route_max; i++) {
        route_entry_t *r = &s_routing.routes[i];
        if (!r->used) continue;

        neighbor_entry_t *n = neighbor_find(r->next_hop);
        if (n && (n->capabilities & MESH_ESPNOW_CAP_GATEWAY)) {
            if (r->metric < best_metric) {
                best_metric = r->metric;
                best_gw     = r->dest;
                best_parent = r->next_hop;
            }
        }
    }

    if (best_gw != 0 && best_metric < s_routing.gateway_metric) {
        s_routing.gateway_id     = best_gw;
        s_routing.parent_id      = best_parent;
        s_routing.gateway_metric = best_metric;
        ROUTE_LOG(ESP_LOG_INFO, "Gateway: 0x%08X via 0x%08X (metric=%u)", best_gw, best_parent, best_metric);
    } else if (best_gw == 0) {
        s_routing.gateway_id     = 0;
        s_routing.parent_id      = 0;
        s_routing.gateway_metric = METRIC_MAX;
    }
}

/*============================================================================
 *  Route lookup with intelligent selection
 *============================================================================*/

bool mesh_routing_find_route(uint32_t dest, uint32_t *next_hop, uint8_t *hops, int8_t *rssi) {
    if (dest == 0) return false;

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    /* 1. Direct neighbor check (always best) */
    neighbor_entry_t *n = neighbor_find(dest);
    if (n) {
        if (next_hop) *next_hop = dest;
        if (hops)     *hops     = 1;
        if (rssi)     *rssi     = n->rssi;
        return true;
    }

    /* 2. Route table lookup */
    route_entry_t *r = route_find(dest);
    if (r) {
        /* Verify next hop is still a neighbor */
        neighbor_entry_t *nh = neighbor_find(r->next_hop);
        if (nh) {
            r->last_seen_ms = now_ms;
            if (next_hop) *next_hop = r->next_hop;
            if (hops)     *hops     = r->hop_count;
            if (rssi)     *rssi     = r->rssi;
            return true;
        }

        /* Primary next hop gone — try backup */
        if (r->backup_valid) {
            neighbor_entry_t *bn = neighbor_find(r->backup_next_hop);
            if (bn) {
                /* Promote backup */
                ROUTE_LOG(ESP_LOG_INFO, "Auto route repair: 0x%08X via 0x%08X (backup)", dest, r->backup_next_hop);
                r->next_hop     = r->backup_next_hop;
                r->hop_count    = r->backup_hop_count;
                r->rssi         = r->backup_rssi;
                r->backup_valid = false;
                r->last_seen_ms = now_ms;
                s_routing.route_repairs++;

                if (next_hop) *next_hop = r->next_hop;
                if (hops)     *hops     = r->hop_count;
                if (rssi)     *rssi     = r->rssi;
                return true;
            }
        }

        r->used = false;
    }

    return false;
}

/*============================================================================
 *  Route discovery (RREQ / RREP)
 *============================================================================*/

esp_err_t mesh_routing_discover_route(uint32_t dest_id) {
    if (dest_id == 0 || dest_id == g_mesh.config.node_id) {
        return MESH_ESPNOW_ERR_INVALID_PARAM;
    }

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    route_entry_t *r = route_find(dest_id);
    uint32_t last = r ? r->last_rreq_ms : 0;
    uint8_t  attempts = r ? r->rreq_attempts : 0;

    /* Rate limiting with exponential backoff */
    uint32_t backoff = MESH_ESPNOW_RATE_LIMIT_MS * (1 << attempts);
    if (backoff > 10000) backoff = 10000;

    if (now_ms - last < backoff) {
        ROUTE_LOG(ESP_LOG_DEBUG, "RREQ rate-limited for 0x%08X (backoff=%ums)", dest_id, backoff);
        return MESH_ESPNOW_ERR_RATE_LIMITED;
    }

    /* RREQ payload: dest_id (4) + metric (2) */
    uint8_t rreq_payload[6];
    rreq_payload[0] = (dest_id >> 24) & 0xFF;
    rreq_payload[1] = (dest_id >> 16) & 0xFF;
    rreq_payload[2] = (dest_id >> 8) & 0xFF;
    rreq_payload[3] = dest_id & 0xFF;

    uint16_t my_metric = 0; /* starting metric for originator */
    rreq_payload[4] = (my_metric >> 8) & 0xFF;
    rreq_payload[5] = my_metric & 0xFF;

    esp_err_t err = mesh_core_send_packet(0xFFFFFFFF, rreq_payload, 6, PKT_RREQ, 0, MESH_FLAG_RREQ);
    if (err == ESP_OK) {
        if (!r) {
            r = route_alloc();
            r->dest = dest_id;
            r->used = true;
            r->backup_valid = false;
            r->last_rreq_ms = 0;
            r->rreq_attempts = 0;
        }
        r->last_rreq_ms = now_ms;
        r->rreq_attempts++;
        g_mesh.stats.rreqs_sent++;

        ROUTE_LOG(ESP_LOG_INFO, "RREQ for 0x%08X (attempt %u, backoff=%ums)",
                  dest_id, r->rreq_attempts, backoff);
    }
    return err;
}

void mesh_routing_handle_rreq(uint32_t src, uint32_t orig, uint32_t seqno, uint8_t ttl, int8_t rssi) {
    (void)seqno;
    (void)ttl;

    /* Learn reverse route to source (immediate neighbor) */
    mesh_routing_update_route(src, src, 1, rssi);

    /* Update link quality */
    neighbor_entry_t *n = neighbor_find(src);
    if (n) n->rx_count++;

    /* If we are the destination being sought, reply with RREP */
    if (orig != 0) {
        if (orig == g_mesh.config.node_id) {
            /* We ARE the destination — send RREP back to source */
            uint8_t rrep_data[4];
            rrep_data[0] = (orig >> 24) & 0xFF;
            rrep_data[1] = (orig >> 16) & 0xFF;
            rrep_data[2] = (orig >> 8) & 0xFF;
            rrep_data[3] = orig & 0xFF;

            mesh_core_send_packet(src, rrep_data, 4, PKT_RREP, 0, MESH_FLAG_RREP);
            g_mesh.stats.rreps_sent++;
            ROUTE_LOG(ESP_LOG_INFO, "RREP to 0x%08X (I am the destination)", src);
        } else {
            /* We have a route to the destination — reply with it */
            uint32_t nh;
            uint8_t  hcnt;
            int8_t   rs;
            if (mesh_routing_find_route(orig, &nh, &hcnt, &rs)) {
                uint8_t rrep_data[6];
                rrep_data[0] = (orig >> 24) & 0xFF;
                rrep_data[1] = (orig >> 16) & 0xFF;
                rrep_data[2] = (orig >> 8) & 0xFF;
                rrep_data[3] = orig & 0xFF;
                rrep_data[4] = (hcnt + 1);
                rrep_data[5] = (uint8_t)(-rs);

                mesh_core_send_packet(src, rrep_data, 6, PKT_RREP, 0, MESH_FLAG_RREP);
                g_mesh.stats.rreps_sent++;
                ROUTE_LOG(ESP_LOG_DEBUG, "RREP to 0x%08X for 0x%08X (%u hops)", src, orig, hcnt + 1);
            }
        }
    }
}

void mesh_routing_handle_rrep(uint32_t src, uint32_t dest, uint32_t seqno, uint8_t hops, int8_t rssi) {
    (void)seqno;

    mesh_routing_update_route(dest, src, hops, rssi);
    g_mesh.stats.rreps_received++;

    /* Update link quality */
    neighbor_entry_t *n = neighbor_find(src);
    if (n) n->rx_count++;
}

/*============================================================================
 *  Beacon handler
 *============================================================================*/

void mesh_routing_handle_beacon(uint32_t node_id, uint8_t caps, uint8_t hops,
                                uint8_t gateway, uint32_t uptime_s, int8_t rssi,
                                uint8_t subnet_id, uint8_t subnet_channel) {
    uint32_t gw_id = gateway;

    /* Update neighbor */
    mesh_routing_add_neighbor(node_id, hops, caps, rssi, subnet_id, subnet_channel);

    neighbor_entry_t *n = neighbor_find(node_id);
    if (n) {
        n->uptime_s          = uptime_s;
        n->announced_gateway = gw_id;
        n->announced_hops    = hops;
    }

    /* Gateway direct neighbor */
    if (caps & MESH_ESPNOW_CAP_GATEWAY) {
        mesh_routing_update_route(node_id, node_id, 1, rssi);
    }

    /* Neighbor announces gateway route */
    if (gw_id != 0 && gw_id != node_id) {
        mesh_routing_update_route(gw_id, node_id, hops + 1, rssi);
    }
}

/*============================================================================
 *  Periodic optimization
 *
 *  Every optimize_interval_ms, evaluate if any neighbor offers a better
 *  route than the current primary for any destination. This handles
 *  network topology changes gracefully.
 *============================================================================*/

static void route_optimize(uint32_t now_ms) {
    if (now_ms - s_routing.last_optimize_ms < s_routing.optimize_interval_ms) return;
    s_routing.last_optimize_ms = now_ms;

    uint16_t optimizations = 0;

    for (uint16_t ri = 0; ri < s_routing.route_max; ri++) {
        route_entry_t *r = &s_routing.routes[ri];
        if (!r->used) continue;

        uint16_t best_metric = r->metric;
        uint32_t best_next   = r->next_hop;
        uint8_t  best_hops   = r->hop_count;
        int8_t   best_rssi   = r->rssi;
        bool     improved    = false;

        /* Check all neighbors for potentially better paths */
        for (uint16_t ni = 0; ni < s_routing.neighbor_max; ni++) {
            neighbor_entry_t *n = &s_routing.neighbors[ni];
            if (!n->used) continue;
            if (n->node_id == r->next_hop) continue; /* already primary */

            /* Can this neighbor route to dest? Only if it's the dest itself */
            if (n->node_id == r->dest) {
                bool same_subnet = (g_mesh.config.subnet_id == 0 || n->subnet_id == g_mesh.config.subnet_id);
                uint16_t m = compute_metric(1, n->rssi, n->capabilities, n->battery_mv,
                                            n->tx_attempts, n->tx_successes, same_subnet);
                if (m < best_metric) {
                    best_metric = m;
                    best_next   = n->node_id;
                    best_hops   = 1;
                    best_rssi   = n->rssi;
                    improved    = true;
                }
                continue;
            }
        }

        if (improved) {
            /* Demote old primary to backup */
            r->backup_valid     = true;
            r->backup_next_hop  = r->next_hop;
            r->backup_hop_count = r->hop_count;
            r->backup_rssi      = r->rssi;

            r->next_hop  = best_next;
            r->hop_count = best_hops;
            r->rssi      = best_rssi;
            r->metric    = best_metric;
            r->last_seen_ms = now_ms;
            s_routing.route_switches++;
            optimizations++;

            ROUTE_LOG(ESP_LOG_INFO, "Optimized route: 0x%08X → 0x%08X (metric=%u)",
                      r->dest, r->next_hop, r->metric);

            if (g_mesh.config.callbacks.on_route_changed) {
                g_mesh.config.callbacks.on_route_changed(r->dest, r->next_hop, r->hop_count);
            }
        }
    }

    /* Re-evaluate gateway after optimization */
    route_reevaluate_gateway();

    if (optimizations > 0) {
        ROUTE_LOG(ESP_LOG_DEBUG, "Route optimization: %u improved", optimizations);
    }
}

/*============================================================================
 *  Route / Neighbor aging
 *============================================================================*/

void mesh_routing_process(uint32_t now_ms) {
    /* Age neighbors */
    for (uint16_t i = 0; i < s_routing.neighbor_max; i++) {
        neighbor_entry_t *n = &s_routing.neighbors[i];
        if (!n->used) continue;

        if (now_ms - n->last_seen_ms > g_mesh.config.neighbor_timeout_ms) {
            uint32_t lost = n->node_id;
            ROUTE_LOG(ESP_LOG_DEBUG, "Neighbor 0x%08X timed out", lost);
            n->used = false;

            if (g_mesh.config.callbacks.on_node_lost) {
                g_mesh.config.callbacks.on_node_lost(lost);
            }

            /* Repair routes through this neighbor */
            for (uint16_t j = 0; j < s_routing.route_max; j++) {
                route_entry_t *r = &s_routing.routes[j];
                if (!r->used) continue;

                if (r->next_hop == lost) {
                    if (r->backup_valid) {
                        r->next_hop      = r->backup_next_hop;
                        r->hop_count     = r->backup_hop_count;
                        r->rssi          = r->backup_rssi;
                        r->backup_valid  = false;
                        r->last_seen_ms  = now_ms;
                        s_routing.route_repairs++;
                        ROUTE_LOG(ESP_LOG_INFO, "Repaired route: 0x%08X via 0x%08X (backup)", r->dest, r->next_hop);
                    } else {
                        r->used = false;
                        ROUTE_LOG(ESP_LOG_INFO, "Route 0x%08X lost (no backup)", r->dest);
                    }
                }

                if (r->backup_valid && r->backup_next_hop == lost) {
                    r->backup_valid = false;
                }
            }

            if (s_routing.parent_id == lost) s_routing.parent_id = 0;
            if (s_routing.gateway_id == lost) s_routing.gateway_id = 0;
        }
    }

    /* Age routes */
    for (uint16_t i = 0; i < s_routing.route_max; i++) {
        route_entry_t *r = &s_routing.routes[i];
        if (!r->used) continue;

        if (now_ms - r->last_seen_ms > g_mesh.config.route_timeout_ms) {
            /* Try backup before expiring completely */
            if (r->backup_valid) {
                neighbor_entry_t *bn = neighbor_find(r->backup_next_hop);
                if (bn) {
                    ROUTE_LOG(ESP_LOG_DEBUG, "Route 0x%08X switching to backup (age)", r->dest);
                    r->next_hop      = r->backup_next_hop;
                    r->hop_count     = r->backup_hop_count;
                    r->rssi          = r->backup_rssi;
                    r->backup_valid  = false;
                    r->last_seen_ms  = now_ms;
                    s_routing.route_repairs++;
                    continue;
                }
            }
            ROUTE_LOG(ESP_LOG_DEBUG, "Route to 0x%08X expired", r->dest);
            r->used = false;
        }
    }

    /* Periodic optimization */
    route_optimize(now_ms);
}

/*============================================================================
 *  Link quality tracking (called from reliable layer)
 *============================================================================*/

void mesh_routing_record_tx_success(uint32_t neighbor_id) {
    neighbor_entry_t *n = neighbor_find(neighbor_id);
    if (n) {
        n->tx_attempts++;
        n->tx_successes++;
    }
}

void mesh_routing_record_tx_failure(uint32_t neighbor_id) {
    neighbor_entry_t *n = neighbor_find(neighbor_id);
    if (n) {
        n->tx_attempts++;
    }
}

void mesh_routing_update_neighbor_battery(uint32_t neighbor_id, uint32_t mv) {
    neighbor_entry_t *n = neighbor_find(neighbor_id);
    if (n) {
        n->battery_mv = mv;
    }
}

/*============================================================================
 *  Query functions
 *============================================================================*/

uint32_t mesh_routing_get_gateway(void) {
    return g_mesh.config.gateway_mode ? g_mesh.config.node_id : s_routing.gateway_id;
}

uint32_t mesh_routing_get_parent(void) {
    return g_mesh.config.gateway_mode ? g_mesh.config.node_id : s_routing.parent_id;
}

uint16_t mesh_routing_neighbor_count(void) {
    uint16_t count = 0;
    for (uint16_t i = 0; i < s_routing.neighbor_max; i++) {
        if (s_routing.neighbors[i].used) count++;
    }
    return count;
}

uint16_t mesh_routing_route_count(void) {
    uint16_t count = 0;
    for (uint16_t i = 0; i < s_routing.route_max; i++) {
        if (s_routing.routes[i].used) count++;
    }
    return count;
}

int8_t mesh_routing_avg_rssi(void) {
    int32_t sum = 0;
    uint16_t count = 0;
    for (uint16_t i = 0; i < s_routing.neighbor_max; i++) {
        if (s_routing.neighbors[i].used) {
            sum += s_routing.neighbors[i].rssi;
            count++;
        }
    }
    return count ? (int8_t)(sum / count) : 0;
}

uint8_t mesh_routing_avg_hops(void) {
    uint32_t sum = 0;
    uint16_t count = 0;
    for (uint16_t i = 0; i < s_routing.neighbor_max; i++) {
        if (s_routing.neighbors[i].used && s_routing.neighbors[i].hop_count != 0xFF) {
            sum += s_routing.neighbors[i].hop_count;
            count++;
        }
    }
    return count ? (uint8_t)(sum / count) : 0;
}

void mesh_routing_get_table(mesh_espnow_route_t *entries, uint16_t *count, uint16_t max) {
    uint16_t written = 0;
    for (uint16_t i = 0; i < s_routing.route_max && written < max; i++) {
        if (s_routing.routes[i].used) {
            entries[written].node_id      = s_routing.routes[i].dest;
            entries[written].next_hop     = s_routing.routes[i].next_hop;
            entries[written].hop_count    = s_routing.routes[i].hop_count;
            entries[written].rssi         = s_routing.routes[i].rssi;
            entries[written].last_seen_ms = s_routing.routes[i].last_seen_ms;
            written++;
        }
    }
    *count = written;
}

void mesh_routing_get_neighbors(mesh_espnow_neighbor_t *entries, uint16_t *count, uint16_t max) {
    uint16_t written = 0;
    for (uint16_t i = 0; i < s_routing.neighbor_max && written < max; i++) {
        if (s_routing.neighbors[i].used) {
            entries[written].node_id       = s_routing.neighbors[i].node_id;
            entries[written].rssi          = s_routing.neighbors[i].rssi;
            entries[written].rssi_min      = s_routing.neighbors[i].rssi_min;
            entries[written].rssi_max      = s_routing.neighbors[i].rssi_max;
            entries[written].last_seen_ms  = s_routing.neighbors[i].last_seen_ms;
            entries[written].hop_count     = s_routing.neighbors[i].hop_count;
            entries[written].capabilities  = s_routing.neighbors[i].capabilities;
            entries[written].uptime_s      = s_routing.neighbors[i].uptime_s;
            written++;
        }
    }
    *count = written;
}

uint32_t mesh_routing_get_route_repairs(void)  { return s_routing.route_repairs; }
uint32_t mesh_routing_get_route_switches(void) { return s_routing.route_switches; }

/*============================================================================
 *  Battery update for a neighbor (called from beacon handler)
 *============================================================================*/

void mesh_routing_handle_battery_info(uint32_t node_id, uint32_t mv) {
    mesh_routing_update_neighbor_battery(node_id, mv);

    /* Re-evaluate routes involving this neighbor */
    for (uint16_t i = 0; i < s_routing.route_max; i++) {
        route_entry_t *r = &s_routing.routes[i];
        if (!r->used) continue;
        if (r->next_hop == node_id || r->backup_next_hop == node_id) {
            neighbor_entry_t *n = neighbor_find(r->next_hop);
            uint8_t caps = n ? n->capabilities : 0;
            uint32_t tx_a = n ? n->tx_attempts : 0;
            uint32_t tx_s = n ? n->tx_successes : 0;
            bool same_subnet = (n && (g_mesh.config.subnet_id == 0 || n->subnet_id == g_mesh.config.subnet_id));
            r->metric = compute_metric(r->hop_count, r->rssi, caps, mv, tx_a, tx_s, same_subnet);
        }
    }

    route_reevaluate_gateway();
}
