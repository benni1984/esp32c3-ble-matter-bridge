#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Callback invoked for every received BTHome advertisement.
 *
 * @param mac          6-byte Bluetooth MAC address of the sender
 * @param name         Device name from the advertisement (may be NULL)
 * @param svc_data     Raw BTHome service data (after the 0xFCD2 UUID bytes)
 * @param svc_data_len Length of svc_data in bytes
 * @param rssi         Signal strength in dBm
 */
typedef void (*ble_scanner_cb_t)(const uint8_t mac[6],
                                  const char   *name,
                                  const uint8_t *svc_data,
                                  size_t         svc_data_len,
                                  int8_t         rssi);

/**
 * Initialise the NimBLE host and register the advertisement callback.
 * Call once before ble_scanner_start().
 *
 * The BLE host task is started internally; do NOT call nimble_port_init()
 * separately when using this component.
 */
esp_err_t ble_scanner_init(ble_scanner_cb_t callback);

/**
 * Begin passive BLE scanning.
 *
 * Call after Matter commissioning is complete so that BLE is no longer
 * needed for the GATT commissioning channel.
 */
esp_err_t ble_scanner_start(void);

/** Stop passive BLE scanning (e.g. before re-commissioning). */
esp_err_t ble_scanner_stop(void);

#ifdef __cplusplus
}
#endif
