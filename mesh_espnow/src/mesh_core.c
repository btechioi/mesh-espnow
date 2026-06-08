/* mesh_core.c — Core lifecycle, state machine, ESP-NOW integration */

#include "mesh_priv.h"

/*============================================================================
 *  Global state & logging
 *============================================================================*/

mesh_espnow_ctx_t g_mesh = {0};

mesh_espnow_log_level_t g_mesh_log     = MESH_ESPNOW_LOG_INFO;
mesh_espnow_log_level_t g_route_log    = MESH_ESPNOW_LOG_INFO;
mesh_espnow_log_level_t g_reliable_log = MESH_ESPNOW_LOG_INFO;
mesh_espnow_log_level_t g_power_log    = MESH_ESPNOW_LOG_INFO;
mesh_espnow_log_level_t g_sec_log      = MESH_ESPNOW_LOG_WARN;
mesh_espnow_log_level_t g_diag_log     = MESH_ESPNOW_LOG_INFO;

static const char *TAG = "mesh_core";

/*============================================================================
 *  Version
 *============================================================================*/

static const mesh_espnow_version_t s_version = {
    .major    = MESH_ESPNOW_VERSION_MAJOR,
    .minor    = MESH_ESPNOW_VERSION_MINOR,
    .patch    = MESH_ESPNOW_VERSION_PATCH,
    .build_time = __TIME__[0] == '?' ? 0 : 0, // set at build
    .git_sha  = "unknown",
    .idf_ver  = IDF_VER,
};

const mesh_espnow_version_t* mesh_espnow_get_version(void) { return &s_version; }

/*============================================================================
 *  Error strings
 *============================================================================*/

const char* mesh_espnow_err_to_str(esp_err_t err) {
    switch (err) {
        case ESP_OK:                         return "ESP_OK";
        case ESP_ERR_ESPNOW_NOT_INIT:        return "ESP-NOW not initialized";
        case ESP_ERR_ESPNOW_ARG:             return "ESP-NOW invalid argument";
        case ESP_ERR_ESPNOW_NO_MEM:          return "ESP-NOW out of memory";
        case ESP_ERR_ESPNOW_FULL:            return "ESP-NOW peer table full";
        case ESP_ERR_ESPNOW_NOT_FOUND:       return "ESP-NOW peer not found";
        case ESP_ERR_ESPNOW_INTERNAL:        return "ESP-NOW internal error";
        case ESP_ERR_ESPNOW_EXIST:           return "ESP-NOW peer already exists";
        case ESP_ERR_ESPNOW_IF:              return "ESP-NOW interface error";
        case MESH_ESPNOW_ERR_INVALID_STATE:  return "Invalid state for operation";
        case MESH_ESPNOW_ERR_NO_ROUTE:       return "No route to destination";
        case MESH_ESPNOW_ERR_RATE_LIMITED:   return "Operation rate-limited";
        case MESH_ESPNOW_ERR_NO_GATEWAY:     return "No gateway known";
        case MESH_ESPNOW_ERR_DUPLICATE:      return "Duplicate packet dropped";
        case MESH_ESPNOW_ERR_INVALID_PARAM:  return "Invalid parameter";
        case MESH_ESPNOW_ERR_PAYLOAD_TOO_BIG:return "Payload exceeds limit";
        case MESH_ESPNOW_ERR_NOT_INITIALIZED:return "Not initialized";
        case MESH_ESPNOW_ERR_ALREADY_INIT:   return "Already initialized";
        case MESH_ESPNOW_ERR_DECRYPT_FAILED: return "Decryption/MIC failed";
        case MESH_ESPNOW_ERR_CONFIG_INVALID: return "Configuration invalid";
        default:                             return "Unknown error";
    }
}

/*============================================================================
 *  Power mode string
 *============================================================================*/

const char* mesh_espnow_power_mode_str(mesh_espnow_power_mode_t mode) {
    switch (mode) {
        case MESH_ESPNOW_POWER_ALWAYS_ON:          return "always_on";
        case MESH_ESPNOW_POWER_DUTY_CYCLE:         return "duty_cycle";
        case MESH_ESPNOW_POWER_DEEP_SLEEP:         return "deep_sleep";
        case MESH_ESPNOW_POWER_DEEP_SLEEP_ON_DEMAND: return "deep_sleep_on_demand";
        default:                                   return "unknown";
    }
}

/*============================================================================
 *  State string
 *============================================================================*/

const char* mesh_espnow_state_str(mesh_espnow_state_t state) {
    switch (state) {
        case MESH_ESPNOW_STATE_UNINITIALIZED: return "uninitialized";
        case MESH_ESPNOW_STATE_INIT:          return "init";
        case MESH_ESPNOW_STATE_DISCOVERING:   return "discovering";
        case MESH_ESPNOW_STATE_CONNECTED:     return "connected";
        case MESH_ESPNOW_STATE_SLEEPING:      return "sleeping";
        case MESH_ESPNOW_STATE_ERROR:         return "error";
        default:                              return "unknown";
    }
}

/*============================================================================
 *  Mutex helpers
 *============================================================================*/

