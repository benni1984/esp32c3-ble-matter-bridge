#include "ble_scanner.h"
#include "bthome.h"

#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "ble_scanner";

// BTHome v2 service UUID 0xFCD2 (little-endian)
static constexpr uint8_t BTHOME_UUID_LSB = 0xD2;
static constexpr uint8_t BTHOME_UUID_MSB = 0xFC;

// Ecowitt WS90 service UUID 0xFE9F (little-endian)
static constexpr uint8_t ECOWITT_UUID_LSB = 0x9F;
static constexpr uint8_t ECOWITT_UUID_MSB = 0xFE;

static ble_scanner_cb_t s_callback    = nullptr;
static bool             s_scanning    = false;
static bool             s_owns_nimble = false;
static uint32_t         s_adv_total   = 0;
static uint32_t         s_adv_parsed  = 0;

// Per-MAC rate limiter: suppress repeated callbacks within THROTTLE_MS
#define THROTTLE_MS  5000
#define MAX_TRACKED  8

static struct { uint8_t mac[6]; uint32_t last_ms; } s_throttle[MAX_TRACKED];

static bool throttle_check(const uint8_t mac[6])
{
    uint32_t now = (uint32_t)(xTaskGetTickCount() * (uint64_t)portTICK_PERIOD_MS);
    for (int i = 0; i < MAX_TRACKED; i++) {
        if (memcmp(s_throttle[i].mac, mac, 6) == 0) {
            uint32_t elapsed = now - s_throttle[i].last_ms;
            if (elapsed < THROTTLE_MS) {
                ESP_LOGD(TAG, "throttle: suppressed (elapsed=%lums)", (unsigned long)elapsed);
                return false;
            }
            s_throttle[i].last_ms = now;
            return true;
        }
    }
    int oldest = 0;
    for (int i = 1; i < MAX_TRACKED; i++) {
        if (s_throttle[i].last_ms < s_throttle[oldest].last_ms) oldest = i;
    }
    memcpy(s_throttle[oldest].mac, mac, 6);
    s_throttle[oldest].last_ms = now;
    return true;
}

// Intercept Matter's NimBLE teardown via --wrap (see CMakeLists.txt)
extern "C" int      __wrap_nimble_port_stop(void)   { return 0; }
extern "C" esp_err_t __wrap_nimble_port_deinit(void){ return ESP_OK; }

// ─── Ecowitt WS90 decoder (UUID 0xFE9F) ─────────────────────────────────────
//
// 20-byte payload after the 2-byte UUID:
//   [0]     device type
//   [1]     battery %
//   [2-3]   wind speed avg  LE uint16  ×0.1 m/s
//   [4-5]   wind direction  LE uint16  ×1 °
//   [6-7]   wind gust       LE uint16  ×0.1 m/s
//   [8-9]   rain rate       LE uint16  ×0.1 mm/h
//   [10]    UV index        uint8      ×0.1
//   [11-13] illuminance     LE uint24  ×10 lux
//   [14-19] reserved

