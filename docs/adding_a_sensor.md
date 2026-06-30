# Adding a New WS90 Measurement Type

The shelly_poller pipeline decodes the WS90's BTHome v2 payload and feeds readings
into the Matter bridge. The steps below show how to expose an additional WS90
measurement (e.g. wind gust) as a new Matter endpoint.

---

## How the pipeline works

```
Shelly HTTP (BLE.CloudRelay.ListInfos)
  → base64-decode the fcd2 service-data field
  → bthome_parse()          (components/bthome/bthome.cpp)
  → sensor_registry         (components/sensor_registry/)
  → matter_bridge_update()  (components/matter_bridge/matter_bridge.cpp)
```

Each recognised BTHome object ID in the payload becomes one `sensor_reading_t`
in `sensor_data_t`, which then maps to one Matter endpoint under the bridge.

---

## Step 1 — Add a sensor type in `bthome.h`

Open `components/bthome/include/bthome.h` and add your type to the enum:

```c
typedef enum {
    SENSOR_BATTERY,
    SENSOR_TEMPERATURE,
    SENSOR_HUMIDITY,
    SENSOR_PRESSURE,
    SENSOR_ILLUMINANCE,
    SENSOR_WIND_SPEED,
    SENSOR_WIND_SPEED_GUST,   // ← already present
    SENSOR_WIND_DIRECTION,
    SENSOR_RAIN,
    SENSOR_UV_INDEX,
    SENSOR_DEW_POINT,         // ← new example
    SENSOR_TYPE_COUNT         // must always be last!
} sensor_type_t;
```

Also add a name string for the new type inside `sensor_type_name()` in
`components/bthome/bthome.cpp` at the same position as the enum entry.

---

## Step 2 — Add the BTHome Object ID in `bthome.cpp`

Open `components/bthome/bthome.cpp` and add a row to the `s_objects[]` table:

```c
// obj_id   type               factor   bytes  signed
{ 0x???,  SENSOR_DEW_POINT,   0.01f,   2,     true  },
```

Fields:
- `obj_id` — the BTHome Object ID (see the [BTHome spec](https://bthome.io/format/)
  and Ecowitt/Shelly proprietary extensions `0xD1`–`0xD4`)
- `factor` — `raw_value × factor = physical value`
- `bytes` — number of payload bytes consumed by this object
- `signed` — `true` if the raw integer is signed (two's complement)

The WS90's proprietary object IDs currently mapped are:

| Object ID | Measurement       |
|-----------|-------------------|
| `0xD1`    | Wind speed (avg)  |
| `0xD2`    | Wind direction    |
| `0xD3`    | Wind gust         |
| `0xD4`    | Rain              |

---

## Step 3 — Map to a Matter cluster in `matter_bridge.cpp`

Open `components/matter_bridge/matter_bridge.cpp` and extend two `switch` blocks.

**In `create_sensor_endpoint()`:**
```cpp
case SENSOR_DEW_POINT: {
    // No dedicated Matter cluster — use TemperatureMeasurement as a proxy.
    temperature_sensor::config_t cfg;
    cfg.temperature_measurement.measured_value = (int16_t)(initial_value * 100);
    ep = temperature_sensor::create(s_node, &cfg, ENDPOINT_FLAG_BRIDGE, s_aggregator);
    break;
}
```

**In `matter_bridge_update()`:**
```cpp
case SENSOR_DEW_POINT:
    cluster_id = chip::app::Clusters::TemperatureMeasurement::Id;
    attr_id    = chip::app::Clusters::TemperatureMeasurement::Attributes::MeasuredValue::Id;
    val        = esp_matter_int16((int16_t)(r.value * 100));
    break;
```

Matter 1.3 does not have dedicated clusters for wind, rain, UV, or dew point.
The workaround used in this firmware is **FlowMeasurement** for dimensionless /
non-temperature values — it is visible in Home Assistant but not in Apple Home.

---

## Step 4 — Rebuild and flash

```bash
idf.py build
idf.py -p COM3 flash monitor
```

The new endpoint appears automatically on the next Shelly poll (every 10 seconds).
No re-commissioning is needed unless the endpoint count changes while an existing
fabric is active.

---

## Notes

- Endpoint count is limited to **16** by `REGISTRY_MAX_SENSORS` in
  `components/sensor_registry/include/sensor_registry.h`.
- The WS90 BTHome payload is decoded by `shelly_poller` via the same
  `bthome_parse()` function used everywhere else — no special handling needed.
- **Apple Home** only displays Temperature, Humidity, Pressure, and Illuminance
  clusters with a dedicated UI. All other endpoints are accessible via
  Home Assistant or any generic Matter attribute browser.
