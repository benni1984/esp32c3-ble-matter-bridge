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

static_assert(SENSOR_TYPE_COUNT <= 16, "known_type_mask is a uint16_t — widen it before adding more sensor types");

/**
 * One entry per unique BLE sensor (keyed by MAC address).
 * The matter_endpoint_id[] array is indexed by sensor_type_t and is rebuilt
 * from scratch every boot by the endpoint pre-creation loop — an ID of 0
 * means "no live endpoint this boot" (either not yet due, or no Matter
 * mapping for that type).
 * known_type_mask is append-only and persists across boots: bit i set means
 * sensor_type i has ever been observed for this MAC and is due to get a
 * Matter endpoint pre-created at the *next* boot if it doesn't have one yet.
 * Endpoints can't be created mid-session (Matter requires the full endpoint
 * set to exist before a controller connects), so this is the durable
 * "what should exist next time" record that matter_endpoint_id[] alone
 * can't provide.
 */
typedef struct {
    uint8_t  mac[6];
    char     name[32];
    uint16_t matter_endpoint_id[SENSOR_TYPE_COUNT]; // 0 = not created this boot
    uint16_t known_type_mask;                       // bit i = sensor_type i seen, ever
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

/**
 * Mark sensor_type t as known/persisted for entry (sets a bit in
 * known_type_mask). No-op if already set.
 * Caller is responsible for calling sensor_registry_save() afterward if
 * this returns true — kept separate so multiple types can be marked in a
 * batch with a single NVS write.
 * @return true if the bit was newly set (i.e. this type was never seen
 *         before for this entry), false if it was already known.
 */
bool sensor_registry_mark_known(registry_entry_t *entry, sensor_type_t t);

/**
 * Set (or overwrite) the human-readable name for an existing entry and
 * persist it immediately. Unlike sensor_registry_get_or_create()'s name
 * argument (which only fills in a name if none is set yet), this always
 * overwrites — it backs the 'sensor_reg name' console command.
 * @return ESP_ERR_NOT_FOUND if no entry exists for that MAC.
 */
esp_err_t sensor_registry_set_name(const uint8_t mac[6], const char *name);

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