static bool parse_ecowitt_ws90(const uint8_t *payload, size_t len,
                                const uint8_t mac[6], const char *name,
                                sensor_data_t *out)
{
    if (len < 14 || !out) return false;

    memcpy(out->mac, mac, 6);
    strncpy(out->name, name ? name : "WS90", sizeof(out->name) - 1);
    out->reading_count = 0;

    auto add = [&](sensor_type_t t, float v) {
        if (out->reading_count < BTHOME_MAX_READINGS)
            out->readings[out->reading_count++] = { t, v };
    };

    add(SENSOR_BATTERY,         (float)payload[1]);
    add(SENSOR_WIND_SPEED,      (uint16_t)(payload[2] | (payload[3] << 8)) * 0.1f);
    add(SENSOR_WIND_DIRECTION,  (float)(uint16_t)(payload[4] | (payload[5] << 8)));
    add(SENSOR_WIND_SPEED_GUST, (uint16_t)(payload[6] | (payload[7] << 8)) * 0.1f);
    add(SENSOR_RAIN,            (uint16_t)(payload[8] | (payload[9] << 8)) * 0.1f);
    add(SENSOR_UV_INDEX,        payload[10] * 0.1f);
    uint32_t lux = (uint32_t)(payload[11] | (payload[12] << 8) | (payload[13] << 16));
    add(SENSOR_ILLUMINANCE,     lux * 10.0f);

    ESP_LOGI(TAG, "WS90 [%02X:%02X:%02X:%02X:%02X:%02X] bat=%d%% "
                  "wind=%.1fm/s dir=%.0f° gust=%.1f rain=%.1fmm/h UV=%.1f lux=%.0f",
             mac[0],mac[1],mac[2],mac[3],mac[4],mac[5],
             payload[1],
             out->readings[1].value, out->readings[2].value,
             out->readings[3].value, out->readings[4].value,
             out->readings[5].value, out->readings[6].value);

    // Dump bytes [14..len-1] to find hidden temp/humidity fields
    if (len > 14) {
        char hex[64] = {};
        int pos = 0;
        for (size_t i = 14; i < len && pos < 60; i++)
            pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", payload[i]);
        ESP_LOGI(TAG, "WS90 reserved bytes[14+]: %s", hex);
    }

    return out->reading_count > 0;
}

// ─── Advertisement parser ────────────────────────────────────────────────────

static void parse_and_dispatch(const uint8_t *adv, uint8_t adv_len,
                                const uint8_t mac[6], int8_t rssi)
{
    const uint8_t *p   = adv;
    const uint8_t *end = adv + adv_len;

    const uint8_t *bthome_data   = nullptr;
    size_t         bthome_len    = 0;
    const uint8_t *ecowitt_data  = nullptr;
    size_t         ecowitt_len   = 0;
    char           name[32]      = {};

    while (p + 1 < end) {
        uint8_t        field_len  = p[0];
        if (field_len == 0) break;
        if (p + field_len >= end) break;

        uint8_t        field_type = p[1];
        const uint8_t *field_data = p + 2;
        size_t         data_len   = field_len - 1;

        switch (field_type) {
        case 0x16:  // Service Data – 16-bit UUID
            if (data_len >= 2) {
                uint16_t uuid = (uint16_t)(field_data[0] | (field_data[1] << 8));
                if (field_data[0] == BTHOME_UUID_LSB && field_data[1] == BTHOME_UUID_MSB) {
                    bthome_data = field_data + 2;
                    bthome_len  = data_len - 2;
                    ESP_LOGI(TAG, "BTHome svc-data from [%02X:%02X:%02X:%02X:%02X:%02X] len=%d dev_info=0x%02X",
                             mac[0],mac[1],mac[2],mac[3],mac[4],mac[5],
                             (int)bthome_len, bthome_len > 0 ? bthome_data[0] : 0xFF);
                } else if (field_data[0] == ECOWITT_UUID_LSB && field_data[1] == ECOWITT_UUID_MSB) {
                    ecowitt_data = field_data + 2;
                    ecowitt_len  = data_len - 2;
                } else {
                    ESP_LOGD(TAG, "Unknown UUID 0x%04X len=%d", uuid, (int)data_len);
                }
            }
            break;

        case 0x09:  // Complete Local Name
        case 0x08:  // Shortened Local Name
            if (data_len > 0 && name[0] == '\0') {
                size_t copy = data_len < sizeof(name) - 1 ? data_len : sizeof(name) - 1;
                memcpy(name, field_data, copy);
                name[copy] = '\0';
            }
            break;

        default:
            break;
        }

        p += field_len + 1;
    }

    sensor_data_t data = {};

    if (bthome_data && bthome_len > 0) {
        memcpy(data.mac, mac, 6);
        strncpy(data.name, name[0] ? name : "WS90", sizeof(data.name) - 1);
        if (bthome_parse(mac, bthome_data, bthome_len, &data) && s_callback
                && throttle_check(mac)) {
            s_adv_parsed++;
            s_callback(mac, &data);
        }
    } else if (ecowitt_data && ecowitt_len > 0) {
        if (parse_ecowitt_ws90(ecowitt_data, ecowitt_len, mac,
                               name[0] ? name : nullptr, &data) && s_callback
                && throttle_check(mac)) {
            s_adv_parsed++;
            s_callback(mac, &data);
        }
    }
}

