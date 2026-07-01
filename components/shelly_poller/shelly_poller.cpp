#include "shelly_poller.h"
#include "bthome.h"

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "mbedtls/base64.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "shelly_poller";

// WS90 Shelly chip MAC — same byte order as NimBLE delivers (LSB first)
static const uint8_t WS90_MAC[6] = {0x0D, 0x3D, 0x13, 0x6A, 0x4D, 0xFC};

#define MAX_URLS 4
static char               s_urls[MAX_URLS][128];
static int                s_url_count;
static shelly_poller_cb_t s_cb;

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
    cfg.timeout_ms      = 1000;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP failed %s: %s", url, esp_err_to_name(err));
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
    ESP_LOGI(TAG, "WS90 poll OK: %d readings", data.reading_count);
    s_cb(WS90_MAC, &data);
    return true;
}

static void poll_once(void)
{
    for (int i = 0; i < s_url_count; i++) {
        if (poll_url(s_urls[i])) return;
        if (i + 1 < s_url_count) ESP_LOGW(TAG, "Trying next Shelly...");
    }
    ESP_LOGE(TAG, "All Shelly devices unreachable");
}

static EventGroupHandle_t s_ip_event_group;
#define IP_READY_BIT BIT0

static void ip_event_cb(void *, esp_event_base_t, int32_t, void *)
{
    xEventGroupSetBits(s_ip_event_group, IP_READY_BIT);
}

static void poller_task(void *)
{
    s_ip_event_group = xEventGroupCreate();
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_cb, NULL);

    // If WiFi already up when called (e.g. reboot with existing credentials), skip wait.
    esp_netif_t *netif = esp_netif_get_default_netif();
    if (netif) {
        esp_netif_ip_info_t ip_info = {};
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            xEventGroupSetBits(s_ip_event_group, IP_READY_BIT);
        }
    }

    xEventGroupWaitBits(s_ip_event_group, IP_READY_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_cb);
    vEventGroupDelete(s_ip_event_group);

    // Wait for CASE to complete before opening HTTP connections.
    // kCommissioningComplete fires when CommissioningComplete cluster is received
    // over BLE — BEFORE matter-server initiates CASE over UDP. BLE coexistence
    // with WiFi causes UDP drops; adding HTTP load at this moment exhausts the
    // PacketBuffer pool and blocks CASE_Sigma2. 90s covers the full fail-safe window.
    ESP_LOGI(TAG, "WiFi up — waiting 90 s for CASE to complete before first poll");
    vTaskDelay(pdMS_TO_TICKS(90000));
    ESP_LOGI(TAG, "Starting Shelly poll loop (%d URL(s))", s_url_count);

    while (true) {
        poll_once();
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

esp_err_t shelly_poller_init(shelly_poller_cb_t cb)
{
    s_url_count = 0;
    s_cb = cb;
    return ESP_OK;
}

esp_err_t shelly_poller_add_url(const char *shelly_ip)
{
    if (s_url_count >= MAX_URLS) return ESP_ERR_NO_MEM;
    snprintf(s_urls[s_url_count++], sizeof(s_urls[0]),
             "http://%s/rpc/BLE.CloudRelay.ListInfos", shelly_ip);
    ESP_LOGI(TAG, "Shelly URL added: %s", s_urls[s_url_count - 1]);
    return ESP_OK;
}

void shelly_poller_start(void)
{
    static bool s_started = false;
    if (s_started) {
        ESP_LOGI(TAG, "Poller already running");
        return;
    }
    s_started = true;
    xTaskCreate(poller_task, "shelly_poll", 4096, nullptr, 1, nullptr);
}