esp_err_t mesh_core_mutex_lock(void) {
    if (!g_mesh.mutex) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(g_mesh.mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t mesh_core_mutex_unlock(void) {
    if (!g_mesh.mutex) return ESP_ERR_INVALID_STATE;
    xSemaphoreGive(g_mesh.mutex);
    return ESP_OK;
}

/*============================================================================
 *  State helpers
 *============================================================================*/

bool mesh_core_is_initialized(void) { return g_mesh.state != MESH_ESPNOW_STATE_UNINITIALIZED; }
bool mesh_core_is_started(void)     { return g_mesh.started; }

void mesh_core_enter_error_state(esp_err_t err, const char *msg) {
    g_mesh.last_err = err;
    strncpy(g_mesh.last_err_msg, msg ? msg : "unknown", sizeof(g_mesh.last_err_msg) - 1);
    g_mesh.last_err_msg[sizeof(g_mesh.last_err_msg) - 1] = '\0';
    MESH_LOG(ESP_LOG_ERROR, TAG, "FATAL: %s (%s)", msg, mesh_espnow_err_to_str(err));
    g_mesh.state = MESH_ESPNOW_STATE_ERROR;
    if (g_mesh.config.callbacks.on_fatal_error) {
        g_mesh.config.callbacks.on_fatal_error(err, g_mesh.last_err_msg);
    }
}

void mesh_core_transition_to(mesh_espnow_state_t new_state) {
    mesh_espnow_state_t old = g_mesh.state;
    g_mesh.state = new_state;
    MESH_LOG(ESP_LOG_INFO, TAG, "State: %s → %s",
             mesh_espnow_state_str(old), mesh_espnow_state_str(new_state));
}

/*============================================================================
 *  Utility
 *============================================================================*/

int8_t mesh_core_rssi_to_quality(int8_t rssi) {
    if (rssi >= -50) return 100;
    if (rssi <= -100) return 0;
    return (int8_t)((rssi + 100) * 2);
}

bool mesh_core_is_gateway(void) {
    return g_mesh.config.gateway_mode;
}

bool mesh_core_addr_is_broadcast(const uint8_t *mac) {
    for (int i = 0; i < MESH_ESPNOW_ADDR_LEN; i++) {
        if (mac[i] != 0xFF) return false;
    }
    return true;
}

void mesh_core_mac_to_node_id(const uint8_t *mac, uint32_t *id) {
    *id = ((uint32_t)mac[0] << 24) | ((uint32_t)mac[1] << 16) |
          ((uint32_t)mac[2] << 8)  | mac[3];
}

void mesh_core_node_id_to_mac(uint32_t id, uint8_t *mac) {
    mac[0] = (id >> 24) & 0xFF;
    mac[1] = (id >> 16) & 0xFF;
    mac[2] = (id >> 8) & 0xFF;
    mac[3] = id & 0xFF;
    mac[4] = 0x00;
    mac[5] = 0x00;
}

/*============================================================================
 *  ESP-NOW send wrapper
 *============================================================================*/

esp_err_t mesh_core_espnow_send(const uint8_t *mac, const uint8_t *data, size_t len) {
    if (!g_mesh.espnow_inited) return ESP_ERR_ESPNOW_NOT_INIT;
    esp_err_t err = esp_now_send(mac, data, len);
    if (err != ESP_OK) {
        MESH_LOG(ESP_LOG_WARN, TAG, "esp_now_send failed: %s", mesh_espnow_err_to_str(err));
    }
    return err;
}

/*============================================================================
 *  Packet builder + sender
 *============================================================================*/

static esp_err_t send_packet_inner(uint32_t dest_id, uint32_t next_hop,
                                    const uint8_t *payload, uint16_t payload_len,
                                    uint8_t type, uint32_t ack_seqno, uint8_t flags) {
    uint8_t buf[MESH_PACKET_MAX];
    mesh_espnow_header_t *hdr = (mesh_espnow_header_t *)buf;

    if (payload_len > MESH_PAYLOAD_MAX) return MESH_ESPNOW_ERR_PAYLOAD_TOO_BIG;

    hdr->proto_ver     = MESH_PROTO_VER;
    hdr->type          = type;
    hdr->ttl           = g_mesh.config.ttl;
    hdr->src_id        = g_mesh.config.node_id;
    hdr->dest_id       = dest_id;
    hdr->seqno         = (uint32_t)(esp_random());
    hdr->ack_seqno     = ack_seqno;
    hdr->payload_len   = payload_len;
    hdr->flags         = flags;

    size_t total_len = payload_len;

    if (payload_len > 0) {
        memcpy(buf + MESH_HEADER_SIZE, payload, payload_len);
    }

    /* Apply encryption */
    if (g_mesh.config.encryption_enabled && payload_len > 0 &&
        (type == PKT_DATA || type == PKT_BROADCAST)) {
        hdr->flags |= MESH_FLAG_ENC;
        esp_err_t err = mesh_security_encrypt(buf + MESH_HEADER_SIZE, &total_len);
        if (err != ESP_OK) {
            MESH_LOG(ESP_LOG_WARN, TAG, "Encrypt failed: %s", mesh_espnow_err_to_str(err));
            return err;
        }
    }

    hdr->payload_len = total_len;
    size_t wire_len = MESH_HEADER_SIZE + total_len;

    /* Determine dest MAC (use next_hop if provided, else dest_id) */
    uint32_t target = (next_hop != 0) ? next_hop : dest_id;
    uint8_t dest_mac[MESH_ESPNOW_ADDR_LEN];
    if (target == 0xFFFFFFFF) {
        memset(dest_mac, 0xFF, MESH_ESPNOW_ADDR_LEN);
    } else {
        mesh_core_node_id_to_mac(target, dest_mac);
    }

    esp_err_t err = mesh_core_espnow_send(dest_mac, buf, wire_len);
    if (err == ESP_OK) {
        g_mesh.stats.tx_packets++;
        g_mesh.stats.tx_bytes += payload_len;
    }
    return err;
}

esp_err_t mesh_core_send_packet(uint32_t dest_id, const uint8_t *payload,
                                uint16_t payload_len, uint8_t type,
                                uint32_t ack_seqno, uint8_t flags) {
    return send_packet_inner(dest_id, 0, payload, payload_len, type, ack_seqno, flags);
}

esp_err_t mesh_core_send_packet_to(uint32_t dest_id, uint32_t next_hop,
                                    const uint8_t *payload, uint16_t payload_len,
                                    uint8_t type, uint32_t ack_seqno, uint8_t flags) {
    return send_packet_inner(dest_id, next_hop, payload, payload_len, type, ack_seqno, flags);
}

/*============================================================================
 *  ESP-NOW receive callback
 *============================================================================*/

static void espnow_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
    if (!recv_info || !data || len < MESH_HEADER_SIZE) return;
    mesh_core_handle_data(recv_info->src_addr, data, len);
}

static int8_t get_rssi(const uint8_t *mac) {
    int8_t rssi = 0;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    esp_now_get_rssi(mac, &rssi);
#else
    (void)mac;
#endif
    return rssi;
}

void mesh_core_handle_data(const uint8_t *mac, const uint8_t *data, int len) {
    const mesh_espnow_header_t *hdr = (const mesh_espnow_header_t *)data;
    int8_t rssi = get_rssi(mac);

    /* Validate protocol version */
    if (hdr->proto_ver != MESH_PROTO_VER) {
        MESH_LOG(ESP_LOG_WARN, TAG, "Proto version mismatch: %d", hdr->proto_ver);
        return;
    }

    /* Update neighbor table */
    mesh_routing_add_neighbor(hdr->src_id, 0xFF, 0, rssi, hdr->subnet_id, 0);

    uint16_t plen = hdr->payload_len;
    if (plen > MESH_PAYLOAD_MAX) plen = MESH_PAYLOAD_MAX;

    switch (hdr->type) {
        case PKT_BEACON: {
            /* Beacon: caps(1) + hops(1) + gateway(4) + uptime(4) + battery(4) = 14 bytes */
            if (plen >= 14) {
                const uint8_t *p = data + MESH_HEADER_SIZE;
                uint8_t  caps    = p[0];
                uint8_t  hops    = p[1];
                uint32_t gateway = ((uint32_t)p[2] << 24) | ((uint32_t)p[3] << 16) |
                                   ((uint32_t)p[4] << 8)  | p[5];
                uint32_t uptime  = ((uint32_t)p[6] << 24) | ((uint32_t)p[7] << 16) |
                                   ((uint32_t)p[8] << 8)  | p[9];
                uint32_t battery = ((uint32_t)p[10] << 24) | ((uint32_t)p[11] << 16) |
                                   ((uint32_t)p[12] << 8)  | p[13];
                mesh_routing_handle_beacon(hdr->src_id, caps, hops, gateway, uptime, rssi,
                                           hdr->subnet_id, g_mesh.config.subnet_channel);
                if (battery > 0) {
                    mesh_routing_handle_battery_info(hdr->src_id, battery);
                }
            }
            break;
        }

        case PKT_DATA: {
            /* Decrypt */
            uint8_t decrypted[MESH_ESPNOW_MAX_PAYLOAD_LEN + MESH_MIC_TAG_LEN];
            const uint8_t *payload = data + MESH_HEADER_SIZE;
            size_t dlen = plen;

            if (hdr->flags & MESH_FLAG_ENC && g_mesh.config.encryption_enabled) {
                memcpy(decrypted, payload, plen);
                if (mesh_security_decrypt(decrypted, &dlen) != ESP_OK) {
                    MESH_LOG(ESP_LOG_WARN, TAG, "Decrypt failed from 0x%08X", hdr->src_id);
                    return;
                }
                payload = decrypted;
            }

            /* Forward if not for us — use next hop from routing table */
            if (hdr->dest_id != g_mesh.config.node_id && hdr->dest_id != 0xFFFFFFFF) {
                if (hdr->ttl > 1) {
                    uint32_t next_hop = 0;
                    uint8_t  nh_hops = 0;
                    int8_t   nh_rssi = 0;
                    if (mesh_routing_find_route(hdr->dest_id, &next_hop, &nh_hops, &nh_rssi)) {
                        mesh_core_send_packet_to(hdr->dest_id, next_hop, payload,
                                                  (uint16_t)dlen, PKT_DATA, 0, hdr->flags);
                    } else {
                        /* No route — one-hop gamble to final dest */
                        mesh_core_send_packet(hdr->dest_id, payload, (uint16_t)dlen,
                                               PKT_DATA, 0, hdr->flags);
                    }
                    g_mesh.stats.forwarded++;
                }
                return;
            }

            g_mesh.stats.rx_packets++;
            g_mesh.stats.rx_bytes += dlen;

            /* Send ACK */
            mesh_core_send_packet(hdr->src_id, NULL, 0, PKT_DATA_ACK, hdr->seqno, 0);

            /* Deliver to application */
            if (g_mesh.config.callbacks.on_data) {
                g_mesh.config.callbacks.on_data(hdr->src_id, payload, (uint16_t)dlen, rssi);
            }
            break;
        }

        case PKT_DATA_ACK:
            mesh_reliable_handle_ack(hdr->src_id, hdr->ack_seqno);
            g_mesh.stats.ack_received++;
            break;

        case PKT_RREQ:
            if (hdr->flags & MESH_FLAG_RREQ) {
                /* Payload: dest(4) + metric(2) */
                uint32_t rreq_dest = 0;
                if (plen >= 4) {
                    const uint8_t *p = data + MESH_HEADER_SIZE;
                    rreq_dest = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                                ((uint32_t)p[2] << 8)  | p[3];
                }
                mesh_routing_handle_rreq(hdr->src_id, rreq_dest, hdr->seqno, hdr->ttl, rssi);
            }
            g_mesh.stats.rreqs_received++;
            break;

        case PKT_RREP:
            if (hdr->flags & MESH_FLAG_RREP) {
                /* Payload: dest(4) + hop_count(1) + rssi(1) */
                uint32_t rrep_dest = 0;
                uint8_t  rrep_hops = 1;
                if (plen >= 4) {
                    const uint8_t *p = data + MESH_HEADER_SIZE;
                    rrep_dest = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                                ((uint32_t)p[2] << 8)  | p[3];
                }
                if (plen >= 6) {
                    rrep_hops = data[MESH_HEADER_SIZE + 4];
                }
                mesh_routing_handle_rrep(hdr->src_id, rrep_dest, hdr->seqno, rrep_hops, rssi);
            }
            g_mesh.stats.rreps_received++;
            break;

        case PKT_BROADCAST: {
            /* Decrypt */
            const uint8_t *payload = data + MESH_HEADER_SIZE;
            uint8_t decrypted[MESH_ESPNOW_MAX_PAYLOAD_LEN + MESH_MIC_TAG_LEN];
            size_t dlen = plen;
            if (hdr->flags & MESH_FLAG_ENC && g_mesh.config.encryption_enabled) {
                memcpy(decrypted, payload, plen);
                if (mesh_security_decrypt(decrypted, &dlen) != ESP_OK) return;
                payload = decrypted;
            }

            /* Re-broadcast with decremented TTL */
            if (hdr->ttl > 1) {
                mesh_core_send_packet(0xFFFFFFFF, payload, dlen, PKT_BROADCAST, 0, hdr->flags);
                g_mesh.stats.forwarded++;
            }

            g_mesh.stats.rx_packets++;
            if (g_mesh.config.callbacks.on_broadcast) {
                g_mesh.config.callbacks.on_broadcast(hdr->src_id, payload, (uint16_t)dlen);
            }
            break;
        }

        case PKT_GOODBYE:
            mesh_routing_remove_neighbor(hdr->src_id);
            if (g_mesh.config.callbacks.on_node_lost) {
                g_mesh.config.callbacks.on_node_lost(hdr->src_id);
            }
            break;

        default:
            MESH_LOG(ESP_LOG_WARN, TAG, "Unknown packet type: 0x%02X", hdr->type);
            break;
    }
}

/*============================================================================
 *  ESP-NOW send callback
 *============================================================================*/

static void espnow_send_cb(const uint8_t *mac, esp_now_send_status_t status) {
    /* Can be extended for delivery feedback */
    (void)mac;
    if (status != ESP_NOW_SEND_SUCCESS) {
        g_mesh.stats.tx_packets--; /* wasn't actually sent */
    }
}

/*============================================================================
 *  Config validation
 *============================================================================*/

esp_err_t mesh_espnow_validate_config(const mesh_espnow_config_t *cfg, const char **err) {
    static const char *ok = NULL;

    if (!cfg) {
        if (err) *err = "Config is NULL";
        return MESH_ESPNOW_ERR_INVALID_PARAM;
    }

    if (cfg->channel < 1 || cfg->channel > 11) {
        if (err) *err = "Channel must be 1-11";
        return MESH_ESPNOW_ERR_CONFIG_INVALID;
    }
    if (cfg->beacon_interval_ms < 100 || cfg->beacon_interval_ms > 60000) {
        if (err) *err = "beacon_interval_ms must be 100-60000";
        return MESH_ESPNOW_ERR_CONFIG_INVALID;
    }
    if (cfg->neighbor_timeout_ms < 5000 || cfg->neighbor_timeout_ms > 300000) {
        if (err) *err = "neighbor_timeout_ms must be 5000-300000";
        return MESH_ESPNOW_ERR_CONFIG_INVALID;
    }
    if (cfg->route_timeout_ms < 10000 || cfg->route_timeout_ms > 600000) {
        if (err) *err = "route_timeout_ms must be 10000-600000";
        return MESH_ESPNOW_ERR_CONFIG_INVALID;
    }
    if (cfg->retransmit_timeout_ms < 100 || cfg->retransmit_timeout_ms > 10000) {
        if (err) *err = "retransmit_timeout_ms must be 100-10000";
        return MESH_ESPNOW_ERR_CONFIG_INVALID;
    }
    if (cfg->deep_sleep_interval_ms < 100 || cfg->deep_sleep_interval_ms > 600000) {
        if (err) *err = "deep_sleep_interval_ms must be 100-600000";
        return MESH_ESPNOW_ERR_CONFIG_INVALID;
    }
    if (cfg->max_retransmits > 10) {
        if (err) *err = "max_retransmits must be 0-10";
        return MESH_ESPNOW_ERR_CONFIG_INVALID;
    }
    if (cfg->ttl < 1 || cfg->ttl > 64) {
        if (err) *err = "ttl must be 1-64";
        return MESH_ESPNOW_ERR_CONFIG_INVALID;
    }
    if (cfg->max_neighbors < 4 || cfg->max_neighbors > 128) {
        if (err) *err = "max_neighbors must be 4-128";
        return MESH_ESPNOW_ERR_CONFIG_INVALID;
    }
    if (cfg->max_routes < 8 || cfg->max_routes > 256) {
        if (err) *err = "max_routes must be 8-256";
        return MESH_ESPNOW_ERR_CONFIG_INVALID;
    }
    if (err) *err = ok;
    return ESP_OK;
}

/*============================================================================
 *  Log level control
 *============================================================================*/

void mesh_espnow_set_log_level(const char *subsystem, mesh_espnow_log_level_t level) {
    if (!subsystem) return;
    if (strcmp(subsystem, "mesh") == 0)     g_mesh_log     = level;
    if (strcmp(subsystem, "routing") == 0)  g_route_log    = level;
    if (strcmp(subsystem, "reliable") == 0) g_reliable_log = level;
    if (strcmp(subsystem, "power") == 0)    g_power_log    = level;
    if (strcmp(subsystem, "security") == 0) g_sec_log      = level;
    if (strcmp(subsystem, "diag") == 0)     g_diag_log     = level;
}

/*============================================================================
 *  init
 *============================================================================*/

esp_err_t mesh_espnow_init(const mesh_espnow_config_t *cfg) {
    esp_err_t err;

    if (g_mesh.state != MESH_ESPNOW_STATE_UNINITIALIZED) {
        return MESH_ESPNOW_ERR_ALREADY_INIT;
    }

    /* Validate config if provided */
    if (cfg) {
        const char *verr = NULL;
        err = mesh_espnow_validate_config(cfg, &verr);
        if (err != ESP_OK) {
            MESH_LOG(ESP_LOG_ERROR, TAG, "Config invalid: %s", verr);
            return err;
        }
    }

    /* Initialize global context */
    memset(&g_mesh, 0, sizeof(g_mesh));

    /* Create mutex */
    g_mesh.mutex = xSemaphoreCreateMutex();
    if (!g_mesh.mutex) {
        MESH_LOG(ESP_LOG_ERROR, TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Copy config */
    if (cfg) {
        memcpy(&g_mesh.config, cfg, sizeof(mesh_espnow_config_t));
    } else {
        mesh_espnow_config_t def = MESH_ESPNOW_CONFIG_DEFAULT();
        memcpy(&g_mesh.config, &def, sizeof(mesh_espnow_config_t));
    }

    /* Get MAC */
    esp_err_t mac_err = esp_read_mac(g_mesh.mac, ESP_MAC_WIFI_STA);
    if (mac_err != ESP_OK) {
        vSemaphoreDelete(g_mesh.mutex);
        g_mesh.mutex = NULL;
        return mac_err;
    }

    /* Auto-generate node ID if 0 */
    if (g_mesh.config.node_id == 0) {
        mesh_core_mac_to_node_id(g_mesh.mac, &g_mesh.config.node_id);
    }

    /* Init NVS */
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        vSemaphoreDelete(g_mesh.mutex);
        g_mesh.mutex = NULL;
        return err;
    }

    err = nvs_open(MESH_NVS_NAMESPACE, NVS_READWRITE, &g_mesh.nvs_handle);
    if (err == ESP_OK) g_mesh.nvs_opened = true;

    /* If gateway mode, add self as gateway capability */
    if (g_mesh.config.gateway_mode) {
        g_mesh.config.capabilities |= MESH_ESPNOW_CAP_GATEWAY;
    }

    /* Init Wi-Fi */
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_cfg);
    if (err != ESP_OK) {
        MESH_LOG(ESP_LOG_ERROR, TAG, "WiFi init failed: %s", mesh_espnow_err_to_str(err));
        goto fail;
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) goto fail;

    err = esp_wifi_start();
    if (err != ESP_OK) goto fail;

    err = esp_wifi_set_channel(g_mesh.config.channel, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) goto fail;

    g_mesh.wifi_inited = true;

    /* Init ESP-NOW */
    err = esp_now_init();
    if (err != ESP_OK) {
        MESH_LOG(ESP_LOG_ERROR, TAG, "ESP-NOW init failed: %s", mesh_espnow_err_to_str(err));
        goto fail;
    }

    err = esp_now_register_recv_cb(espnow_recv_cb);
    if (err != ESP_OK) goto fail;

    err = esp_now_register_send_cb(espnow_send_cb);
    if (err != ESP_OK) goto fail;

    err = esp_now_set_pmk((esp_now_pmk_t *)g_mesh.config.pre_shared_key);
    if (err != ESP_OK) {
        MESH_LOG(ESP_LOG_WARN, TAG, "PMK set failed: %s", mesh_espnow_err_to_str(err));
    }

    g_mesh.espnow_inited = true;

    /* If we woke from ESP-NOW deep sleep, retrieve and process the wakeup packet */
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 4, 0)
    {
        uint8_t wakeup_data[256];
        esp_now_wakeup_packet_t wakeup_pkt = {
            .data = wakeup_data,
            .len = sizeof(wakeup_data),
        };
        if (esp_now_get_wakeup_packet(&wakeup_pkt) == ESP_OK && wakeup_pkt.len > 0) {
            MESH_LOG(ESP_LOG_INFO, TAG, "Woke from ESP-NOW, processing wakeup packet (%d bytes)",
                     wakeup_pkt.len);
            mesh_core_handle_data(wakeup_pkt.src_addr, wakeup_pkt.data, wakeup_pkt.len);
        }
    }
#endif

    /* Init subsystems */
    err = mesh_routing_init();
    if (err != ESP_OK) goto fail;

    err = mesh_reliable_init();
    if (err != ESP_OK) goto fail;

    err = mesh_power_init();
    if (err != ESP_OK) goto fail;

    err = mesh_security_init(g_mesh.config.pre_shared_key, MESH_ESPNOW_PSK_LEN);
    if (err != ESP_OK) goto fail;

    err = mesh_diag_init();
    if (err != ESP_OK) goto fail;

    g_mesh.start_time_us = esp_timer_get_time();
    mesh_core_transition_to(MESH_ESPNOW_STATE_INIT);

    MESH_LOG(ESP_LOG_INFO, TAG, "Mesh node 0x%08X on ch %d [%s]",
             g_mesh.config.node_id, g_mesh.config.channel,
             g_mesh.config.gateway_mode ? "GATEWAY" : "NODE");

    return ESP_OK;

fail:
    mesh_espnow_deinit();
    return err;
}

