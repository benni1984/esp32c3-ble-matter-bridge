#include "matter_bridge.h"
#include "mac_commissioning_data_provider.h"
#include "bridge_device_info_provider.h"
#include "bthome.h"

#include <esp_matter.h>
#include <esp_matter_endpoint.h>
#include <esp_matter_cluster.h>
#include <esp_matter_attribute_utils.h>
#include <esp_matter_console.h>
#include <esp_matter_ota.h>

#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
// Pressure/Humidity/Flow: esp-matter exposes a dedicated, documented
// SetMeasuredValue()/FindClusterOnEndpoint() free-function API for these three
// (see clusters/*/integration.h) — the safe, officially-supported path.
#include <clusters/pressure_measurement/integration.h>
#include <clusters/relative_humidity_measurement/integration.h>
#include <clusters/flow_measurement/integration.h>
// Temperature/Illuminance: no equivalent integration.h exposed in this
// esp-matter version — reach their registered cluster objects directly via the
// generic ServerClusterInterfaceRegistry instead (riskier: relies on
// connectedhomeip internals rather than an esp-matter-documented API).
#include <esp_matter_data_model_provider.h>
#include <app/server-cluster/ServerClusterInterfaceRegistry.h>
#include <app/ConcreteClusterPath.h>
#include <app/clusters/temperature-measurement-server/TemperatureMeasurementCluster.h>
#include <app/clusters/illuminance-measurement-server/IlluminanceMeasurementCluster.h>
#include <credentials/FabricTable.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/CommissionableDataProvider.h>
#include <setup_payload/QRCodeSetupPayloadGenerator.h>
#include <setup_payload/ManualSetupPayloadGenerator.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
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

// Looks up the registered cluster object for (endpoint, cluster) directly in
// chip::app's ServerClusterInterfaceRegistry. Used only for Temperature/
// Illuminance, which esp-matter doesn't expose a documented setter for in this
// version — see the include-block comment above for the tradeoff.
template <typename ClusterT>
static ClusterT *find_measurement_cluster(uint16_t endpoint_id, uint32_t cluster_id)
{
    auto *iface = esp_matter::data_model::provider::get_instance().registry().Get(
        chip::app::ConcreteClusterPath(endpoint_id, cluster_id));
    return iface ? static_cast<ClusterT *>(iface) : nullptr;
}

// Forward declaration — defined in "Initial attribute values" section below.
static void force_initial_attr_values(registry_entry_t *entry);

// Calls force_initial_attr_values() for every active registry entry, not
// just one hardcoded device.
static void force_initial_attr_values_all(void)
{
    for (int i = 0; i < sensor_registry_count(); i++) {
        registry_entry_t *e = sensor_registry_get(i);
        if (e && e->active) force_initial_attr_values(e);
    }
}

static node_t                          *s_node       = nullptr;
static endpoint_t                      *s_aggregator = nullptr;
static matter_bridge_commissioned_cb_t  s_on_commissioned = nullptr;
static MacCommissionableDataProvider    s_cdp;
static BridgeDeviceInfoProvider          s_device_info_provider;

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
        // Force non-null values immediately after CommissioningComplete.
        // The bootstrap read (matter-server initial attribute read) starts
        // ~200 ms after this event; these in-memory writes complete in
        // microseconds, so the bootstrap read will always see non-null values.
        // kInterfaceIpAddressChanged fires earlier but may race with
        // CHIP stack initialisation when the device is already on WiFi.
        force_initial_attr_values_all();
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
        // Force non-null initial values for all measurement attributes.
        // This is the definitive safe point: CHIP stack is fully initialized,
        // but CASE session and bootstrap read haven't happened yet.
        // The ScheduleLambda approach races with CHIP's NVS-restore during
        // start(); writing here avoids that race entirely.
        force_initial_attr_values_all();
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

// ─── Initial attribute values ─────────────────────────────────────────────────

