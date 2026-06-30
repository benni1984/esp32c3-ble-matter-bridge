#pragma once

#include "bthome.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*shelly_poller_cb_t)(const uint8_t mac[6], const sensor_data_t *data);

esp_err_t shelly_poller_init(shelly_poller_cb_t cb);
esp_err_t shelly_poller_add_url(const char *shelly_ip);
void      shelly_poller_start(void);

#ifdef __cplusplus
}
#endif
