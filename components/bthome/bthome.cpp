#include "bthome.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_console.h"
#include "mbedtls/ccm.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "bthome";

// ─── BTHome v2 device-info byte ──────────────────────────────────────────────

static constexpr uint8_t BTHOME_VERSION_MASK = 0xC0;
static constexpr uint8_t BTHOME_VERSION_V2   = 0x40;
static constexpr uint8_t BTHOME_ENCRYPT_FLAG = 0x01;
static constexpr uint8_t BTHOME_MAC_INCLUDED = 0x08;

// Service UUID for BTHome v2 (used in CCM nonce construction)
static constexpr uint8_t BTHOME_UUID_LSB = 0xD2;
static constexpr uint8_t BTHOME_UUID_MSB = 0xFC;

// ─── Object descriptor ───────────────────────────────────────────────────────

struct ObjDef {
    uint8_t       obj_id;
    sensor_type_t type;
    float         factor;
    uint8_t       byte_size;
    bool          is_signed;
};

static constexpr ObjDef s_objects[] = {
    // Standard BTHome v2 object IDs (from bthome.io/format/)
    { 0x01, SENSOR_BATTERY,         1.0f,  1, false },
    { 0x02, SENSOR_TEMPERATURE,     0.01f, 2, true  },
    { 0x03, SENSOR_HUMIDITY,        0.01f, 2, false },
    { 0x04, SENSOR_PRESSURE,        0.01f, 3, false },
    { 0x05, SENSOR_ILLUMINANCE,     0.01f, 3, false },
    { 0x08, SENSOR_TEMPERATURE,     0.01f, 2, true  },  // dewpoint (sint16 ×0.01°C)
    { 0x2E, SENSOR_HUMIDITY,        1.0f,  1, false },  // humidity uint8 ×1%
    { 0x44, SENSOR_WIND_SPEED,      0.01f, 2, false },  // speed ×0.01 m/s (wind avg & gust)
    { 0x45, SENSOR_TEMPERATURE,     0.1f,  2, true  },  // temperature sint16 ×0.1°C
    { 0x46, SENSOR_UV_INDEX,        0.1f,  1, false },  // UV index uint8 ×0.1
    { 0x5E, SENSOR_WIND_DIRECTION,  0.01f, 2, false },  // direction ×0.01°
    { 0x5F, SENSOR_RAIN,            0.1f,  2, false },  // precipitation ×0.1 mm
};
static constexpr int N_OBJECTS = sizeof(s_objects) / sizeof(s_objects[0]);

// ─── Key store ───────────────────────────────────────────────────────────────

#define MAX_KEYS  16
#define NVS_NS    "bthome_keys"

struct KeyEntry {
    uint8_t mac[6];
    uint8_t key[BTHOME_KEY_LEN];
};

static KeyEntry s_keys[MAX_KEYS];
static int      s_key_count = 0;

