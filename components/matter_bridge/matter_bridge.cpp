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
#include "nvs.h"
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
static endpoint_t                      *s_aggregator = nullptr;
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

// NVS namespace + key for the "CommissioningComplete was received" flag.
// Written only on kCommissioningComplete. Factory reset clears it along with
// everything else. Used to detect stale partial commissionings (AddNOC written
// to NVS, device rebooted, but CommissioningComplete was never received) which
// leave the device with FabricCount>0 but no working CASE session — causing it
// to boot in operational mode with no BLE advertising.
static const char *k_bridge_ns  = "bridge-state";
static const char *k_commissioned = "commissioned";

static void mark_commissioned(void)
{
    nvs_handle_t h;
    if (nvs_open(k_bridge_ns, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, k_commissioned, 1);
        nvs_commit(h);
        nvs_close(h);
    }
}

static bool is_fully_commissioned(void)
{
    uint8_t val = 0;
    nvs_handle_t h;
    if (nvs_open(k_bridge_ns, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, k_commissioned, &val);
        nvs_close(h);
    }
    return val != 0;
}

static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
    case DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Matter commissioning complete");
        mark_commissioned();
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
        // Start the sensor poller on every IP assignment.
        // kCommissioningComplete only fires on the first commissioning; on
        // subsequent boots the device reconnects without commissioning again.
        // Checking fabric count ensures we only start after a fabric exists.
        if (s_on_commissioned && matter_bridge_is_commissioned()) {
            s_on_commissioned();
        }
        break;

    default:
        break;
    }
}

// ─── Endpoint factory ─────────────────────────────────────────────────────────

/**
 * Create a Matter endpoint for a single sensor type and store its ID
 * in the registry entry.
 *
 * HA 2026.x matter integration only creates sensor entities for endpoints
 * that are direct children of ep0 (Root Node).  Bridge sub-endpoints
 * (in the Aggregator's PartsList) only get an Identify button entity,
 * never sensor measurement entities — confirmed in HA core debug logs:
 *   "Creating button entity for Identify.IdentifyType" (×9)
 *   but NO "Creating sensor entity for TemperatureMeasurement" etc.
 *
 * Fix: no bridge topology.  Use plain sensor::create() with ENDPOINT_FLAG_NONE
 * so endpoints appear in ep0's PartsList.  HA then treats them as root-device
 * endpoints and creates sensor entities for measurement clusters.
 * All WS90 sensors appear under ONE "WS90 Weather Bridge" device instead of
 * 9 sub-devices, but the sensor values are finally visible.
 */
