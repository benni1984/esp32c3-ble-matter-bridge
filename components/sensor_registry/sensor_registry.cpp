#include "sensor_registry.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_console.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG           = "sensor_registry";
static const char *NVS_NS        = "ble_bridge";      // NVS namespace
static const char *NVS_KEY       = "registry";         // NVS key for the blob
static const char *NVS_EPOCH_KEY = "boot_epoch";        // NVS key for the boot counter

// ─── Internal table ──────────────────────────────────────────────────────────

static registry_entry_t s_entries[REGISTRY_MAX_SENSORS];
static int              s_count = 0;
static uint32_t         s_boot_epoch = 0;

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

// Bumps and persists the boot-epoch counter — this firmware has no RTC/NTP,
// so this ordinal counter (incremented once per boot) is the only "how long
// ago" signal available. Used for LRU eviction and the 'stale' report.
static void bump_boot_epoch(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    uint32_t stored = 0;
    nvs_get_u32(h, NVS_EPOCH_KEY, &stored);
    s_boot_epoch = stored + 1;
    nvs_set_u32(h, NVS_EPOCH_KEY, s_boot_epoch);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Boot epoch: %u", (unsigned)s_boot_epoch);
}

uint32_t sensor_registry_boot_epoch(void) { return s_boot_epoch; }

esp_err_t sensor_registry_init(void)
{
    memset(s_entries, 0, sizeof(s_entries));
    s_count = 0;
    bump_boot_epoch();
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
            s_entries[i].last_seen_epoch = s_boot_epoch;
            return &s_entries[i];
        }
    }

    // New sensor — evict the least-recently-seen entry if the table is full
    // instead of just rejecting it. A device that's stopped broadcasting
    // naturally has the lowest last_seen_epoch and yields its slot; a
    // persistently-broadcasting unwanted device never gets evicted this way
    // (it's always "recently seen" too) — that's what 'sensor_reg block' is
    // for (see cmd_sensor_reg()).
    int slot;
    if (s_count >= REGISTRY_MAX_SENSORS) {
        int victim = 0;
        for (int i = 1; i < s_count; i++) {
            if (s_entries[i].last_seen_epoch < s_entries[victim].last_seen_epoch) victim = i;
        }
        ESP_LOGW(TAG, "Registry full — evicting %s (%02X:%02X:%02X:%02X:%02X:%02X), "
                      "last seen %u boot(s) ago, to make room for %02X:%02X:%02X:%02X:%02X:%02X",
                 s_entries[victim].name,
                 s_entries[victim].mac[0], s_entries[victim].mac[1], s_entries[victim].mac[2],
                 s_entries[victim].mac[3], s_entries[victim].mac[4], s_entries[victim].mac[5],
                 (unsigned)(s_boot_epoch - s_entries[victim].last_seen_epoch),
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        slot = victim;
    } else {
        slot = s_count++;
    }

    registry_entry_t *e = &s_entries[slot];
    memset(e, 0, sizeof(*e));
    memcpy(e->mac, mac, 6);
    if (name && name[0]) {
        strncpy(e->name, name, sizeof(e->name) - 1);
    } else {
        snprintf(e->name, sizeof(e->name), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    e->active = true;
    e->last_seen_epoch = s_boot_epoch;

    ESP_LOGI(TAG, "New sensor registered: %s", e->name);
    sensor_registry_save();
    return e;
}

bool sensor_registry_mark_known(registry_entry_t *entry, sensor_type_t t)
{
    if (!entry || t < 0 || t >= SENSOR_TYPE_COUNT) return false;
    uint16_t bit = (uint16_t)(1u << t);
    if (entry->known_type_mask & bit) return false;
    entry->known_type_mask |= bit;
    return true;
}

esp_err_t sensor_registry_set_name(const uint8_t mac[6], const char *name)
{
    for (int i = 0; i < s_count; i++) {
        if (memcmp(s_entries[i].mac, mac, 6) == 0) {
            strncpy(s_entries[i].name, name, sizeof(s_entries[i].name) - 1);
            s_entries[i].name[sizeof(s_entries[i].name) - 1] = '\0';
            return sensor_registry_save();
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t sensor_registry_block(const uint8_t mac[6], bool blocked)
{
    for (int i = 0; i < s_count; i++) {
        if (memcmp(s_entries[i].mac, mac, 6) == 0) {
            s_entries[i].active = !blocked;
            return sensor_registry_save();
        }
    }
    return ESP_ERR_NOT_FOUND;
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
            printf("  [%d] %02X:%02X:%02X:%02X:%02X:%02X  %s%s\n",
                   i, m[0],m[1],m[2],m[3],m[4],m[5], s_entries[i].name,
                   s_entries[i].active ? "" : " [blocked]");
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
    if (argc >= 4 && strcmp(argv[1], "name") == 0) {
        uint8_t mac[6];
        if (!parse_mac(argv[2], mac)) { printf("Invalid MAC\n"); return 1; }
        if (sensor_registry_set_name(mac, argv[3]) == ESP_OK) {
            printf("Name set.\n");
        } else {
            printf("MAC not found in registry.\n");
        }
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "block") == 0) {
        uint8_t mac[6];
        if (!parse_mac(argv[2], mac)) { printf("Invalid MAC\n"); return 1; }
        if (sensor_registry_block(mac, true) == ESP_OK) {
            printf("Blocked. Reboot to apply (its endpoints stay until then).\n");
        } else {
            printf("MAC not found in registry.\n");
        }
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "unblock") == 0) {
        uint8_t mac[6];
        if (!parse_mac(argv[2], mac)) { printf("Invalid MAC\n"); return 1; }
        if (sensor_registry_block(mac, false) == ESP_OK) {
            printf("Unblocked. Reboot to apply.\n");
        } else {
            printf("MAC not found in registry.\n");
        }
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "stale") == 0) {
        int threshold = 10;
        if (argc >= 3) threshold = atoi(argv[2]);
        printf("Sensors stale for >= %d boot(s):\n", threshold);
        for (int i = 0; i < s_count; i++) {
            uint32_t age = s_boot_epoch - s_entries[i].last_seen_epoch;
            if ((int)age < threshold) continue;
            uint8_t *m = s_entries[i].mac;
            printf("  [%d] %02X:%02X:%02X:%02X:%02X:%02X  %s  last seen %u boot(s) ago\n",
                   i, m[0],m[1],m[2],m[3],m[4],m[5], s_entries[i].name, (unsigned)age);
        }
        return 0;
    }
    printf("Usage:\n");
    printf("  sensor_reg list                show all registered sensors\n");
    printf("  sensor_reg name <MAC> <name>   set a friendly name\n");
    printf("  sensor_reg block <MAC>          ignore this device's readings\n");
    printf("  sensor_reg unblock <MAC>        re-enable a blocked device\n");
    printf("  sensor_reg stale [N]            list sensors not seen in N boots (default 10)\n");
    printf("  sensor_reg del <MAC>            remove one sensor\n");
    printf("  sensor_reg clear                remove all sensors\n");
    return 1;
}

void sensor_registry_register_console_command(void)
{
    const esp_console_cmd_t cmd = {
        .command  = "sensor_reg",
        .help     = "Manage the BLE sensor registry",
        .hint     = "list|name|block|unblock|stale|del|clear",
        .func     = cmd_sensor_reg,
        .argtable = nullptr,
    };
    esp_console_cmd_register(&cmd);
}