static void mac_to_str(const uint8_t mac[6], char out[13])
{
    snprintf(out, 13, "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void bthome_key_store_init(void)
{
    // Keys are loaded lazily from NVS on first lookup — nothing to do here.
    ESP_LOGI(TAG, "Key store ready (keys loaded from NVS on first use)");
}

static bool find_key_in_cache(const uint8_t mac[6], uint8_t key_out[BTHOME_KEY_LEN])
{
    for (int i = 0; i < s_key_count; i++) {
        if (memcmp(s_keys[i].mac, mac, 6) == 0) {
            memcpy(key_out, s_keys[i].key, BTHOME_KEY_LEN);
            return true;
        }
    }
    return false;
}

static bool find_key(const uint8_t mac[6], uint8_t key_out[BTHOME_KEY_LEN])
{
    if (find_key_in_cache(mac, key_out)) return true;

    // Try NVS
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;

    char nvs_key[13];
    mac_to_str(mac, nvs_key);
    size_t len = BTHOME_KEY_LEN;
    uint8_t key[BTHOME_KEY_LEN];
    esp_err_t err = nvs_get_blob(h, nvs_key, key, &len);
    nvs_close(h);

    if (err != ESP_OK || len != BTHOME_KEY_LEN) return false;

    // Cache in RAM
    if (s_key_count < MAX_KEYS) {
        memcpy(s_keys[s_key_count].mac, mac, 6);
        memcpy(s_keys[s_key_count].key, key, BTHOME_KEY_LEN);
        s_key_count++;
    }
    memcpy(key_out, key, BTHOME_KEY_LEN);
    return true;
}

esp_err_t bthome_set_key(const uint8_t mac[6], const uint8_t key[BTHOME_KEY_LEN])
{
    // Update or insert in RAM cache
    for (int i = 0; i < s_key_count; i++) {
        if (memcmp(s_keys[i].mac, mac, 6) == 0) {
            memcpy(s_keys[i].key, key, BTHOME_KEY_LEN);
            goto write_nvs;
        }
    }
    if (s_key_count >= MAX_KEYS) {
        ESP_LOGE(TAG, "Key store full (max %d keys)", MAX_KEYS);
        return ESP_ERR_NO_MEM;
    }
    memcpy(s_keys[s_key_count].mac, mac, 6);
    memcpy(s_keys[s_key_count].key, key, BTHOME_KEY_LEN);
    s_key_count++;

write_nvs:;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    char nvs_key[13];
    mac_to_str(mac, nvs_key);
    err = nvs_set_blob(h, nvs_key, key, BTHOME_KEY_LEN);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t bthome_clear_key(const uint8_t mac[6])
{
    // Remove from RAM cache
    for (int i = 0; i < s_key_count; i++) {
        if (memcmp(s_keys[i].mac, mac, 6) == 0) {
            s_keys[i] = s_keys[--s_key_count];
            break;
        }
    }
    // Remove from NVS
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    char nvs_key[13];
    mac_to_str(mac, nvs_key);
    err = nvs_erase_key(h, nvs_key);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) nvs_commit(h);
    nvs_close(h);
    return err;
}

// ─── AES-128-CCM decryption ──────────────────────────────────────────────────
//
// BTHome v2 encrypted packet layout (after dev_info + optional MAC):
//   [encrypted_objects … N bytes] [MIC 4 bytes] [counter 4 bytes]
//
// CCM nonce (13 bytes):
//   mac[6] (as received in BLE adv, little-endian)
//   | 0xD2 0xFC (BTHome service UUID, LE)
//   | dev_info (1 byte)
//   | counter  (4 bytes, LE)

static bool decrypt_bthome(const uint8_t mac[6], uint8_t dev_info,
                            const uint8_t *enc, size_t enc_len,
                            const uint8_t mic[4], const uint8_t counter_bytes[4],
                            uint8_t *plain_out)
{
    uint8_t key[BTHOME_KEY_LEN];
    if (!find_key(mac, key)) {
        ESP_LOGW(TAG, "No bindkey for %02X:%02X:%02X:%02X:%02X:%02X"
                      " – use 'bthome_key set' to configure",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return false;
    }

    uint8_t nonce[13];
    memcpy(nonce, mac, 6);
    nonce[6] = BTHOME_UUID_LSB;
    nonce[7] = BTHOME_UUID_MSB;
    nonce[8] = dev_info;
    memcpy(nonce + 9, counter_bytes, 4);

    mbedtls_ccm_context ccm;
    mbedtls_ccm_init(&ccm);
    int rc = mbedtls_ccm_setkey(&ccm, MBEDTLS_CIPHER_ID_AES, key, 128);
    if (rc == 0) {
        rc = mbedtls_ccm_auth_decrypt(&ccm, enc_len,
                                       nonce, sizeof(nonce),
                                       nullptr, 0,
                                       enc, plain_out,
                                       mic, 4);
    }
    mbedtls_ccm_free(&ccm);

    if (rc != 0) {
        ESP_LOGW(TAG, "AES-CCM decrypt failed (rc=%d) for %02X:%02X:%02X:%02X:%02X:%02X"
                      " – wrong bindkey?",
                 rc, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return false;
    }
    return true;
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

static const ObjDef *find_obj(uint8_t id)
{
    for (int i = 0; i < N_OBJECTS; i++) {
        if (s_objects[i].obj_id == id) return &s_objects[i];
    }
    return nullptr;
}

static uint32_t read_le_uint(const uint8_t *p, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v |= ((uint32_t)p[i] << (8 * i));
    return v;
}

static int32_t read_le_sint(const uint8_t *p, int n)
{
    uint32_t u = read_le_uint(p, n);
    int shift = (4 - n) * 8;
    return (int32_t)(u << shift) >> shift;
}

// ─── Parser ──────────────────────────────────────────────────────────────────

bool bthome_parse(const uint8_t mac[6], const uint8_t *svc_data, size_t len,
                  sensor_data_t *out)
{
    if (!svc_data || len < 1 || !out) return false;

    const uint8_t  dev_info = svc_data[0];
    const uint8_t *p        = svc_data + 1;
    const uint8_t *end      = svc_data + len;

    if ((dev_info & BTHOME_VERSION_MASK) != BTHOME_VERSION_V2) {
        ESP_LOGW(TAG, "Unsupported BTHome version (dev_info=0x%02X)", dev_info);
        return false;
    }

    // Skip optional MAC prefix embedded in the advertisement payload
    if ((dev_info & BTHOME_MAC_INCLUDED) && (p + 6 <= end)) {
        p += 6;
    }

    // Decrypt if needed
    static uint8_t s_plain[64];  // scratch buffer; single-threaded NimBLE host task
    if (dev_info & BTHOME_ENCRYPT_FLAG) {
        // Need at least MIC(4) + counter(4) = 8 bytes after the header
        if ((size_t)(end - p) < 8) {
            ESP_LOGW(TAG, "Encrypted packet too short (%d bytes)", (int)(end - p));
            return false;
        }
        const uint8_t *counter_bytes = end - 4;
        const uint8_t *mic           = end - 8;
        size_t enc_len = (size_t)(mic - p);

        if (enc_len > sizeof(s_plain)) {
            ESP_LOGW(TAG, "Encrypted payload too large (%d bytes)", (int)enc_len);
            return false;
        }

        if (!decrypt_bthome(mac, dev_info, p, enc_len, mic, counter_bytes, s_plain)) {
            return false;
        }
        p   = s_plain;
        end = s_plain + enc_len;
    }

    out->reading_count = 0;
    while (p < end && out->reading_count < BTHOME_MAX_READINGS) {
        uint8_t obj_id = *p++;
        const ObjDef *def = find_obj(obj_id);
        if (!def) {
            // Binary sensors (0x0F, 0x10-0x2D) are always 1 byte — skip gracefully
            if (obj_id == 0x0F || (obj_id >= 0x10 && obj_id <= 0x2D)) {
                if (p < end) p++;  // skip the 1-byte value
                continue;
            }
            ESP_LOGI(TAG, "Unknown BTHome obj_id 0x%02X after %d readings – stopping",
                     obj_id, out->reading_count);
            break;
        }
        if (p + def->byte_size > end) {
            ESP_LOGW(TAG, "obj_id 0x%02X: not enough bytes remaining", obj_id);
            break;
        }
        float value = def->is_signed
            ? (float)read_le_sint(p, def->byte_size) * def->factor
            : (float)read_le_uint(p, def->byte_size) * def->factor;
        p += def->byte_size;
        out->readings[out->reading_count++] = { def->type, value };
        ESP_LOGD(TAG, "  %s = %.2f", sensor_type_name(def->type), value);
    }

    return out->reading_count > 0;
}

const char *sensor_type_name(sensor_type_t type)
{
    static const char *names[] = {
        "battery", "temperature", "humidity", "pressure", "illuminance",
        "wind_speed", "wind_gust", "wind_direction", "rain", "uv_index",
    };
    static_assert(sizeof(names)/sizeof(names[0]) == SENSOR_TYPE_COUNT,
                  "sensor_type_name: table out of sync with enum");
    if (type < 0 || type >= SENSOR_TYPE_COUNT) return "unknown";
    return names[type];
}

// ─── Console command: bthome_key ─────────────────────────────────────────────
//
//  bthome_key set AA:BB:CC:DD:EE:FF 00112233445566778899AABBCCDDEEFF
//  bthome_key del AA:BB:CC:DD:EE:FF
//  bthome_key list

static bool parse_mac(const char *s, uint8_t mac[6])
{
    return sscanf(s, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                  &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) == 6;
}

static bool parse_hex_key(const char *s, uint8_t key[BTHOME_KEY_LEN])
{
    if (strlen(s) != BTHOME_KEY_LEN * 2) return false;
    for (int i = 0; i < BTHOME_KEY_LEN; i++) {
        unsigned byte;
        if (sscanf(s + i * 2, "%02x", &byte) != 1) return false;
        key[i] = (uint8_t)byte;
    }
    return true;
}

static int cmd_bthome_key(int argc, char **argv)
{
    if (argc < 2) goto usage;

    if (strcmp(argv[1], "set") == 0) {
        if (argc != 4) goto usage;
        uint8_t mac[6], key[BTHOME_KEY_LEN];
        if (!parse_mac(argv[2], mac)) {
            printf("Invalid MAC — use format AA:BB:CC:DD:EE:FF\n");
            return 1;
        }
        if (!parse_hex_key(argv[3], key)) {
            printf("Invalid key — must be exactly 32 hex characters (16 bytes)\n");
            return 1;
        }
        esp_err_t err = bthome_set_key(mac, key);
        if (err == ESP_OK) {
            printf("Bindkey stored for %s\n", argv[2]);
        } else {
            printf("Error: %s\n", esp_err_to_name(err));
            return 1;
        }

    } else if (strcmp(argv[1], "del") == 0) {
        if (argc != 3) goto usage;
        uint8_t mac[6];
        if (!parse_mac(argv[2], mac)) {
            printf("Invalid MAC — use format AA:BB:CC:DD:EE:FF\n");
            return 1;
        }
        bthome_clear_key(mac);
        printf("Bindkey removed for %s\n", argv[2]);

    } else if (strcmp(argv[1], "list") == 0) {
        if (s_key_count == 0) {
            printf("No bindkeys stored in RAM cache (NVS may have more — they load on first use)\n");
        } else {
            printf("Cached bindkeys (%d):\n", s_key_count);
            for (int i = 0; i < s_key_count; i++) {
                printf("  %02X:%02X:%02X:%02X:%02X:%02X  ",
                       s_keys[i].mac[0], s_keys[i].mac[1], s_keys[i].mac[2],
                       s_keys[i].mac[3], s_keys[i].mac[4], s_keys[i].mac[5]);
                for (int j = 0; j < BTHOME_KEY_LEN; j++)
                    printf("%02X", s_keys[i].key[j]);
                printf("\n");
            }
        }
    } else {
        goto usage;
    }
    return 0;

usage:
    printf("Usage:\n");
    printf("  bthome_key set <MAC> <KEY>   store bindkey (KEY = 32 hex chars)\n");
    printf("  bthome_key del <MAC>         remove bindkey\n");
    printf("  bthome_key list              show cached keys\n");
    printf("\nExample:\n");
    printf("  bthome_key set A4:C1:38:AA:BB:CC 00112233445566778899AABBCCDDEEFF\n");
    return 1;
}

void bthome_register_console_command(void)
{
    const esp_console_cmd_t cmd = {
        .command = "bthome_key",
        .help    = "Manage BTHome bindkeys for encrypted sensors",
        .hint    = "set|del|list",
        .func    = cmd_bthome_key,
        .argtable = nullptr,
    };
    esp_console_cmd_register(&cmd);
    ESP_LOGI(TAG, "Console: 'bthome_key' command registered");
}
