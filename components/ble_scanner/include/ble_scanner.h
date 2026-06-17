#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "bthome.h"  // for sensor_data_t

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Callback invoked for every successfully parsed advertisement
 * (BTHome v2 or Ecowitt WS90).
 *
 * @param mac   6-byte BLE MAC of the sender
 * @param data  Parsed sensor readings (owned by callee — do not store pointer)
 */
typedef void (*ble_scanner_cb_t)(const uint8_t mac[6], const sensor_data_t *data);

esp_err_t ble_scanner_init(ble_scanner_cb_t callback);
esp_err_t ble_scanner_start(void);
esp_err_t ble_scanner_stop(void);

#ifdef __cplusplus
}
#endif