/*============================================================================
 *  start
 *============================================================================*/

esp_err_t mesh_espnow_start(void) {
    if (g_mesh.state == MESH_ESPNOW_STATE_UNINITIALIZED) {
        return MESH_ESPNOW_ERR_INVALID_STATE;
    }
    if (g_mesh.started) {
        return ESP_OK;
    }

    g_mesh.started = true;
    g_mesh.last_beacon_ms = 0;
    g_mesh.last_process_ms = (uint32_t)(esp_timer_get_time() / 1000);
    mesh_core_transition_to(MESH_ESPNOW_STATE_DISCOVERING);

    MESH_LOG(ESP_LOG_INFO, TAG, "Node started — discovering network...");
    return ESP_OK;
}

/*============================================================================
 *  stop
 *============================================================================*/

void mesh_espnow_stop(void) {
    if (!g_mesh.started) return;

    /* Send goodbye beacon */
    uint8_t goodbye[1] = {0};
    mesh_core_send_packet(0xFFFFFFFF, goodbye, 1, PKT_GOODBYE, 0, 0);

    g_mesh.started = false;
    mesh_core_transition_to(MESH_ESPNOW_STATE_INIT);
    MESH_LOG(ESP_LOG_INFO, TAG, "Node stopped");
}

/*============================================================================
 *  deinit
 *============================================================================*/

