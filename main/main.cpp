#include "ble_scanner.h"
#include "bthome.h"
#include "sensor_registry.h"
#include "matter_bridge.h"

#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

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

    if (!bthome_parse(mac, svc_data, svc_data_len, &data)) {
        return;  // parse failed (encrypted or unknown version)
    }

    ESP_LOGI(TAG, "BLE [%02X:%02X:%02X:%02X:%02X:%02X] %s rssi=%d readings=%d",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             name ? name : "?", rssi, data.reading_count);

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

    // Load persisted sensor list and bindkeys.
    sensor_registry_init();
    bthome_key_store_init();

    // Set up the Matter node, aggregator endpoint, and restore previous sensors.
    matter_bridge_init(on_commissioned);

    // Start the Matter stack first — esp_matter::start() loads fabrics from NVS
    // synchronously, so matter_bridge_is_commissioned() returns the correct value
    // only AFTER this call.
    matter_bridge_start();  // also registers bthome_key console command

    if (matter_bridge_is_commissioned()) {
        // Already paired. Matter holds NimBLE for its own commissioning channel
        // and releases it after ~3-4 seconds (logs "BLE deinit successful").
        // We must wait for that release before initialising the BLE scanner.
        ESP_LOGI(TAG, "Already commissioned – BLE scan will start after Matter releases BLE");
        xTaskCreate([](void *) {
            vTaskDelay(pdMS_TO_TICKS(2000));  // wait for NimBLE to be synced by Matter
            ESP_LOGI("main", "Starting BLE sensor scan...");
            ble_scanner_init(on_ble_advertisement);
            ble_scanner_start();
            vTaskDelete(nullptr);
        }, "ble_start", 4096, nullptr, 1, nullptr);
    } else {
        // First boot or factory reset.
        // Matter will use BLE for commissioning; scanning starts in on_commissioned().
        ESP_LOGI(TAG, "Not commissioned. Waiting for Matter commissioning...");
        ESP_LOGI(TAG, "Use Apple Home or Home Assistant to scan the QR code below.");

        // Repeat QR code every 5 s for up to 3 minutes after boot.
        // After that window the QR code is silenced until the next reboot —
        // once commissioned the task exits immediately via is_commissioned().
        xTaskCreate([](void *) {
            const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(3 * 60 * 1000);
            while (!matter_bridge_is_commissioned() &&
                   xTaskGetTickCount() < deadline) {
                matter_bridge_print_pairing_info();
                vTaskDelay(pdMS_TO_TICKS(5000));
            }
            if (!matter_bridge_is_commissioned()) {
                ESP_LOGW("main", "QR code window closed (3 min). Reboot to show QR code again.");
            }
            vTaskDelete(nullptr);
        }, "qr_repeat", 4096, nullptr, 1, nullptr);
    }

    // app_main returns here; all work happens in background FreeRTOS tasks.
}
