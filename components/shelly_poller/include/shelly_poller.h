#pragma once

#include "bthome.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*shelly_poller_cb_t)(const uint8_t mac[6], const sensor_data_t *data);

// Initialize the poller with a callback. Discovery happens automatically via
// mDNS when the poller starts. Optional fallback URLs can be added with
// shelly_poller_add_fallback() for networks where mDNS is unreliable.
esp_err_t shelly_poller_init(shelly_poller_cb_t cb);

// Add a fallback URL (http://<ip>/rpc/BLE.CloudRelay.ListInfos) tried only
// when mDNS discovery finds nothing.
esp_err_t shelly_poller_add_fallback(const char *shelly_ip);

void      shelly_poller_start(void);

#ifdef __cplusplus
}
#endif