void mesh_espnow_deinit(void) {
    mesh_espnow_stop();

    mesh_diag_deinit();
    mesh_security_deinit();
    mesh_power_deinit();
    mesh_reliable_deinit();
    mesh_routing_deinit();

    if (g_mesh.espnow_inited) {
        esp_now_unregister_recv_cb();
        esp_now_unregister_send_cb();
        esp_now_deinit();
        g_mesh.espnow_inited = false;
    }

    if (g_mesh.wifi_inited) {
        esp_wifi_stop();
        esp_wifi_deinit();
        g_mesh.wifi_inited = false;
    }

    if (g_mesh.nvs_opened) {
        nvs_close(g_mesh.nvs_handle);
        g_mesh.nvs_opened = false;
    }

    if (g_mesh.mutex) {
        vSemaphoreDelete(g_mesh.mutex);
        g_mesh.mutex = NULL;
    }

    mesh_core_transition_to(MESH_ESPNOW_STATE_UNINITIALIZED);
    g_mesh.started = false;
}

/*============================================================================
 *  factory_reset
 *============================================================================*/

esp_err_t mesh_espnow_factory_reset(void) {
    esp_err_t err;

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    /* Erase mesh namespace */
    nvs_handle_t h;
    err = nvs_open(MESH_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }

    /* Erase all NVS */
    nvs_flash_erase();

    MESH_LOG(ESP_LOG_INFO, TAG, "Factory reset — rebooting");
    esp_restart();
    return ESP_OK; /* never reached */
}