// ─── NimBLE GAP event callback ───────────────────────────────────────────────

static int gap_event_cb(struct ble_gap_event *event, void * /*arg*/)
{
    if (event->type != BLE_GAP_EVENT_DISC) return 0;
    const struct ble_gap_disc_desc &d = event->disc;
    s_adv_total++;
    parse_and_dispatch(d.data, d.length_data, d.addr.val, d.rssi);
    return 0;
}

static void start_scan_internal(void)
{
    struct ble_gap_disc_params params = {};
    params.itvl              = 0x0140;  // 320ms interval (was 50ms)
    params.window            = 0x0040;  // 40ms window → ~12.5% duty cycle (was 60%)
    params.filter_policy     = BLE_HCI_SCAN_FILT_NO_WL;
    params.limited           = 0;
    params.passive           = 1;
    params.filter_duplicates = 0;

    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &params, gap_event_cb, nullptr);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "ble_gap_disc failed: %d", rc);
    } else {
        ESP_LOGI(TAG, "Passive BLE scan started (BTHome 0xFCD2 + Ecowitt 0xFE9F)");
        s_scanning = true;
    }
}

// ─── NimBLE host lifecycle ───────────────────────────────────────────────────

static void on_sync(void)  { if (s_scanning) start_scan_internal(); }
static void on_reset(int)  { s_scanning = false; }

static void ble_host_task(void *) { nimble_port_run(); nimble_port_freertos_deinit(); }

// ─── Watchdog ────────────────────────────────────────────────────────────────

static void watchdog_task(void *)
{
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        bool synced  = ble_hs_synced();
        bool scanning = ble_gap_disc_active();
        ESP_LOGI(TAG, "BLE watchdog: synced=%d scanning=%d adv_total=%lu parsed=%lu",
                 synced, scanning, s_adv_total, s_adv_parsed);
        if (s_scanning && synced && !scanning) {
            ESP_LOGW(TAG, "BLE scan stopped unexpectedly — restarting");
            start_scan_internal();
        }
    }
}

// ─── Public API ──────────────────────────────────────────────────────────────

esp_err_t ble_scanner_init(ble_scanner_cb_t callback)
{
    if (!callback) return ESP_ERR_INVALID_ARG;
    s_callback = callback;

    if (ble_hs_synced()) {
        ESP_LOGI(TAG, "BLE scanner init: NimBLE already running, skip init");
        s_owns_nimble = false;
    } else {
        ESP_LOGI(TAG, "BLE scanner init: starting NimBLE");
        nimble_port_init();
        ble_hs_cfg.sync_cb  = on_sync;
        ble_hs_cfg.reset_cb = on_reset;
        nimble_port_freertos_init(ble_host_task);
        s_owns_nimble = true;
    }

    ESP_LOGI(TAG, "BLE scanner initialised");
    return ESP_OK;
}

esp_err_t ble_scanner_start(void)
{
    s_scanning = true;
    if (ble_hs_synced()) start_scan_internal();
    xTaskCreate(watchdog_task, "ble_wdog", 2048, nullptr, 1, nullptr);
    return ESP_OK;
}

esp_err_t ble_scanner_stop(void)
{
    s_scanning = false;
    int rc = ble_gap_disc_cancel();
    return (rc == 0 || rc == BLE_HS_EALREADY) ? ESP_OK : ESP_FAIL;
}