static esp_err_t create_sensor_endpoint(registry_entry_t *entry,
                                         sensor_type_t     type,
                                         float             initial_value)
{
    endpoint_t *ep = nullptr;

    switch (type) {

    case SENSOR_TEMPERATURE: {
        temperature_sensor::config_t cfg = {};
        cfg.temperature_measurement.measured_value     = (int16_t)(initial_value * 100.0f);
        cfg.temperature_measurement.min_measured_value = (int16_t)-4000;
        cfg.temperature_measurement.max_measured_value = (int16_t) 8500;
        ep = temperature_sensor::create(s_node, &cfg, ENDPOINT_FLAG_NONE, nullptr);
        break;
    }

    case SENSOR_HUMIDITY: {
        humidity_sensor::config_t cfg = {};
        cfg.relative_humidity_measurement.measured_value     = (uint16_t)(initial_value * 100.0f);
        cfg.relative_humidity_measurement.min_measured_value = (uint16_t)0;
        cfg.relative_humidity_measurement.max_measured_value = (uint16_t)10000;
        ep = humidity_sensor::create(s_node, &cfg, ENDPOINT_FLAG_NONE, nullptr);
        break;
    }

    case SENSOR_PRESSURE: {
        pressure_sensor::config_t cfg = {};
        cfg.pressure_measurement.measured_value     = (int16_t)(initial_value);
        cfg.pressure_measurement.min_measured_value = (int16_t)0;
        cfg.pressure_measurement.max_measured_value = (int16_t)12000;
        ep = pressure_sensor::create(s_node, &cfg, ENDPOINT_FLAG_NONE, nullptr);
        break;
    }

    case SENSOR_ILLUMINANCE: {
        light_sensor::config_t cfg = {};
        float lux = initial_value > 0 ? initial_value : 1.0f;
        cfg.illuminance_measurement.measured_value     = (uint16_t)(10000.0f * log10f(lux) + 1.0f);
        cfg.illuminance_measurement.min_measured_value = (uint16_t)1;
        cfg.illuminance_measurement.max_measured_value = (uint16_t)65533;
        ep = light_sensor::create(s_node, &cfg, ENDPOINT_FLAG_NONE, nullptr);
        break;
    }

    case SENSOR_WIND_SPEED:
    case SENSOR_WIND_SPEED_GUST:
    case SENSOR_WIND_DIRECTION:
    case SENSOR_RAIN:
    case SENSOR_UV_INDEX:
    case SENSOR_BATTERY: {
        flow_sensor::config_t cfg = {};
        cfg.flow_measurement.measured_value     = (uint16_t)(initial_value * 10.0f);
        cfg.flow_measurement.min_measured_value = (uint16_t)0;
        cfg.flow_measurement.max_measured_value = (uint16_t)65534;
        ep = flow_sensor::create(s_node, &cfg, ENDPOINT_FLAG_NONE, nullptr);
        break;
    }

    default:
        ESP_LOGW(TAG, "No Matter mapping for sensor type %d", type);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (!ep) {
        ESP_LOGE(TAG, "Failed to create endpoint for sensor type %d", type);
        return ESP_FAIL;
    }

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

    // No Aggregator endpoint: HA 2026.x only creates sensor entities for endpoints
    // in ep0's PartsList (root device endpoints), not for bridge sub-endpoints in
    // an Aggregator's PartsList.  Using plain sensor endpoints without ENDPOINT_FLAG_BRIDGE
    // places them in ep0's PartsList so HA creates proper sensor entities.

    // Pre-create all WS90 sensor endpoints BEFORE commissioning starts.
    // Reason: HA's matter.js reads descriptor.deviceTypeList during the initial
    // attribute enumeration at commissioning time. If endpoints are added dynamically
    // AFTER commissioning, matter.js tries to process them before reading their
    // descriptor, leaving deviceTypeList as undefined → crash in #updateDeviceTypes.
    // By pre-creating here, all endpoints exist during the initial attribute read.
    static const uint8_t WS90_MAC[6] = {0x0D, 0x3D, 0x13, 0x6A, 0x4D, 0xFC};
    registry_entry_t *ws90 = sensor_registry_get_or_create(WS90_MAC, "WS90");
    if (ws90) {
        static const sensor_type_t ws90_types[] = {
            SENSOR_BATTERY,
            SENSOR_TEMPERATURE,
            SENSOR_HUMIDITY,
            SENSOR_PRESSURE,
            SENSOR_ILLUMINANCE,
            SENSOR_WIND_SPEED,
            SENSOR_WIND_DIRECTION,
            SENSOR_RAIN,
            SENSOR_UV_INDEX,
        };
        // Use non-zero sentinel values: MeasuredValue = 0 risks being parsed as
        // Matter NullValue by some controllers; realistic defaults are safer.
        static const float ws90_defaults[] = {
            50.0f,    // BATTERY (%)
            20.0f,    // TEMPERATURE (°C → 2000 = 20.00°C)
            50.0f,    // HUMIDITY (% → 5000 = 50.00%)
            1013.0f,  // PRESSURE (hPa → stored as int16)
            100.0f,   // ILLUMINANCE (lux)
            0.1f,     // WIND_SPEED (m/s)
            0.1f,     // WIND_DIRECTION (°)
            0.0f,     // RAIN (mm) — 0 is valid since no rain is the default
            1.0f,     // UV_INDEX
        };
        for (int i = 0; i < (int)(sizeof(ws90_types) / sizeof(ws90_types[0])); i++) {
            create_sensor_endpoint(ws90, ws90_types[i], ws90_defaults[i]);
        }
        ESP_LOGI(TAG, "Matter bridge: pre-created %d WS90 endpoints",
                 (int)(sizeof(ws90_types) / sizeof(ws90_types[0])));
    }

    return ESP_OK;
}

esp_err_t matter_bridge_start(void)
{
    // Re-set CommissionableDataProvider right before BLE advertising starts.
    chip::DeviceLayer::SetCommissionableDataProvider(&s_cdp);

    // Write MAC-derived discriminator and passcode directly into CHIP's NVS storage
    // (namespace "chip-config") before esp_matter::start() reads them.
    // ConfigurationMgr().GetSetupDiscriminator() reads NVS first; on fresh flash
    // (empty NVS after web-installer erase) it returns the hardcoded default 0xF00
    // (3840) and ignores CommissionableDataProvider. This causes BLE to advertise
    // disc=0xF00 while the QR code shows the MAC-derived disc=1562, so HA can't
    // find the device via BLE and commissioning times out.
    {
        uint16_t disc = 0;
        s_cdp.GetSetupDiscriminator(disc);

        // CHIP reads SetupDiscriminator from "chip-factory" namespace (factory-provisioned
        // data), NOT "chip-config". Writing there ensures ConfigurationMgr() returns the
        // MAC-derived value instead of falling back to the hardcoded default 0xF00.
        // Only write discriminator — writing pin-code causes CHIP to generate its own
        // SPAKE2+ verifier from NVS (bypassing CommissionableDataProvider's verifier),
        // which breaks PASE.
        nvs_handle_t h;
        if (nvs_open("chip-factory", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_u32(h, "discriminator", (uint32_t)disc);
            nvs_commit(h);
            nvs_close(h);
            ESP_LOGI(TAG, "Pre-stored discriminator=%u in chip-factory NVS", disc);
        }
    }

    esp_err_t ret = esp_matter::start(app_event_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_matter::start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Detect stale partial commissioning: AddNOC was stored in NVS (FabricCount > 0)
    // but CommissioningComplete was never received (flag not set). This happens when
    // commissioning is interrupted (WiFi connect failure, reboot, etc.).
    // In this state, the device boots in operational mode with no BLE advertising —
    // iOS can't find it and HA times out after 3 minutes. Factory reset restores
    // clean commissioning mode.
    chip::DeviceLayer::SystemLayer().ScheduleLambda([]() {
        if (chip::Server::GetInstance().GetFabricTable().FabricCount() > 0
                && !is_fully_commissioned()) {
            ESP_LOGW(TAG, "Stale partial commissioning detected "
                         "(AddNOC in NVS but CommissioningComplete never received). "
                         "Scheduling factory reset to restore clean commissioning mode.");
            chip::Server::GetInstance().ScheduleFactoryReset();
        }
    });

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
            // Endpoint not pre-created (unexpected sensor type) — skip.
            ESP_LOGD(TAG, "No pre-created endpoint for sensor type %d, skipping", type);
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