/*============================================================================
 *  Send API implementations
 *============================================================================*/

esp_err_t mesh_espnow_send(uint32_t dest_id, const uint8_t *data, uint16_t len,
                           mesh_espnow_tx_diag_t *diag) {
    if (!data && len > 0) return MESH_ESPNOW_ERR_INVALID_PARAM;
    if (len > MESH_PAYLOAD_MAX) return MESH_ESPNOW_ERR_PAYLOAD_TOO_BIG;
    if (!g_mesh.started) return MESH_ESPNOW_ERR_INVALID_STATE;
    if (dest_id == 0 || dest_id == 0xFFFFFFFF) return MESH_ESPNOW_ERR_INVALID_PARAM;

    return mesh_reliable_send(dest_id, (const uint16_t *)data, len);
}

esp_err_t mesh_espnow_broadcast(const uint8_t *data, uint16_t len) {
    if (!data && len > 0) return MESH_ESPNOW_ERR_INVALID_PARAM;
    if (len > MESH_PAYLOAD_MAX) return MESH_ESPNOW_ERR_PAYLOAD_TOO_BIG;
    if (!g_mesh.started) return MESH_ESPNOW_ERR_INVALID_STATE;

    return mesh_core_send_packet(0xFFFFFFFF, data, len, PKT_BROADCAST, 0, 0);
}

