#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ─── Sensor types ─────────────────────────────────────────────────────────────

/**
 * All measurement types that can be carried in a BTHome advertisement.
 * To add support for a new type:
 *   1. Add an entry here.
 *   2. Add a row in the s_objects[] table in bthome.cpp.
 *   3. Handle the new type in matter_bridge.cpp.
 * That is all. See docs/adding_a_sensor.md for a step-by-step guide.
 */
typedef enum {
    SENSOR_BATTERY,           // %   (0-100)
    SENSOR_TEMPERATURE,       // °C
    SENSOR_HUMIDITY,          // %
    SENSOR_PRESSURE,          // hPa
    SENSOR_ILLUMINANCE,       // lux
    SENSOR_WIND_SPEED,        // m/s
    SENSOR_WIND_SPEED_GUST,   // m/s  (peak gust)
    SENSOR_WIND_DIRECTION,    // degrees (0-360)
    SENSOR_RAIN,              // mm/h
    SENSOR_UV_INDEX,          // index (0-11+)
    SENSOR_TYPE_COUNT         // keep last – used for array sizing
} sensor_type_t;

// ─── Parsed sensor data ───────────────────────────────────────────────────────

typedef struct {
    sensor_type_t type;
    float         value;
} sensor_reading_t;

#define BTHOME_MAX_READINGS 12

typedef struct {
    uint8_t           mac[6];
    char              name[32];   // set by caller from advertisement
    sensor_reading_t  readings[BTHOME_MAX_READINGS];
    int               reading_count;
} sensor_data_t;

// ─── Parser API ───────────────────────────────────────────────────────────────

/**
 * Parse a raw BTHome v2 service-data payload.
 *
 * @param svc_data  Pointer to the bytes immediately after the 0xFCD2 UUID
 * @param len       Number of bytes in svc_data
 * @param out       Output struct filled by the parser
 * @return true if at least one reading was parsed successfully
 *
 * Encrypted advertisements are skipped (returns false).
 * Unknown object IDs are silently ignored so the rest of the packet is parsed.
 */
bool bthome_parse(const uint8_t *svc_data, size_t len, sensor_data_t *out);

/** Human-readable name for a sensor type (for logging). */
const char *sensor_type_name(sensor_type_t type);

#ifdef __cplusplus
}
#endif
