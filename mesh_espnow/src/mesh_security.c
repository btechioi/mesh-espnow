/* mesh_security.c: AES-128-CCM encryption for ESP-NOW mesh */

#include "mesh_priv.h"
#include "mbedtls/aes.h"
#include "mbedtls/ccm.h"

static const char *TAG = "mesh_sec";

#define MIC_TAG_LEN 8

static struct {
    mbedtls_ccm_context ctx;
    bool initialized;
} s_sec;

esp_err_t mesh_security_init(const uint8_t *key, size_t key_len) {
    if (!key || key_len != 16) return MESH_ESPNOW_ERR_INVALID_PARAM;

    mbedtls_ccm_init(&s_sec.ctx);

    int ret = mbedtls_ccm_setkey(&s_sec.ctx, MBEDTLS_CIPHER_ID_AES, key, key_len * 8);
    if (ret != 0) {
        SEC_LOG(ESP_LOG_ERROR, "CCM setkey failed: %d", ret);
        mbedtls_ccm_free(&s_sec.ctx);
        return ESP_FAIL;
    }

    s_sec.initialized = true;
    SEC_LOG(ESP_LOG_INFO, "AES-128-CCM initialized");
    return ESP_OK;
}

void mesh_security_deinit(void) {
    if (s_sec.initialized) {
        mbedtls_ccm_free(&s_sec.ctx);
        s_sec.initialized = false;
    }
}

/**
 * @brief Encrypt plaintext in-place.
 *
 * Input:  data[0..*len-1] = plaintext
 * Output: data[0..*len+MIC_TAG_LEN-1] = ciphertext || MIC tag
 *         *len is incremented by MIC_TAG_LEN.
 *
 * The caller MUST ensure the buffer has at least *len + MIC_TAG_LEN bytes.
 */
esp_err_t mesh_security_encrypt(uint8_t *data, size_t *len) {
    if (!s_sec.initialized) return ESP_ERR_INVALID_STATE;
    if (!data || !len || *len > MESH_PAYLOAD_MAX) return MESH_ESPNOW_ERR_INVALID_PARAM;
    if (*len + MIC_TAG_LEN > MESH_PACKET_MAX - MESH_HEADER_SIZE) return MESH_ESPNOW_ERR_PAYLOAD_TOO_BIG;

    /* Build nonce: 4 bytes node_id (truncated) + 9 bytes random = 13 */
    uint8_t nonce[13];
    nonce[0] = (g_mesh.config.node_id >> 24) & 0xFF;
    nonce[1] = (g_mesh.config.node_id >> 16) & 0xFF;
    nonce[2] = (g_mesh.config.node_id >> 8) & 0xFF;
    nonce[3] = g_mesh.config.node_id & 0xFF;
    esp_fill_random(&nonce[4], 9);

    uint8_t tag[MIC_TAG_LEN];
    int ret = mbedtls_ccm_encrypt_and_tag(&s_sec.ctx, *len, nonce, sizeof(nonce),
                                          NULL, 0, data, data, tag, sizeof(tag));
    if (ret != 0) {
        SEC_LOG(ESP_LOG_ERROR, "Encrypt failed: %d", ret);
        return ESP_FAIL;
    }

    /* Append MIC tag */
    memcpy(data + *len, tag, MIC_TAG_LEN);
    *len += MIC_TAG_LEN;

    return ESP_OK;
}

/**
 * @brief Decrypt ciphertext in-place.
 *
 * Input:  data[0..*len-1] = ciphertext || MIC tag (last MIC_TAG_LEN bytes)
 * Output: data[0..*len-MIC_TAG_LEN-1] = plaintext
 *         *len is decremented by MIC_TAG_LEN.
 */
esp_err_t mesh_security_decrypt(uint8_t *data, size_t *len) {
    if (!s_sec.initialized) return ESP_ERR_INVALID_STATE;
    if (!data || !len || *len <= MIC_TAG_LEN) return MESH_ESPNOW_ERR_INVALID_PARAM;

    size_t cipher_len = *len - MIC_TAG_LEN;

    uint8_t nonce[13];
    nonce[0] = (g_mesh.config.node_id >> 24) & 0xFF;
    nonce[1] = (g_mesh.config.node_id >> 16) & 0xFF;
    nonce[2] = (g_mesh.config.node_id >> 8) & 0xFF;
    nonce[3] = g_mesh.config.node_id & 0xFF;
    esp_fill_random(&nonce[4], 9);

    uint8_t tag[MIC_TAG_LEN];
    memcpy(tag, data + cipher_len, MIC_TAG_LEN);

    int ret = mbedtls_ccm_auth_decrypt(&s_sec.ctx, cipher_len, nonce, sizeof(nonce),
                                       NULL, 0, data, data, tag, sizeof(tag));
    if (ret != 0) {
        SEC_LOG(ESP_LOG_WARN, "Decrypt/MIC failed: %d", ret);
        return MESH_ESPNOW_ERR_DECRYPT_FAILED;
    }

    *len = cipher_len;
    return ESP_OK;
}