esp_err_t mesh_espnow_send_to_gateway(const uint8_t *data, uint16_t len) {
    if (!data && len > 0) return MESH_ESPNOW_ERR_INVALID_PARAM;
    if (len > MESH_PAYLOAD_MAX) return MESH_ESPNOW_ERR_PAYLOAD_TOO_BIG;
    if (!g_mesh.started) return MESH_ESPNOW_ERR_INVALID_STATE;

    uint32_t gw = mesh_routing_get_gateway();
    if (gw == 0) return MESH_ESPNOW_ERR_NO_GATEWAY;

    return mesh_reliable_send(gw, (const uint16_t *)data, len);
}

esp_err_t mesh_espnow_discover_route(uint32_t dest_id) {
    if (!g_mesh.started) return MESH_ESPNOW_ERR_INVALID_STATE;
    return mesh_routing_discover_route(dest_id);
}

/*============================================================================
 *  Power API
 *============================================================================*/

esp_err_t mesh_espnow_sleep(void) {
    if (g_mesh.state == MESH_ESPNOW_STATE_SLEEPING) return ESP_OK;

    mesh_core_transition_to(MESH_ESPNOW_STATE_SLEEPING);

    /* Send goodbye */
    uint8_t bye[1] = {0};
    mesh_core_send_packet(0xFFFFFFFF, bye, 1, PKT_GOODBYE, 0, 0);

    /* Small delay for TX to complete */
    vTaskDelay(pdMS_TO_TICKS(10));

    if (g_mesh.config.power_mode == MESH_ESPNOW_POWER_DEEP_SLEEP_ON_DEMAND) {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 4, 0)
        MESH_LOG(ESP_LOG_INFO, TAG, "Entering deep sleep on demand — wake on ESP-NOW packet");
        esp_sleep_enable_espnow_wakeup();
#else
        MESH_LOG(ESP_LOG_WARN, TAG, "ON_DEMAND requires IDF >= 4.4, falling back to timer wakeup");
        esp_sleep_enable_timer_wakeup(g_mesh.config.deep_sleep_interval_ms * 1000);
#endif
    } else {
        MESH_LOG(ESP_LOG_INFO, TAG, "Entering deep sleep for %u ms",
                 g_mesh.config.deep_sleep_interval_ms);
        esp_sleep_enable_timer_wakeup(g_mesh.config.deep_sleep_interval_ms * 1000);
    }

    esp_deep_sleep_start();

    return ESP_OK; /* never reached */
}

