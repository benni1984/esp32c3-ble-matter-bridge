# Adding a New BLE Sensor

This project is designed so that new sensors can be integrated with minimal effort.
Most sensors that use the **BTHome v2** protocol work without changing any core
component code — you only need to add a few entries.

---

## Case 1: Sensor uses BTHome v2 with already-supported measurement types

Just bring the ESP32 within range of the sensor.
If the sensor is detected (UUID `0xFCD2` in its BLE advertisement), it will automatically
appear as a new device in Apple Home / Home Assistant — no code changes needed.

Already supported types: Temperature, Humidity, Pressure, Illuminance, UV Index,
Wind Speed, Wind Direction, Rainfall, Battery.

---

## Case 2: Sensor uses BTHome v2 but with an unknown Object ID

### Step 1 — Add a new sensor type in `bthome.h`

Open `components/bthome/include/bthome.h` and add your type to the enum:

```c
typedef enum {
    SENSOR_BATTERY,
    SENSOR_TEMPERATURE,
    // ... existing types ...
    SENSOR_CO2,          // ← add here
    SENSOR_TYPE_COUNT    // must always be the last entry!
} sensor_type_t;
```

Also add a name string in `sensor_type_name()` in `bthome.cpp` at the same position as the enum entry.

### Step 2 — Add the Object ID in `bthome.cpp`

Open `components/bthome/bthome.cpp` and add a row to the `s_objects[]` table:

```c
// obj_id  type          factor  bytes  signed
{ 0x12,  SENSOR_CO2,   1.0f,   2,     false },
```

Where:
- `obj_id` — the BTHome Object ID (from the [BTHome specification](https://bthome.io/format/))
- `factor` — raw_value × factor = physical value
- `bytes` — number of payload bytes for this object
- `signed` — `true` if the value is a signed integer

### Step 3 — Add the Matter mapping in `matter_bridge.cpp`

Open `components/matter_bridge/matter_bridge.cpp` and extend two `switch` blocks:

**In the `create_sensor_endpoint()` switch:**
```cpp
case SENSOR_CO2: {
    // CO2 has no dedicated Matter cluster in spec 1.3.
    // Use FlowMeasurement as a generic placeholder.
    flow_sensor::config_t cfg;
    cfg.flow_measurement.measured_value = (uint16_t)(initial_value);
    ep = flow_sensor::create(s_node, &cfg, ENDPOINT_FLAG_BRIDGE, s_aggregator);
    break;
}
```

**In the `matter_bridge_update()` switch:**
```cpp
case SENSOR_CO2:
    cluster_id = chip::app::Clusters::FlowMeasurement::Id;
    attr_id    = chip::app::Clusters::FlowMeasurement::Attributes::MeasuredValue::Id;
    val        = esp_matter_uint16((uint16_t)(r.value));
    break;
```

That's it. Rebuild (`idf.py build`), flash, done.

---

## Case 3: Sensor uses a different protocol (not BTHome)

Example: a proprietary device with its own advertisement format.

### Step 1 — Write a custom parser

Create a new component under `components/sensors/my_sensor/`:

```
components/sensors/my_sensor/
├── CMakeLists.txt
├── include/my_sensor.h
└── my_sensor.cpp
```

Your parser receives the raw advertisement bytes and fills a `sensor_data_t` struct:

```c
bool my_sensor_parse(const uint8_t *adv_data, size_t len,
                     const uint8_t mac[6], sensor_data_t *out);
```

### Step 2 — Register in `ble_scanner`

In `ble_scanner.cpp`, add a second filter alongside the BTHome UUID filter —
matching your sensor's Manufacturer Specific Data company ID or a custom service UUID.

### Step 3 — Wire up in `main.cpp`

Call your parser in the `on_ble_advertisement()` callback,
before or after `bthome_parse()` is attempted:

```cpp
if (!bthome_parse(svc_data, svc_data_len, &data)) {
    if (!my_sensor_parse(raw_adv, raw_len, mac, &data)) return;
}
matter_bridge_update(mac, &data);
```

---

## Notes

- **Encrypted BTHome packets** are currently skipped.
  Decryption requires a per-device binding key; support is planned for a future release.
- **Apple Home** only displays standard Matter clusters (Temperature, Humidity, Pressure, Illuminance).
  Wind, rain, etc. are only visible in Home Assistant under the bridge device.
- **Maximum 16 sensors** simultaneously (constant `REGISTRY_MAX_SENSORS` in `sensor_registry.h`).
