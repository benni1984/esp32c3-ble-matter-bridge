#include "bthome.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "bthome";

// ─── BTHome v2 device-info byte ──────────────────────────────────────────────
//
//  Bit  7  6  | 5  4  3 (reserved) | 2 (trigger) | 1 (reserved) | 0 (encrypt)
//  Version  ──┘
//
//  Version 2 → bits [7:6] = 0b10  → byte & 0xC0 == 0x40 (when no other flags set)
//  Encryption flag in bit 0 → if set the payload is AES-CCM encrypted; skip it.

static constexpr uint8_t BTHOME_VERSION_MASK    = 0xC0;
static constexpr uint8_t BTHOME_VERSION_V2      = 0x40;
static constexpr uint8_t BTHOME_ENCRYPT_FLAG    = 0x01;
static constexpr uint8_t BTHOME_MAC_INCLUDED    = 0x08;  // MAC prepended to objects

// ─── Object descriptor ───────────────────────────────────────────────────────

struct ObjDef {
    uint8_t       obj_id;
    sensor_type_t type;
    float         factor;    // raw_value * factor = physical value
    uint8_t       byte_size; // payload bytes for this object
    bool          is_signed; // interpret payload as signed integer?
};

// ─── Object ID table ─────────────────────────────────────────────────────────
//
// Standard BTHome v2 objects:  https://bthome.io/format/
// WS90 proprietary extensions: https://shelly-api-docs.shelly.cloud/docs-ble/Devices/BLU_ZB/wstation/
//
// To add a new sensor type: add a row here + a SENSOR_xxx enum in bthome.h.

static constexpr ObjDef s_objects[] = {
    // obj_id  type                      factor   bytes  signed
    { 0x01,  SENSOR_BATTERY,           1.0f,    1,     false },
    { 0x02,  SENSOR_TEMPERATURE,       0.01f,   2,     true  },
    { 0x03,  SENSOR_HUMIDITY,          0.01f,   2,     false },
    { 0x04,  SENSOR_PRESSURE,          0.01f,   3,     false },
    { 0x05,  SENSOR_ILLUMINANCE,       0.01f,   3,     false },
    { 0x4A,  SENSOR_UV_INDEX,          0.1f,    1,     false },
    { 0x44,  SENSOR_WIND_DIRECTION,    0.01f,   2,     false },  // BTHome v2
    { 0x58,  SENSOR_ILLUMINANCE,       1.0f,    3,     false },  // illuminance (alt)
    { 0x5E,  SENSOR_WIND_DIRECTION,    0.01f,   2,     false },  // alt wind dir
    { 0x5F,  SENSOR_RAIN,              0.1f,    2,     false },  // precipitation
    // ── WS90 "powered by Shelly" proprietary ──────────────────────────────────
    { 0xD1,  SENSOR_WIND_SPEED,        0.1f,    2,     false },  // avg wind speed m/s
    { 0xD2,  SENSOR_WIND_DIRECTION,    1.0f,    2,     false },  // wind direction °
    { 0xD3,  SENSOR_WIND_SPEED_GUST,   0.1f,    2,     false },  // gust speed m/s
    { 0xD4,  SENSOR_RAIN,              0.1f,    2,     false },  // rain mm/h
};
static constexpr int N_OBJECTS = sizeof(s_objects) / sizeof(s_objects[0]);

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
    // sign-extend
    int shift = (4 - n) * 8;
    return (int32_t)(u << shift) >> shift;
}

// ─── Parser ──────────────────────────────────────────────────────────────────

bool bthome_parse(const uint8_t *svc_data, size_t len, sensor_data_t *out)
{
    if (!svc_data || len < 1 || !out) return false;

    const uint8_t dev_info = svc_data[0];
    const uint8_t *p       = svc_data + 1;
    const uint8_t *end     = svc_data + len;

    // Version check
    if ((dev_info & BTHOME_VERSION_MASK) != BTHOME_VERSION_V2) {
        ESP_LOGD(TAG, "Unsupported BTHome version (dev_info=0x%02X)", dev_info);
        return false;
    }

    // Skip encrypted packets (decryption requires a per-device key)
    if (dev_info & BTHOME_ENCRYPT_FLAG) {
        ESP_LOGD(TAG, "Encrypted BTHome packet – skipped");
        return false;
    }

    // Skip optional MAC prefix
    if ((dev_info & BTHOME_MAC_INCLUDED) && (p + 6 <= end)) {
        p += 6;
    }

    out->reading_count = 0;

    while (p < end && out->reading_count < BTHOME_MAX_READINGS) {
        uint8_t obj_id = *p++;
        const ObjDef *def = find_obj(obj_id);

        if (!def) {
            // Unknown object ID.  We cannot determine the payload size,
            // so we have to stop parsing this packet.
            ESP_LOGD(TAG, "Unknown BTHome obj_id 0x%02X – stopping parse", obj_id);
            break;
        }

        if (p + def->byte_size > end) {
            ESP_LOGW(TAG, "obj_id 0x%02X: not enough bytes remaining", obj_id);
            break;
        }

        float value;
        if (def->is_signed) {
            value = (float)read_le_sint(p, def->byte_size) * def->factor;
        } else {
            value = (float)read_le_uint(p, def->byte_size) * def->factor;
        }
        p += def->byte_size;

        out->readings[out->reading_count++] = { def->type, value };
        ESP_LOGD(TAG, "  %s = %.2f", sensor_type_name(def->type), value);
    }

    return out->reading_count > 0;
}

const char *sensor_type_name(sensor_type_t type)
{
    static const char *names[] = {
        "battery",        // SENSOR_BATTERY
        "temperature",    // SENSOR_TEMPERATURE
        "humidity",       // SENSOR_HUMIDITY
        "pressure",       // SENSOR_PRESSURE
        "illuminance",    // SENSOR_ILLUMINANCE
        "wind_speed",     // SENSOR_WIND_SPEED
        "wind_gust",      // SENSOR_WIND_SPEED_GUST
        "wind_direction", // SENSOR_WIND_DIRECTION
        "rain",           // SENSOR_RAIN
        "uv_index",       // SENSOR_UV_INDEX
    };
    static_assert(sizeof(names)/sizeof(names[0]) == SENSOR_TYPE_COUNT,
                  "sensor_type_name: table out of sync with enum");
    if (type < 0 || type >= SENSOR_TYPE_COUNT) return "unknown";
    return names[type];
}