void mesh_espnow_update_battery(uint32_t millivolts) {
    if (millivolts > 5000) millivolts = 5000;
    g_mesh.battery_mv = millivolts;
    mesh_power_update_battery(millivolts);
}

uint32_t mesh_espnow_estimate_life_s(uint32_t battery_capacity_mah) {
    if (battery_capacity_mah == 0) return 0;

    /* Current estimates per power mode */
    uint32_t current_ua = 15000; /* ALWAYS_ON ~15mA */
    if (g_mesh.config.power_mode == MESH_ESPNOW_POWER_DUTY_CYCLE) {
        uint32_t cycle = g_mesh.config.beacon_interval_ms;
        uint32_t awake = g_mesh.config.awake_window_ms;
        if (cycle > 0) {
            current_ua = (uint32_t)(((uint64_t)15000 * awake + (uint64_t)25 * (cycle - awake)) / cycle);
            if (current_ua < 26) current_ua = 26;
        }
    } else if (g_mesh.config.power_mode == MESH_ESPNOW_POWER_DEEP_SLEEP ||
               g_mesh.config.power_mode == MESH_ESPNOW_POWER_DEEP_SLEEP_ON_DEMAND) {
        current_ua = 14; /* ~14uA deep sleep + occasional wake */
    }

    uint64_t life_us = (uint64_t)battery_capacity_mah * 1000 * 3600 * 1000 / current_ua;
    return (uint32_t)(life_us / 1000000);
}

/*============================================================================
 *  Info API
 *============================================================================*/

uint32_t mesh_espnow_get_node_id(void) {
    return g_mesh.config.node_id;
}

mesh_espnow_state_t mesh_espnow_get_state(void) {
    return g_mesh.state;
}

