#include "ble_scanner.h"

#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"

#include <string.h>

static const char *TAG = "ble_scanner";

// BTHome v2 service UUID: 0xFCD2, stored little-endian in advertisements
static constexpr uint8_t BTHOME_UUID_LSB = 0xD2;
static constexpr uint8_t BTHOME_UUID_MSB = 0xFC;

static ble_scanner_cb_t s_callback = nullptr;
static bool             s_scanning  = false;

// ─── Advertisement parser ────────────────────────────────────────────────────

/**
 * Walk through raw BLE advertisement data and look for:
 *   - AD type 0x16 (Service Data, 16-bit UUID) with UUID == 0xFCD2  → BTHome payload
 *   - AD type 0x09 / 0x08 (Complete / Shortened Local Name)
 */
static void parse_and_dispatch(const uint8_t *adv, uint8_t adv_len,
                                const uint8_t mac[6], int8_t rssi)
{
    const uint8_t *p   = adv;
    const uint8_t *end = adv + adv_len;

    const uint8_t *bthome_payload = nullptr;
    size_t         bthome_len     = 0;
    char           name[32]       = {};

    while (p + 1 < end) {
        uint8_t        field_len  = p[0];
        if (field_len == 0) break;
        if (p + field_len >= end) break;

        uint8_t        field_type = p[1];
        const uint8_t *field_data = p + 2;
        size_t         data_len   = field_len - 1;

        switch (field_type) {
        case 0x16:  // Service Data – 16-bit UUID
            if (data_len >= 2 &&
                field_data[0] == BTHOME_UUID_LSB &&
                field_data[1] == BTHOME_UUID_MSB)
            {
                bthome_payload = field_data + 2;
                bthome_len     = data_len - 2;
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

    if (bthome_payload && bthome_len > 0 && s_callback) {
        s_callback(mac, name[0] ? name : nullptr, bthome_payload, bthome_len, rssi);
    }
}

// ─── NimBLE GAP event callback ───────────────────────────────────────────────

static int gap_event_cb(struct ble_gap_event *event, void * /*arg*/)
{
    if (event->type != BLE_GAP_EVENT_DISC) return 0;

    const struct ble_gap_disc_desc &d = event->disc;
    parse_and_dispatch(d.data, d.length_data, d.addr.val, d.rssi);
    return 0;
}

static void start_scan_internal(void)
{
    struct ble_gap_disc_params params = {};
    params.itvl              = 0x0050;  // 50 ms scan interval
    params.window            = 0x0030;  // 30 ms scan window
    params.filter_policy     = BLE_HCI_SCAN_FILT_NO_WL;
    params.limited           = 0;
    params.passive           = 1;       // passive — no scan requests sent
    params.filter_duplicates = 0;       // receive repeated ads (sensor updates)

    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &params, gap_event_cb, nullptr);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "ble_gap_disc failed: %d", rc);
    } else {
        ESP_LOGI(TAG, "Passive BLE scan started (BTHome / UUID 0xFCD2)");
        s_scanning = true;
    }
}

// ─── NimBLE host lifecycle ───────────────────────────────────────────────────

static void on_sync(void)
{
    // BLE host is ready.  If scanning was requested before sync, start now.
    if (s_scanning) {
        start_scan_internal();
    }
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset: reason=%d", reason);
    s_scanning = false;
}

static void ble_host_task(void * /*param*/)
{
    ESP_LOGI(TAG, "BLE host task started");
    nimble_port_run();              // blocks until nimble_port_stop()
    nimble_port_freertos_deinit();
}

// ─── Public API ──────────────────────────────────────────────────────────────

esp_err_t ble_scanner_init(ble_scanner_cb_t callback)
{
    if (!callback) return ESP_ERR_INVALID_ARG;
    s_callback = callback;

    // In IDF v5.x nimble_port_init() handles BT controller init internally.
    // esp_nimble_hci_init() is a v4.x-only wrapper and must not be called.
    nimble_port_init();
    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    nimble_port_freertos_init(ble_host_task);
    ESP_LOGI(TAG, "BLE scanner initialised");
    return ESP_OK;
}

esp_err_t ble_scanner_start(void)
{
    // If the host is already synced, start immediately; otherwise on_sync() will.
    s_scanning = true;
    if (ble_hs_synced()) {
        start_scan_internal();
    }
    return ESP_OK;
}

esp_err_t ble_scanner_stop(void)
{
    s_scanning = false;
    int rc = ble_gap_disc_cancel();
    return (rc == 0 || rc == BLE_HS_EALREADY) ? ESP_OK : ESP_FAIL;
}
