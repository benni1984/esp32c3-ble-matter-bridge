#include "shelly_poller.h"
#include "bthome.h"

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

#include <string.h>

static const char *TAG = "shelly_poller";

// WS90 Shelly chip MAC — same byte order as NimBLE delivers (LSB first)
static const uint8_t WS90_MAC[6] = {0x0D, 0x3D, 0x13, 0x6A, 0x4D, 0xFC};

static char               s_urls[2][128];
static int                s_url_count;
static shelly_poller_cb_t s_cb;

// 512 bytes is enough for the BLE.CloudRelay.ListInfos JSON response
static char s_resp_buf[512];
static int  s_resp_len;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        int copy = evt->data_len;
        if (s_resp_len + copy >= (int)sizeof(s_resp_buf) - 1)
            copy = sizeof(s_resp_buf) - 1 - s_resp_len;
        if (copy > 0) {
            memcpy(s_resp_buf + s_resp_len, evt->data, copy);
            s_resp_len += copy;
        }
    }
    return ESP_OK;
}

static bool poll_url(const char *url)
{
    s_resp_len = 0;
    memset(s_resp_buf, 0, sizeof(s_resp_buf));

    esp_http_client_config_t cfg = {};
    cfg.url             = url;
    cfg.event_handler   = http_event_handler;
    cfg.timeout_ms      = 5000;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP failed: %s", esp_err_to_name(err));
        return false;
    }

    cJSON *root = cJSON_ParseWithLength(s_resp_buf, s_resp_len);
    if (!root) { ESP_LOGW(TAG, "JSON parse failed"); return false; }

    cJSON *devices = cJSON_GetObjectItem(root, "devices");
    if (!cJSON_IsArray(devices) || cJSON_GetArraySize(devices) == 0) {
        cJSON_Delete(root); return false;
    }

    cJSON *dev_obj = cJSON_GetArrayItem(devices, 0);
    cJSON *dev = cJSON_GetObjectItem(dev_obj, "fc:4d:6a:13:3d:0d");
    if (!dev) { cJSON_Delete(root); return false; }

    cJSON *sdata = cJSON_GetObjectItem(dev, "sdata");
    cJSON *fcd2  = cJSON_GetObjectItem(sdata, "fcd2");
    if (!cJSON_IsString(fcd2)) { cJSON_Delete(root); return false; }

    const char *b64 = fcd2->valuestring;
    uint8_t payload[64];
    size_t out_len = 0;
    int rc = mbedtls_base64_decode(payload, sizeof(payload), &out_len,
                                    (const uint8_t *)b64, strlen(b64));
    cJSON_Delete(root);

    if (rc != 0) { ESP_LOGW(TAG, "Base64 decode failed: %d", rc); return false; }

    sensor_data_t data = {};
    if (!bthome_parse(WS90_MAC, payload, out_len, &data)) {
        ESP_LOGW(TAG, "BTHome parse failed"); return false;
    }

    snprintf(data.name, sizeof(data.name), "WS90");
    ESP_LOGI(TAG, "WS90 poll: %d readings", data.reading_count);
    s_cb(WS90_MAC, &data);
    return true;
}

static void poll_once(void)
{
    for (int i = 0; i < s_url_count; i++) {
        if (poll_url(s_urls[i])) return;
        if (i + 1 < s_url_count) ESP_LOGW(TAG, "Trying fallback Shelly...");
    }
    ESP_LOGE(TAG, "All Shelly devices unreachable");
}

static bool has_ipv4(void)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta) return false;
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(sta, &info) != ESP_OK) return false;
    return info.ip.addr != 0;
}

static void poller_task(void *)
{
    // Wait for a real IPv4 address before making HTTP requests
    while (!has_ipv4()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "WiFi up — starting poll loop");

    while (true) {
        poll_once();
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

esp_err_t shelly_poller_init(const char *shelly_ip, shelly_poller_cb_t cb)
{
    s_url_count = 0;
    s_cb = cb;
    snprintf(s_urls[s_url_count++], 128, "http://%s/rpc/BLE.CloudRelay.ListInfos", shelly_ip);
    ESP_LOGI(TAG, "Shelly poller primary: %s", s_urls[0]);
    return ESP_OK;
}

esp_err_t shelly_poller_add_fallback(const char *shelly_ip)
{
    if (s_url_count >= 2) return ESP_ERR_NO_MEM;
    snprintf(s_urls[s_url_count++], 128, "http://%s/rpc/BLE.CloudRelay.ListInfos", shelly_ip);
    ESP_LOGI(TAG, "Shelly poller fallback: %s", s_urls[s_url_count - 1]);
    return ESP_OK;
}

void shelly_poller_start(void)
{
    xTaskCreate(poller_task, "shelly_poll", 4096, nullptr, 1, nullptr);
}