esp_err_t mesh_espnow_get_stats(mesh_espnow_stats_t *stats) {
    if (!stats) return MESH_ESPNOW_ERR_INVALID_PARAM;

    g_mesh.stats.uptime_ms = (uint32_t)((esp_timer_get_time() - g_mesh.start_time_us) / 1000);
    g_mesh.stats.neighbor_count = mesh_routing_neighbor_count();
    g_mesh.stats.route_count    = mesh_routing_route_count();
    g_mesh.stats.avg_rssi       = mesh_routing_avg_rssi();
    g_mesh.stats.avg_hop_count  = mesh_routing_avg_hops();
    g_mesh.stats.gateway_id     = mesh_routing_get_gateway();
    g_mesh.stats.parent_id      = mesh_routing_get_parent();
    g_mesh.stats.battery_mv     = g_mesh.battery_mv;
    g_mesh.stats.heap_free      = (uint32_t)esp_get_free_heap_size();

    memcpy(stats, &g_mesh.stats, sizeof(mesh_espnow_stats_t));
    return ESP_OK;
}

uint16_t mesh_espnow_get_routes(mesh_espnow_route_t *entries, uint16_t max) {
    if (!entries || max == 0) return 0;
    uint16_t count = 0;
    mesh_routing_get_table(entries, &count, max);
    return count;
}

uint16_t mesh_espnow_get_neighbors(mesh_espnow_neighbor_t *entries, uint16_t max) {
    if (!entries || max == 0) return 0;
    uint16_t count = 0;
    mesh_routing_get_neighbors(entries, &count, max);
    return count;
}

uint32_t mesh_espnow_get_parent(void) {
    return mesh_routing_get_parent();
}

uint32_t mesh_espnow_get_gateway(void) {
    return mesh_routing_get_gateway();
}

bool mesh_espnow_is_healthy(void) {
    if (g_mesh.state != MESH_ESPNOW_STATE_CONNECTED) return false;
    if (g_mesh.state == MESH_ESPNOW_STATE_ERROR) return false;
    if (mesh_routing_neighbor_count() == 0) return false;
    if (!mesh_core_is_gateway() && mesh_routing_get_gateway() == 0) return false;
    return true;
}

const char* mesh_espnow_last_error(void) {
    return g_mesh.last_err_msg;
}

void mesh_espnow_reset_stats(void) {
    memset(&g_mesh.stats, 0, sizeof(g_mesh.stats));
    g_mesh.stats_dirty = true;
}

/*============================================================================
 *  Process (main loop tick)
 *============================================================================*/

void mesh_espnow_process(uint32_t now_ms) {
    if (!g_mesh.started) return;

    /* Send periodic beacons */
    if (now_ms - g_mesh.last_beacon_ms >= g_mesh.config.beacon_interval_ms) {
        g_mesh.last_beacon_ms = now_ms;

        uint8_t beacon_data[14];
        uint32_t uptime_s = (uint32_t)((esp_timer_get_time() - g_mesh.start_time_us) / 1000000);

        beacon_data[0] = g_mesh.config.capabilities;
        beacon_data[1] = mesh_routing_avg_hops();
        /* Gateway ID */
        uint32_t gw = mesh_routing_get_gateway();
        beacon_data[2] = (gw >> 24) & 0xFF;
        beacon_data[3] = (gw >> 16) & 0xFF;
        beacon_data[4] = (gw >> 8) & 0xFF;
        beacon_data[5] = gw & 0xFF;
        /* Uptime */
        beacon_data[6] = (uptime_s >> 24) & 0xFF;
        beacon_data[7] = (uptime_s >> 16) & 0xFF;
        beacon_data[8] = (uptime_s >> 8) & 0xFF;
        beacon_data[9] = uptime_s & 0xFF;
        /* Battery info */
        beacon_data[10] = (g_mesh.battery_mv >> 24) & 0xFF;
        beacon_data[11] = (g_mesh.battery_mv >> 16) & 0xFF;
        beacon_data[12] = (g_mesh.battery_mv >> 8) & 0xFF;
        beacon_data[13] = g_mesh.battery_mv & 0xFF;

        mesh_core_send_packet(0xFFFFFFFF, beacon_data, sizeof(beacon_data), PKT_BEACON, 0, 0);
    }

    /* Subsystem processing */
    mesh_routing_process(now_ms);
    mesh_reliable_process(now_ms);
    mesh_power_process(now_ms);
    mesh_diag_process(now_ms);

    /* Check if we've joined the network */
    if (g_mesh.state == MESH_ESPNOW_STATE_DISCOVERING) {
        uint32_t gw = mesh_routing_get_gateway();
        if (gw != 0 || mesh_core_is_gateway()) {
            mesh_core_transition_to(MESH_ESPNOW_STATE_CONNECTED);
            if (g_mesh.config.callbacks.on_network_joined) {
                g_mesh.config.callbacks.on_network_joined(gw);
            }
        }
    }

    /* Check if we lost the network */
    if (g_mesh.state == MESH_ESPNOW_STATE_CONNECTED) {
        if (!mesh_core_is_gateway() && mesh_routing_get_gateway() == 0) {
            mesh_core_transition_to(MESH_ESPNOW_STATE_DISCOVERING);
            if (g_mesh.config.callbacks.on_network_lost) {
                g_mesh.config.callbacks.on_network_lost();
            }
        }
    }
}

void mesh_espnow_process_from_isr(void) {
    /* Minimal ISR processing — just a placeholder for now.
     * The main data flow is handled in the ESP-NOW receive callback directly.
     */
}

void mesh_espnow_diagnostic_scan(void) {
    mesh_diag_scan();
}
