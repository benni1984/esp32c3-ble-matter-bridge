#pragma once

#include "bthome.h"
#include "sensor_registry.h"
#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Callback type – called when Matter commissioning is complete. */
typedef void (*matter_bridge_commissioned_cb_t)(void);

/**
 * Initialise the Matter node, create the bridge aggregator endpoint,
 * and restore any previously discovered sensors as Matter endpoints.
 *
 * @param on_commissioned  Called once BLE commissioning finishes (or
 *                         immediately if already commissioned).
 */
esp_err_t matter_bridge_init(matter_bridge_commissioned_cb_t on_commissioned);

/** Start the Matter stack.  Must be called after matter_bridge_init(). */
esp_err_t matter_bridge_start(void);

/**
 * Process a fully parsed BTHome packet:
 *   - Look up or create the sensor entry in the registry.
 *   - Create missing Matter endpoints for newly seen measurement types.
 *   - Update attribute values for existing endpoints.
 */
void matter_bridge_update(const uint8_t mac[6], const sensor_data_t *data);

/** Print the Matter commissioning QR code and manual pairing code to serial. */
void matter_bridge_print_pairing_info(void);

/** Return true if the device has at least one commissioned fabric. */
bool matter_bridge_is_commissioned(void);

#ifdef __cplusplus
}
#endif
