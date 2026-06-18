#include "sensor_registry.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_console.h"
#include <string.h>
#include <stdio.h>

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

void sensor_registry_clear(void)
{
    memset(s_entries, 0, sizeof(s_entries));
    s_count = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, NVS_KEY);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "Registry cleared");
}

bool sensor_registry_delete(const uint8_t mac[6])
{
    for (int i = 0; i < s_count; i++) {
        if (memcmp(s_entries[i].mac, mac, 6) == 0) {
            s_entries[i] = s_entries[--s_count];
            memset(&s_entries[s_count], 0, sizeof(registry_entry_t));
            sensor_registry_save();
            return true;
        }
    }
    return false;
}

// ─── Console command: sensor_reg ─────────────────────────────────────────────

static bool parse_mac(const char *s, uint8_t mac[6])
{
    return sscanf(s, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                  &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) == 6;
}

static int cmd_sensor_reg(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "list") == 0) {
        printf("Registered sensors (%d):\n", s_count);
        for (int i = 0; i < s_count; i++) {
            uint8_t *m = s_entries[i].mac;
            printf("  [%d] %02X:%02X:%02X:%02X:%02X:%02X  %s\n",
                   i, m[0],m[1],m[2],m[3],m[4],m[5], s_entries[i].name);
        }
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "clear") == 0) {
        sensor_registry_clear();
        printf("Registry cleared. Reboot to apply.\n");
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "del") == 0) {
        uint8_t mac[6];
        if (!parse_mac(argv[2], mac)) { printf("Invalid MAC\n"); return 1; }
        if (sensor_registry_delete(mac)) {
            printf("Deleted. Reboot to apply.\n");
        } else {
            printf("MAC not found in registry.\n");
        }
        return 0;
    }
    printf("Usage:\n");
    printf("  sensor_reg list          show all registered sensors\n");
    printf("  sensor_reg del <MAC>     remove one sensor\n");
    printf("  sensor_reg clear         remove all sensors\n");
    return 1;
}

void sensor_registry_register_console_command(void)
{
    const esp_console_cmd_t cmd = {
        .command  = "sensor_reg",
        .help     = "Manage the BLE sensor registry",
        .hint     = "list|del|clear",
        .func     = cmd_sensor_reg,
        .argtable = nullptr,
    };
    esp_console_cmd_register(&cmd);
}
