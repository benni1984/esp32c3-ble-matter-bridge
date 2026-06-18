#pragma once

#include "bthome.h"
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REGISTRY_MAX_SENSORS   16  // max simultaneous BLE sensors
#define REGISTRY_MAX_TYPES      8  // max measurement types per sensor

/**
 * One entry per unique BLE sensor (keyed by MAC address).
 * The matter_endpoint_id[] array is indexed by sensor_type_t.
 * An ID of 0 means "not yet created as a Matter endpoint".
 */
typedef struct {
    uint8_t  mac[6];
    char     name[32];
    uint16_t matter_endpoint_id[SENSOR_TYPE_COUNT]; // 0 = not created
    bool     active;
} registry_entry_t;

/** Initialise the registry and load persisted sensor list from NVS. */
esp_err_t sensor_registry_init(void);

/**
 * Look up or create an entry for the given MAC address.
 * Returns a pointer into the internal table; valid until the next call.
 */
registry_entry_t *sensor_registry_get_or_create(const uint8_t mac[6],
                                                  const char   *name);

/** Persist the registry to NVS (called automatically on first discovery). */
esp_err_t sensor_registry_save(void);

/** Return number of registered sensors. */
int sensor_registry_count(void);

/** Iterate: returns entry i (0-based), or NULL if i >= count. */
registry_entry_t *sensor_registry_get(int i);

/** Remove all sensors from registry and NVS. */
void sensor_registry_clear(void);

/** Remove one sensor by MAC. Returns true if found and removed. */
bool sensor_registry_delete(const uint8_t mac[6]);

/** Register 'sensor_reg' console command (list/del/clear). */
void sensor_registry_register_console_command(void);

#ifdef __cplusplus
}
#endif
