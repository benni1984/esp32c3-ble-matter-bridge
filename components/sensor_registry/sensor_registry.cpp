#include "sensor_registry.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

static const char *TAG      = "sensor_registry";
static const char *NVS_NS   = "ble_bridge";      // NVS namespace
static const char *NVS_KEY  = "registry";         // NVS key for the blob

// ─── Internal table ──────────────────────────────────────────────────────────

static registry_entry_t s_entries[REGISTRY_MAX_SENSORS];
static int              s_count = 0;

// ─── NVS helpers ─────────────────────────────────────────────────────────────

esp_err_t sensor_registry_save(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NS, NVS_READWRITE, &handle);
    if (ret != ESP_OK) return ret;

    ret = nvs_set_blob(handle, NVS_KEY, s_entries,
                       sizeof(registry_entry_t) * s_count);
    if (ret == ESP_OK) ret = nvs_commit(handle);
    nvs_close(handle);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Registry saved (%d sensors)", s_count);
    } else {
        ESP_LOGE(TAG, "NVS save failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t registry_load(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NS, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No persisted registry found – starting fresh");
        return ESP_OK;
    }
    if (ret != ESP_OK) return ret;

    size_t required = sizeof(s_entries);
    ret = nvs_get_blob(handle, NVS_KEY, s_entries, &required);
    nvs_close(handle);

    if (ret == ESP_OK) {
        s_count = (int)(required / sizeof(registry_entry_t));
        ESP_LOGI(TAG, "Loaded %d sensor(s) from NVS", s_count);
    } else if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ret = ESP_OK;  // first boot with no registry – fine
    } else {
        ESP_LOGE(TAG, "NVS load failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

// ─── Public API ──────────────────────────────────────────────────────────────

esp_err_t sensor_registry_init(void)
{
    memset(s_entries, 0, sizeof(s_entries));
    s_count = 0;
    return registry_load();
}

registry_entry_t *sensor_registry_get_or_create(const uint8_t mac[6],
                                                  const char   *name)
{
    // Search for existing entry
    for (int i = 0; i < s_count; i++) {
        if (memcmp(s_entries[i].mac, mac, 6) == 0) {
            // Update name if we now have one and didn't before
            if (name && name[0] && s_entries[i].name[0] == '\0') {
                strncpy(s_entries[i].name, name, sizeof(s_entries[i].name) - 1);
            }
            return &s_entries[i];
        }
    }

    // New sensor
    if (s_count >= REGISTRY_MAX_SENSORS) {
        ESP_LOGW(TAG, "Registry full (%d sensors) – ignoring new sensor", REGISTRY_MAX_SENSORS);
        return nullptr;
    }

    registry_entry_t *e = &s_entries[s_count++];
    memset(e, 0, sizeof(*e));
    memcpy(e->mac, mac, 6);
    if (name && name[0]) {
        strncpy(e->name, name, sizeof(e->name) - 1);
    } else {
        snprintf(e->name, sizeof(e->name), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    e->active = true;

    ESP_LOGI(TAG, "New sensor registered: %s", e->name);
    sensor_registry_save();
    return e;
}

int sensor_registry_count(void) { return s_count; }

registry_entry_t *sensor_registry_get(int i)
{
    if (i < 0 || i >= s_count) return nullptr;
    return &s_entries[i];
}
