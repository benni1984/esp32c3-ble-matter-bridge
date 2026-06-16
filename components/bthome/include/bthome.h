#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// ─── Sensor types ─────────────────────────────────────────────────────────────

typedef enum {
    SENSOR_BATTERY,
    SENSOR_TEMPERATURE,
    SENSOR_HUMIDITY,
    SENSOR_PRESSURE,
    SENSOR_ILLUMINANCE,
    SENSOR_WIND_SPEED,
    SENSOR_WIND_SPEED_GUST,
    SENSOR_WIND_DIRECTION,
    SENSOR_RAIN,
    SENSOR_UV_INDEX,
    SENSOR_TYPE_COUNT
} sensor_type_t;

// ─── Parsed sensor data ───────────────────────────────────────────────────────

typedef struct {
    sensor_type_t type;
    float         value;
} sensor_reading_t;

#define BTHOME_MAX_READINGS 12

typedef struct {
    uint8_t           mac[6];
    char              name[32];
    sensor_reading_t  readings[BTHOME_MAX_READINGS];
    int               reading_count;
} sensor_data_t;

// ─── Encryption key store ─────────────────────────────────────────────────────

#define BTHOME_KEY_LEN 16

/**
 * Load persisted bindkeys from NVS into the in-memory cache.
 * Call once at startup before bthome_parse() is used.
 */
void bthome_key_store_init(void);

/**
 * Store a BTHome bindkey for a sensor identified by its BLE MAC address.
 * Persisted to NVS — survives reboot.
 *
 * @param mac  6-byte MAC (as received in the BLE advertisement)
 * @param key  16-byte AES-128 bindkey (from the Shelly app / BTHome device)
 */
esp_err_t bthome_set_key(const uint8_t mac[6], const uint8_t key[BTHOME_KEY_LEN]);

/** Remove the bindkey for a sensor from RAM cache and NVS. */
esp_err_t bthome_clear_key(const uint8_t mac[6]);

// ─── Parser API ───────────────────────────────────────────────────────────────

/**
 * Parse a raw BTHome v2 service-data payload.
 * Encrypted packets are decrypted automatically if a bindkey is stored.
 *
 * @param mac       6-byte BLE MAC of the sender (needed for decryption nonce)
 * @param svc_data  Bytes immediately after the 0xFCD2 UUID in the BLE advertisement
 * @param len       Length of svc_data
 * @param out       Output struct filled on success
 * @return true if at least one reading was parsed successfully
 */
bool bthome_parse(const uint8_t mac[6], const uint8_t *svc_data, size_t len,
                  sensor_data_t *out);

/** Human-readable name for a sensor type (for logging). */
const char *sensor_type_name(sensor_type_t type);

/** Register 'bthome_key' console command (call after esp_matter::console::init()). */
void bthome_register_console_command(void);

#ifdef __cplusplus
}
#endif
