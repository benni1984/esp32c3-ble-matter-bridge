#include "shelly_poller.h"
#include "sensor_registry.h"
#include "matter_bridge.h"

#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

static void on_sensor_data(const uint8_t mac[6], const sensor_data_t *data)
{
    ESP_LOGI(TAG, "BLE [%02X:%02X:%02X:%02X:%02X:%02X] %s readings=%d",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             data->name[0] ? data->name : "?", data->reading_count);
    matter_bridge_update(mac, data);
}

static void on_commissioned(void)
{
    ESP_LOGI(TAG, "Commissioning complete — starting Shelly poller");
    shelly_poller_start();
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "BLE-Matter-Bridge starting");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition erased and re-initialised");
        nvs_flash_erase();
        nvs_flash_init();
    }

    esp_event_loop_create_default();
    sensor_registry_init();
    bthome_key_store_init();

    // WS90 "Powered by Shelly" — BTHome chip MAC FC:4D:6A:13:3D:0D, unencrypted
    {
        static const uint8_t ws90_shelly_mac[6] = {0xFC, 0x4D, 0x6A, 0x13, 0x3D, 0x0D};
        static const uint8_t ws90_shelly_key[16] = {};
        bthome_set_key(ws90_shelly_mac, ws90_shelly_key);
    }

    // Shelly BLE relay IPs — hardcoded, both on Fanny_IoT (192.168.1.x).
    // mDNS discovery is unreliable across VLAN boundaries; fixed IPs are stable.
    shelly_poller_init(on_sensor_data);
    shelly_poller_add_url("192.168.1.81");
    shelly_poller_add_url("192.168.1.173");

    matter_bridge_init(on_commissioned);
    matter_bridge_start();

    if (matter_bridge_is_commissioned()) {
        // Already commissioned (reboot with existing fabric + WiFi credentials).
        // kCommissioningComplete won't fire again, so start poller directly.
        ESP_LOGI(TAG, "Already commissioned — starting Shelly poller");
        shelly_poller_start();
    } else {
        ESP_LOGI(TAG, "Not commissioned. Waiting for Matter commissioning...");
        ESP_LOGI(TAG, "Use Apple Home or Home Assistant to scan the QR code below.");
        xTaskCreate([](void *) {
            const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(3 * 60 * 1000);
            while (!matter_bridge_is_commissioned() && xTaskGetTickCount() < deadline) {
                matter_bridge_print_pairing_info();
                vTaskDelay(pdMS_TO_TICKS(5000));
            }
            if (!matter_bridge_is_commissioned()) {
                ESP_LOGW("main", "QR code window closed (3 min). Reboot to show QR code again.");
            }
            vTaskDelete(nullptr);
        }, "qr_repeat", 4096, nullptr, 1, nullptr);
    }
}