// After esp_matter::start(), the real MeasuredValue held by each cluster's
// registered chip::app cluster object (see find_measurement_cluster() above)
// starts out Null. HA entity discovery skips any attribute that reads back
// Null at bootstrap, so sensor entities are never created on first
// commissioning unless something sets a real value first.
// This function runs on the CHIP task (via ScheduleLambda) immediately after
// start() and calls each cluster's real SetMeasuredValue() so the Matter
// bootstrap read always finds valid data, even before the first Shelly poll.
static void force_initial_attr_values(registry_entry_t *entry)
{
    using namespace chip::app::Clusters;
    using chip::app::DataModel::Nullable;

    // Pressure / Humidity / Flow: officially-supported SetMeasuredValue() free
    // functions from esp-matter's own clusters/*/integration.h.
    uint16_t pres_ep = entry->matter_endpoint_id[SENSOR_PRESSURE];
    if (pres_ep) {
        CHIP_ERROR err = PressureMeasurement::SetMeasuredValue(pres_ep, Nullable<int16_t>((int16_t)1013));
        ESP_LOGI(TAG, "force-init ep%u Pressure: %s", pres_ep, err == CHIP_NO_ERROR ? "OK" : "FAILED");
    }

    uint16_t hum_ep = entry->matter_endpoint_id[SENSOR_HUMIDITY];
    if (hum_ep) {
        CHIP_ERROR err = RelativeHumidityMeasurement::SetMeasuredValue(hum_ep, Nullable<uint16_t>((uint16_t)5000));
        ESP_LOGI(TAG, "force-init ep%u Humidity: %s", hum_ep, err == CHIP_NO_ERROR ? "OK" : "FAILED");
    }

    auto flow = [&](sensor_type_t t, uint16_t v) {
        uint16_t ep = entry->matter_endpoint_id[t];
        if (!ep) return;
        CHIP_ERROR err = FlowMeasurement::SetMeasuredValue(ep, Nullable<uint16_t>(v));
        ESP_LOGI(TAG, "force-init ep%u t%d Flow: %s", ep, t, err == CHIP_NO_ERROR ? "OK" : "FAILED");
    };
    flow(SENSOR_WIND_SPEED,      1);   // 0.1 m/s × 10
    flow(SENSOR_WIND_DIRECTION,  1);   // 0.1 ° × 10
    flow(SENSOR_RAIN,            1);   // 0.1 mm × 10 (not 0 to guarantee non-null)
    flow(SENSOR_UV_INDEX,       10);   // 1.0 × 10
    flow(SENSOR_BATTERY,       500);   // 50 % × 10

    // Temperature / Illuminance: no documented esp-matter setter in this version —
    // reach the registered cluster object directly via find_measurement_cluster().
    uint16_t temp_ep = entry->matter_endpoint_id[SENSOR_TEMPERATURE];
    auto *temp_cluster = find_measurement_cluster<TemperatureMeasurementCluster>(temp_ep, TemperatureMeasurement::Id);
    if (temp_cluster) {
        CHIP_ERROR err = temp_cluster->SetMeasuredValue(Nullable<int16_t>((int16_t)2000));   // 20.00 °C
        ESP_LOGI(TAG, "force-init ep%u Temperature: %s", temp_ep, err == CHIP_NO_ERROR ? "OK" : "FAILED");
    }

    uint16_t illu_ep = entry->matter_endpoint_id[SENSOR_ILLUMINANCE];
    auto *illu_cluster = find_measurement_cluster<IlluminanceMeasurementCluster>(illu_ep, IlluminanceMeasurement::Id);
    if (illu_cluster) {
        CHIP_ERROR err = illu_cluster->SetMeasuredValue(Nullable<uint16_t>((uint16_t)20001));  // log10(100)*10000+1
        ESP_LOGI(TAG, "force-init ep%u Illuminance: %s", illu_ep, err == CHIP_NO_ERROR ? "OK" : "FAILED");
    }

    ESP_LOGI(TAG, "Force-initialized MeasuredValue for all WS90 sensor endpoints");

    // Readback verification for the new-API endpoints, via each cluster's own
    // GetMeasuredValue() — the actual in-memory state the Matter bootstrap read
    // / HA discovery will see.
    if (pres_ep) {
        auto *cluster = PressureMeasurement::FindClusterOnEndpoint(pres_ep);
        if (cluster) {
            auto v = cluster->GetMeasuredValue();
            ESP_LOGI(TAG, "READBACK ep%u Pressure %s", pres_ep, v.IsNull() ? "*** NULL! ***" : "non-null OK");
        }
    }
    if (hum_ep) {
        auto *cluster = RelativeHumidityMeasurement::FindClusterOnEndpoint(hum_ep);
        if (cluster) {
            auto v = cluster->GetMeasuredValue();
            ESP_LOGI(TAG, "READBACK ep%u Humidity %s", hum_ep, v.IsNull() ? "*** NULL! ***" : "non-null OK");
        }
    }
    {
        uint16_t wind_ep = entry->matter_endpoint_id[SENSOR_WIND_SPEED];
        auto *cluster = FlowMeasurement::FindClusterOnEndpoint(wind_ep);
        if (cluster) {
            auto v = cluster->GetMeasuredValue();
            ESP_LOGI(TAG, "READBACK ep%u WindSpeed %s", wind_ep, v.IsNull() ? "*** NULL! ***" : "non-null OK");
        }
    }
    if (temp_cluster) {
        auto v = temp_cluster->GetMeasuredValue();
        ESP_LOGI(TAG, "READBACK ep%u Temperature %s", temp_ep, v.IsNull() ? "*** NULL! ***" : "non-null OK");
    }
    if (illu_cluster) {
        auto v = illu_cluster->GetMeasuredValue();
        ESP_LOGI(TAG, "READBACK ep%u Illuminance %s", illu_ep, v.IsNull() ? "*** NULL! ***" : "non-null OK");
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
 * All of one physical device's sensors appear under ONE HA device (grouped by
 * the bridged device's registry name) instead of one sub-device per sensor,
 * but the sensor values are finally visible.
 */
// Friendly name shown by Home Assistant for endpoints backed by the generic
// FlowMeasurement cluster (which HA otherwise labels indistinguishably as
// "Flow (N)") — see BridgeDeviceInfoProvider for how this reaches HA.
static const char *flow_sensor_label(sensor_type_t type)
{
    switch (type) {
    case SENSOR_WIND_SPEED:      return "Wind Speed";
    case SENSOR_WIND_SPEED_GUST: return "Wind Gust";
    case SENSOR_WIND_DIRECTION:  return "Wind Direction";
    case SENSOR_RAIN:            return "Rain";
    case SENSOR_UV_INDEX:        return "UV Index";
    case SENSOR_BATTERY:         return "Battery";
    default:                     return nullptr;
    }
}

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
        if (ep) {
            // flow_sensor's device type doesn't include FixedLabel by default —
            // add it so BridgeDeviceInfoProvider::IterateFixedLabel() has a
            // cluster to actually serve on this endpoint.
            cluster::fixed_label::config_t fl_cfg;
            cluster::fixed_label::create(ep, &fl_cfg, CLUSTER_FLAG_SERVER);
        }
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

    if (const char *label = flow_sensor_label(type)) {
        BridgeDeviceInfoProvider::RegisterFixedLabel(entry->matter_endpoint_id[type], label);
    }

    return ESP_OK;
}

// Non-zero sentinel values for pre-created endpoints: MeasuredValue = 0 risks
// being parsed as Matter NullValue by some controllers, so realistic defaults
// are safer than zero. These were the WS90's original per-type defaults,
// generalized here to apply to any device broadcasting the same sensor_type.
static float default_initial_value(sensor_type_t type)
{
    switch (type) {
    case SENSOR_BATTERY:        return 50.0f;    // %
    case SENSOR_TEMPERATURE:    return 20.0f;    // °C
    case SENSOR_HUMIDITY:       return 50.0f;    // %
    case SENSOR_PRESSURE:       return 1013.0f;  // hPa
    case SENSOR_ILLUMINANCE:    return 100.0f;   // lux
    case SENSOR_WIND_SPEED:
    case SENSOR_WIND_SPEED_GUST:
    case SENSOR_WIND_DIRECTION: return 0.1f;
    case SENSOR_RAIN:           return 0.0f;     // 0 is valid — no rain is the default
    case SENSOR_UV_INDEX:       return 1.0f;
    default:                    return 0.0f;     // DEWPOINT/CAPACITOR_VOLTAGE — no Matter mapping
    }
}

// ─── Public API ──────────────────────────────────────────────────────────────

esp_err_t matter_bridge_init(matter_bridge_commissioned_cb_t on_commissioned)
{
    s_on_commissioned = on_commissioned;

    // Register MAC-derived commissioning data so every device gets a unique QR code
    chip::DeviceLayer::SetCommissionableDataProvider(&s_cdp);

    // Register our FixedLabel source so the 5 identical FlowMeasurement
    // endpoints (wind speed/direction, rain, UV, battery) get distinct names
    // in Home Assistant instead of "Fluss (N)". Must be set before
    // esp_matter::start() so the FixedLabel cluster's init callback (which
    // reads DeviceLayer::GetDeviceInfoProvider()) sees it.
    chip::DeviceLayer::SetDeviceInfoProvider(&s_device_info_provider);

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

    // Pre-create Matter endpoints for every device × sensor_type this firmware
    // has ever seen (sensor_registry's persisted known_type_mask), BEFORE
    // commissioning starts.
    // Reason: HA's matter.js reads descriptor.deviceTypeList during the initial
    // attribute enumeration at commissioning time. If endpoints are added dynamically
    // AFTER commissioning, matter.js tries to process them before reading their
    // descriptor, leaving deviceTypeList as undefined → crash in #updateDeviceTypes.
    // By pre-creating here, all endpoints exist during the initial attribute read.
    // A device/type observed for the first time THIS session is persisted by
    // matter_bridge_update() but deliberately does NOT get a live endpoint
    // until the next boot reaches this loop — see CLAUDE.md's "New Devices
    // Need a Reboot to Appear in Matter" section.
    int total_created = 0;
    for (int i = 0; i < sensor_registry_count(); i++) {
        registry_entry_t *entry = sensor_registry_get(i);
        if (!entry || !entry->active) continue;

        int created = 0;
        for (int t = 0; t < SENSOR_TYPE_COUNT; t++) {
            if (!(entry->known_type_mask & (1u << t))) continue;
            sensor_type_t type = (sensor_type_t)t;
            if (create_sensor_endpoint(entry, type, default_initial_value(type)) == ESP_OK) {
                created++;
                total_created++;
            }
        }
        ESP_LOGI(TAG, "Matter bridge: pre-created %d endpoint(s) for %s "
                      "(%02X:%02X:%02X:%02X:%02X:%02X)",
                 created, entry->name,
                 entry->mac[0], entry->mac[1], entry->mac[2],
                 entry->mac[3], entry->mac[4], entry->mac[5]);
    }
    ESP_LOGI(TAG, "Matter bridge: pre-created %d endpoint(s) total across %d known device(s)",
             total_created, sensor_registry_count());
    ESP_LOGI(TAG, "Free heap after endpoint creation: %u bytes (largest block: %u)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

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

    // Fix HardwareVersionString: esp-matter reads this from NVS factory data.
    // When no factory data is provisioned it falls back to the SDK default
    // "TEST_VERSION".  Write "ESP32-C3" directly to the in-memory attribute so
    // HA shows a sensible hardware version without needing factory NVS data.
    {
        using namespace chip::app::Clusters;
        attribute_t *hw_attr = attribute::get(
            0,  // ep0 = Root Node
            BasicInformation::Id,
            BasicInformation::Attributes::HardwareVersionString::Id);
        if (hw_attr) {
            esp_matter_attr_val_t hw_val = esp_matter_char_str(
                (char *)"ESP32-C3", strlen("ESP32-C3"));
            attribute::set_val(hw_attr, &hw_val);
        }
    }

    // Detect stale partial commissioning: AddNOC was stored in NVS (FabricCount > 0)
    // but CommissioningComplete was never received (flag not set). This happens when
    // commissioning is interrupted (WiFi connect failure, reboot, etc.).
    // In this state, the device boots in operational mode with no BLE advertising —
    // iOS can't find it and HA times out after 3 minutes. Factory reset restores
    // clean commissioning mode.
    chip::DeviceLayer::SystemLayer().ScheduleLambda([]() {
        // Force non-null initial values for all sensor measurement attributes.
        // Must run after esp_matter::start() has initialized the CHIP attribute
        // store (which resets nullable attrs to NullValue when NVS is empty).
        force_initial_attr_values_all();

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
    if (!entry) {
        // Defensive only — sensor_registry_get_or_create() evicts the
        // least-recently-seen entry instead of returning null once the
        // table is full, so this shouldn't actually happen.
        ESP_LOGW(TAG, "Sensor registry full — cannot track new device %02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return;
    }
    if (!entry->active) {
        // Blocked via 'sensor_reg block' — ignore its readings entirely.
        ESP_LOGD(TAG, "Ignoring reading from blocked device %02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return;
    }

    // A device/sensor_type pair seen for the first time (new MAC, or a known
    // device broadcasting a type it never has before) is persisted here but
    // deliberately NOT given a live Matter endpoint this session — endpoints
    // can only be created before commissioning starts (see matter_bridge_init()).
    // It gets one automatically at the next boot.
    bool newly_learned = false;
    for (int i = 0; i < data->reading_count; i++) {
        if (sensor_registry_mark_known(entry, data->readings[i].type)) {
            newly_learned = true;
            ESP_LOGW(TAG, "New sensor type '%s' observed for %s (%02X:%02X:%02X:%02X:%02X:%02X) — "
                          "will be exposed as a Matter endpoint after next reboot",
                     sensor_type_name(data->readings[i].type), entry->name,
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        }
    }
    if (newly_learned) sensor_registry_save();

    // This runs on the Shelly poller's own task, not the CHIP/Matter task.
    // The new per-cluster SetMeasuredValue() API (unlike the legacy
    // attribute::update(), which took this lock internally) asserts that the
    // caller already holds the CHIP stack lock — without it, esp-matter aborts
    // with "Chip stack locking error ... Code is unsafe/racy" on the very
    // first live update after boot.
    esp_matter::lock::ScopedChipStackLock chip_lock(portMAX_DELAY);

    for (int i = 0; i < data->reading_count; i++) {
        const sensor_reading_t &r    = data->readings[i];
        sensor_type_t           type = r.type;
        uint16_t ep_id               = entry->matter_endpoint_id[type];

        if (ep_id == 0) {
            // Endpoint not pre-created (unexpected sensor type) — skip.
            ESP_LOGD(TAG, "No pre-created endpoint for sensor type %d, skipping", type);
            continue;
        }

        // Update the endpoint's MeasuredValue on the real, chip-registered cluster
        // object — not the legacy esp_matter attribute store (see
        // find_measurement_cluster() above for why).
        using namespace chip::app::Clusters;
        using chip::app::DataModel::Nullable;

        // Pressure/Humidity/Flow: official per-cluster free-function API.
        // Temperature/Illuminance: generic registry lookup (see
        // find_measurement_cluster() above). Both reach the real, chip-registered
        // cluster object instead of the disconnected legacy attribute store.
        CHIP_ERROR err = CHIP_NO_ERROR;
        switch (type) {
        case SENSOR_TEMPERATURE: {
            auto *cluster = find_measurement_cluster<TemperatureMeasurementCluster>(ep_id, TemperatureMeasurement::Id);
            err = cluster ? cluster->SetMeasuredValue(Nullable<int16_t>((int16_t)(r.value * 100.0f)))
                          : CHIP_ERROR_NOT_FOUND;
            break;
        }
        case SENSOR_HUMIDITY:
            err = RelativeHumidityMeasurement::SetMeasuredValue(ep_id, Nullable<uint16_t>((uint16_t)(r.value * 100.0f)));
            break;
        case SENSOR_PRESSURE:
            err = PressureMeasurement::SetMeasuredValue(ep_id, Nullable<int16_t>((int16_t)(r.value)));
            break;
        case SENSOR_ILLUMINANCE: {
            float lux = r.value > 0 ? r.value : 1.0f;
            auto *cluster = find_measurement_cluster<IlluminanceMeasurementCluster>(ep_id, IlluminanceMeasurement::Id);
            err = cluster ? cluster->SetMeasuredValue(Nullable<uint16_t>((uint16_t)(10000.0f * log10f(lux) + 1.0f)))
                          : CHIP_ERROR_NOT_FOUND;
            break;
        }
        default:
            // Generic: flow cluster, value * 10
            err = FlowMeasurement::SetMeasuredValue(ep_id, Nullable<uint16_t>((uint16_t)(r.value * 10.0f)));
            break;
        }

        if (err != CHIP_NO_ERROR)
            ESP_LOGW(TAG, "Update ep %d (%s) FAILED: %" CHIP_ERROR_FORMAT, ep_id, sensor_type_name(type), err.Format());
        else
            ESP_LOGI(TAG, "Updated ep %d (%s) = %.2f", ep_id, sensor_type_name(type), r.value);
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
