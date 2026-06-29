#include "matter_bridge.h"
#include "mac_commissioning_data_provider.h"
#include "bthome.h"

#include <esp_matter.h>
#include <esp_matter_endpoint.h>
#include <esp_matter_cluster.h>
#include <esp_matter_attribute_utils.h>
#include <esp_matter_console.h>
#include <esp_matter_ota.h>

#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
#include <credentials/FabricTable.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/CommissionableDataProvider.h>
#include <setup_payload/QRCodeSetupPayloadGenerator.h>
#include <setup_payload/ManualSetupPayloadGenerator.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <string>
#include <math.h>

// Rate-limit Matter attribute updates per MAC: at most once every UPDATE_INTERVAL_MS
#define UPDATE_INTERVAL_MS 10000
#define MAX_RATE_TRACKED   8
static struct { uint8_t mac[6]; uint32_t last_ms; } s_update_rate[MAX_RATE_TRACKED];

static bool update_rate_ok(const uint8_t mac[6])
{
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    for (int i = 0; i < MAX_RATE_TRACKED; i++) {
        if (memcmp(s_update_rate[i].mac, mac, 6) == 0) {
            if ((now - s_update_rate[i].last_ms) < UPDATE_INTERVAL_MS) return false;
            s_update_rate[i].last_ms = now;
            return true;
        }
    }
    int oldest = 0;
    for (int i = 1; i < MAX_RATE_TRACKED; i++) {
        if (s_update_rate[i].last_ms < s_update_rate[oldest].last_ms) oldest = i;
    }
    memcpy(s_update_rate[oldest].mac, mac, 6);
    s_update_rate[oldest].last_ms = now;
    return true;
}

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::endpoint;
using namespace chip;
using namespace chip::DeviceLayer;

static const char *TAG = "matter_bridge";

static node_t                          *s_node       = nullptr;
static matter_bridge_commissioned_cb_t  s_on_commissioned = nullptr;
static MacCommissionableDataProvider    s_cdp;

// ─── Matter attribute callback ────────────────────────────────────────────────

static esp_err_t app_attribute_cb(callback_type_t type,
                                   uint16_t endpoint_id,
                                   uint32_t cluster_id,
                                   uint32_t attribute_id,
                                   esp_matter_attr_val_t *val,
                                   void * /*priv_data*/)
{
    // All our endpoints are read-only sensors; writes are rejected.
    if (type == PRE_UPDATE) {
        ESP_LOGD(TAG, "Attribute write ep=%d cluster=0x%04lX attr=0x%04lX",
                 endpoint_id, cluster_id, attribute_id);
    }
    return ESP_OK;
}

// ─── Matter device event callback ─────────────────────────────────────────────

static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
    case DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Matter commissioning complete");
        if (s_on_commissioned) s_on_commissioned();
        break;

    case DeviceEventType::kFabricCommitted:
        ESP_LOGI(TAG, "Fabric committed");
        break;

    case DeviceEventType::kWiFiConnectivityChange:
        if (event->WiFiConnectivityChange.Result == kConnectivity_Established) {
            ESP_LOGI(TAG, "WiFi connected");
        }
        break;

    case DeviceEventType::kInterfaceIpAddressChanged:
        ESP_LOGI(TAG, "IP address assigned");
        break;

    default:
        break;
    }
}

// ─── Endpoint factory ─────────────────────────────────────────────────────────

/**
 * Create a Matter endpoint for a single sensor type and store its ID
 * in the registry entry.
 */
