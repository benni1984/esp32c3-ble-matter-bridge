#include "ble_scanner.h"
#include "bthome.h"
#include "sensor_registry.h"
#include "matter_bridge.h"

#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_event.h"

static const char *TAG = "main";

// ─── BLE advertisement callback ──────────────────────────────────────────────

/**
 * Called from the NimBLE host task for every BTHome advertisement received.
 * Parses the payload and forwards the result to the Matter bridge.
 */
static void on_ble_advertisement(const uint8_t  mac[6],
                                  const char    *name,
                                  const uint8_t *svc_data,
                                  size_t         svc_data_len,
                                  int8_t         rssi)
{
    sensor_data_t data = {};
    memcpy(data.mac, mac, 6);
    if (name) strncpy(data.name, name, sizeof(data.name) - 1);

    if (!bthome_parse(svc_data, svc_data_len, &data)) {
        return;  // parse failed (encrypted or unknown version)
    }

    ESP_LOGD(TAG, "BLE [%02X:%02X:%02X:%02X:%02X:%02X] rssi=%d readings=%d",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             rssi, data.reading_count);

    matter_bridge_update(mac, &data);
}

// ─── Commissioning complete callback ─────────────────────────────────────────

/**
 * Called once Matter commissioning is done.
 * At this point the BLE commissioning channel is released, so we can
 * safely start passive BLE scanning for sensor advertisements.
 */
static void on_commissioned(void)
{
    ESP_LOGI(TAG, "Starting BLE sensor scan...");
    ble_scanner_init(on_ble_advertisement);
    ble_scanner_start();
}

// ─── Entry point ─────────────────────────────────────────────────────────────

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "BLE-Matter-Bridge starting");

    // NVS must be initialised before Matter and the sensor registry.
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition erased and re-initialised");
        nvs_flash_erase();
        nvs_flash_init();
    }

    esp_event_loop_create_default();

    // Load persisted sensor list (if any).
    sensor_registry_init();

    // Set up the Matter node, aggregator endpoint, and restore previous sensors.
    matter_bridge_init(on_commissioned);

    if (matter_bridge_is_commissioned()) {
        // Already paired with Apple Home / Home Assistant on a previous boot.
        // BLE is free to scan immediately (no commissioning needed).
        ESP_LOGI(TAG, "Already commissioned – starting BLE scan immediately");
        ble_scanner_init(on_ble_advertisement);
        ble_scanner_start();
    } else {
        // First boot or factory reset.
        // Matter will use BLE for commissioning; scanning starts in on_commissioned().
        ESP_LOGI(TAG, "Not commissioned. Waiting for Matter commissioning...");
        ESP_LOGI(TAG, "Use Apple Home or Home Assistant to scan the QR code below.");
    }

    // Start the Matter stack (WiFi provisioning + MDNS + attribute server).
    // This call does not block; the Matter task runs in the background.
    matter_bridge_start();

    // app_main returns here; all work happens in background FreeRTOS tasks.
}