static esp_err_t create_sensor_endpoint(registry_entry_t *entry,
                                         sensor_type_t     type,
                                         float             initial_value)
{
    endpoint_t *ep = nullptr;

    switch (type) {

    case SENSOR_TEMPERATURE: {
        temperature_sensor::config_t cfg;
        cfg.temperature_measurement.measured_value =
            (int16_t)(initial_value * 100.0f);  // Matter: hundredths of °C
        cfg.temperature_measurement.min_measured_value = -4000;
        cfg.temperature_measurement.max_measured_value =  8500;
        ep = temperature_sensor::create(s_node, &cfg,
                                        ENDPOINT_FLAG_NONE, nullptr);
        break;
    }

    case SENSOR_HUMIDITY: {
        humidity_sensor::config_t cfg;
        cfg.relative_humidity_measurement.measured_value =
            (uint16_t)(initial_value * 100.0f);  // Matter: hundredths of %
        cfg.relative_humidity_measurement.min_measured_value = (uint16_t)0;
        cfg.relative_humidity_measurement.max_measured_value = (uint16_t)10000;
        ep = humidity_sensor::create(s_node, &cfg,
                                     ENDPOINT_FLAG_NONE, nullptr);
        break;
    }

    case SENSOR_PRESSURE: {
        // Matter: Pressure Measurement in units of 0.1 kPa; hPa * 1 = 0.1 kPa units
        pressure_sensor::config_t cfg;
        cfg.pressure_measurement.measured_value    = (int16_t)(initial_value);
        cfg.pressure_measurement.min_measured_value = (int16_t)0;
        cfg.pressure_measurement.max_measured_value = (int16_t)12000;  // 1200 hPa
        ep = pressure_sensor::create(s_node, &cfg,
                                     ENDPOINT_FLAG_NONE, nullptr);
        break;
    }

    case SENSOR_ILLUMINANCE: {
        light_sensor::config_t cfg;
        // Matter illuminance: 10000 * log10(lux) + 1
        float lux = initial_value > 0 ? initial_value : 1.0f;
        cfg.illuminance_measurement.measured_value =
            (uint16_t)(10000.0f * log10f(lux) + 1.0f);
        cfg.illuminance_measurement.min_measured_value = (uint16_t)1;
        cfg.illuminance_measurement.max_measured_value = (uint16_t)65533;
        ep = light_sensor::create(s_node, &cfg,
                                   ENDPOINT_FLAG_NONE, nullptr);
        break;
    }

    // Wind speed, wind direction, rain, UV index, battery, gust:
    // Matter 1.3 has no dedicated cluster; mapped to FlowMeasurement.
    // min/max must satisfy: min < max (0,0 would violate this and crash Matter.js).
    case SENSOR_WIND_SPEED:
    case SENSOR_WIND_SPEED_GUST:
    case SENSOR_WIND_DIRECTION:
    case SENSOR_RAIN:
    case SENSOR_UV_INDEX:
    case SENSOR_BATTERY: {
        flow_sensor::config_t cfg;
        cfg.flow_measurement.measured_value     = (uint16_t)(initial_value * 10.0f);
        cfg.flow_measurement.min_measured_value = (uint16_t)0;
        cfg.flow_measurement.max_measured_value = (uint16_t)65534;
        ep = flow_sensor::create(s_node, &cfg,
                                  ENDPOINT_FLAG_NONE, nullptr);
        break;
    }

    default:
        ESP_LOGW(TAG, "No Matter mapping for sensor type %d", type);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (!ep) {
        ESP_LOGE(TAG, "Failed to create endpoint for type %d", type);
        return ESP_FAIL;
    }

    // Give the endpoint a readable node label
    char label[48];
    snprintf(label, sizeof(label), "%s %s",
             entry->name, sensor_type_name(type));
    esp_matter_attr_val_t label_val = esp_matter_char_str(label, strlen(label));
    attribute::update(endpoint::get_id(ep),
                      chip::app::Clusters::BridgedDeviceBasicInformation::Id,
                      chip::app::Clusters::BridgedDeviceBasicInformation::Attributes::NodeLabel::Id,
                      &label_val);

    entry->matter_endpoint_id[type] = endpoint::get_id(ep);
    ESP_LOGI(TAG, "Created endpoint %d for %s / %s",
             entry->matter_endpoint_id[type], entry->name, sensor_type_name(type));
    return ESP_OK;
}

// ─── Public API ──────────────────────────────────────────────────────────────

esp_err_t matter_bridge_init(matter_bridge_commissioned_cb_t on_commissioned)
{
    s_on_commissioned = on_commissioned;

    // Register MAC-derived commissioning data so every device gets a unique QR code
    chip::DeviceLayer::SetCommissionableDataProvider(&s_cdp);

    node::config_t node_config;
    // 3rd arg is the identify callback (not the device-event callback).
    // We have no identify action on a sensor bridge, so pass nullptr.
    s_node = node::create(&node_config, app_attribute_cb, nullptr);
    if (!s_node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return ESP_FAIL;
    }

    // Restore previously discovered sensors so they re-appear after a reboot.
    int count = sensor_registry_count();
    for (int i = 0; i < count; i++) {
        registry_entry_t *e = sensor_registry_get(i);
        if (!e || !e->active) continue;
        for (int t = 0; t < SENSOR_TYPE_COUNT; t++) {
            if (e->matter_endpoint_id[t] != 0) {
                // The endpoint ID is non-zero → it was previously created.
                // Re-create the endpoint with a neutral initial value (0).
                e->matter_endpoint_id[t] = 0;  // reset so create_sensor_endpoint re-creates it
                create_sensor_endpoint(e, (sensor_type_t)t, 0.0f);
            }
        }
    }

    ESP_LOGI(TAG, "Matter bridge initialised (%d sensor(s) restored)", count);
    return ESP_OK;
}

esp_err_t matter_bridge_start(void)
{
    esp_err_t ret = esp_matter::start(app_event_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_matter::start failed: %s", esp_err_to_name(ret));
        return ret;
    }

#if CONFIG_ENABLE_CHIP_SHELL
    esp_matter::console::diagnostics_register_commands();
    esp_matter::console::wifi_register_commands();
    esp_matter::console::factoryreset_register_commands();
    bthome_register_console_command();             // must be before console::init()
    sensor_registry_register_console_command();    // must be before console::init()
    esp_matter::console::init();
#endif

    return ESP_OK;
}

void matter_bridge_update(const uint8_t mac[6], const sensor_data_t *data)
{
    if (!update_rate_ok(mac)) return;

    registry_entry_t *entry = sensor_registry_get_or_create(mac, data->name);
    if (!entry) return;

    for (int i = 0; i < data->reading_count; i++) {
        const sensor_reading_t &r    = data->readings[i];
        sensor_type_t           type = r.type;
        uint16_t ep_id               = entry->matter_endpoint_id[type];

        if (ep_id == 0) {
            // First time we see this measurement type → create an endpoint.
            create_sensor_endpoint(entry, type, r.value);
            sensor_registry_save();
            continue;
        }

        // Update the existing endpoint's measured-value attribute.
        esp_matter_attr_val_t val;
        uint32_t cluster_id, attr_id;

        // All Matter measurement clusters use nullable attribute types.
        switch (type) {
        case SENSOR_TEMPERATURE:
            cluster_id = chip::app::Clusters::TemperatureMeasurement::Id;
            attr_id    = chip::app::Clusters::TemperatureMeasurement::Attributes::MeasuredValue::Id;
            val        = esp_matter_nullable_int16((int16_t)(r.value * 100.0f));
            break;
        case SENSOR_HUMIDITY:
            cluster_id = chip::app::Clusters::RelativeHumidityMeasurement::Id;
            attr_id    = chip::app::Clusters::RelativeHumidityMeasurement::Attributes::MeasuredValue::Id;
            val        = esp_matter_nullable_uint16((uint16_t)(r.value * 100.0f));
            break;
        case SENSOR_PRESSURE:
            cluster_id = chip::app::Clusters::PressureMeasurement::Id;
            attr_id    = chip::app::Clusters::PressureMeasurement::Attributes::MeasuredValue::Id;
            val        = esp_matter_nullable_int16((int16_t)(r.value));
            break;
        case SENSOR_ILLUMINANCE: {
            float lux  = r.value > 0 ? r.value : 1.0f;
            cluster_id = chip::app::Clusters::IlluminanceMeasurement::Id;
            attr_id    = chip::app::Clusters::IlluminanceMeasurement::Attributes::MeasuredValue::Id;
            val        = esp_matter_nullable_uint16((uint16_t)(10000.0f * log10f(lux) + 1.0f));
            break;
        }
        default:
            // Generic: flow cluster, value * 10
            cluster_id = chip::app::Clusters::FlowMeasurement::Id;
            attr_id    = chip::app::Clusters::FlowMeasurement::Attributes::MeasuredValue::Id;
            val        = esp_matter_nullable_uint16((uint16_t)(r.value * 10.0f));
            break;
        }

        attribute::update(ep_id, cluster_id, attr_id, &val);
        ESP_LOGD(TAG, "Updated ep %d (%s) = %.2f",
                 ep_id, sensor_type_name(type), r.value);
    }
}

void matter_bridge_print_pairing_info(void)
{
    // Build SetupPayload from CommissionableDataProvider (passcode + discriminator)
    // and compile-time VID/PID from CHIPProjectConfig.h.
    using namespace chip;

    uint32_t passcode    = 0;
    uint16_t discriminator = 0;

    auto * provider = DeviceLayer::GetCommissionableDataProvider();
    if (!provider ||
        provider->GetSetupPasscode(passcode)      != CHIP_NO_ERROR ||
        provider->GetSetupDiscriminator(discriminator) != CHIP_NO_ERROR) {
        ESP_LOGW(TAG, "Could not read commissioning credentials – QR code unavailable");
        return;
    }

    SetupPayload payload;
    payload.setUpPINCode        = passcode;
    payload.discriminator.SetLongValue(discriminator);
    payload.commissioningFlow   = CommissioningFlow::kStandard;
    // RendezvousInformationFlag has no operator|; use BitFlags API.
    RendezvousInformationFlags rendezvousFlags(RendezvousInformationFlag::kBLE);
    rendezvousFlags.Set(RendezvousInformationFlag::kOnNetwork);
    payload.rendezvousInformation.SetValue(rendezvousFlags);
    payload.vendorID  = CHIP_DEVICE_CONFIG_DEVICE_VENDOR_ID;
    payload.productID = CHIP_DEVICE_CONFIG_DEVICE_PRODUCT_ID;

    // QR code — payloadBase38Representation() takes std::string& in this SDK version.
    std::string qr_string;
    if (QRCodeSetupPayloadGenerator(payload).payloadBase38Representation(qr_string) == CHIP_NO_ERROR) {
        ESP_LOGI(TAG, "──────────────────────────────────────────");
        ESP_LOGI(TAG, "Matter QR code data: %s", qr_string.c_str());
        ESP_LOGI(TAG, "Scan with Apple Home or Home Assistant");
    }

    // Manual code (11-digit decimal string)
    char manual_buf[32] = {};
    MutableCharSpan manualSpan(manual_buf, sizeof(manual_buf) - 1);
    if (ManualSetupPayloadGenerator(payload).payloadDecimalStringRepresentation(manualSpan) == CHIP_NO_ERROR) {
        ESP_LOGI(TAG, "Manual pairing code: %s", manual_buf);
        ESP_LOGI(TAG, "──────────────────────────────────────────");
    }
}

bool matter_bridge_is_commissioned(void)
{
    return Server::GetInstance().GetFabricTable().FabricCount() > 0;
}
