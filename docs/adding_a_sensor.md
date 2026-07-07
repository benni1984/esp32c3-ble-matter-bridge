# Adding a New BTHome Measurement Type

The shelly_poller pipeline decodes BTHome v2 payloads from any relayed
device and feeds readings into the Matter bridge — the steps below are the
same regardless of which physical device sends the measurement. This adds a
new *measurement type* to the firmware's vocabulary (once), not a per-device
change (see [`docs/supported_devices.md`](supported_devices.md) for how new
*devices* are picked up automatically without any code change). The steps
below show how to expose an additional measurement as a new Matter endpoint,
using the **dewpoint** reading (as broadcast by the Ecowitt WS90) as a
worked example — it's already parsed by `bthome_parse()` today but
intentionally has no Matter endpoint yet (see the note in `bthome.cpp`), so
this doubles as a real, currently-accurate walkthrough rather than a
hypothetical one.

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
in `sensor_data_t`. A type only becomes a visible Matter endpoint if
`create_sensor_endpoint()` has a `case` for it — `sensor_type_t` values
without one are parsed but silently dropped by `matter_bridge_update()`
("No pre-created endpoint for sensor type %d, skipping").

---

## Step 1 — sensor_type_t already covers this example

`components/bthome/include/bthome.h` already has:

```c
typedef enum {
    SENSOR_BATTERY,
    SENSOR_TEMPERATURE,
    SENSOR_HUMIDITY,
    SENSOR_PRESSURE,
    SENSOR_ILLUMINANCE,
    SENSOR_WIND_SPEED,
    SENSOR_WIND_SPEED_GUST,
    SENSOR_WIND_DIRECTION,
    SENSOR_RAIN,
    SENSOR_UV_INDEX,
    SENSOR_DEWPOINT,             // ← parsed already, no endpoint yet
    SENSOR_CAPACITOR_VOLTAGE,    // ← same story, see bthome.cpp for why
    SENSOR_TYPE_COUNT            // must always be last!
} sensor_type_t;
```

For a genuinely new measurement, add your own entry here (before
`SENSOR_TYPE_COUNT`) and a matching name in the `names[]` array inside
`sensor_type_name()` in `bthome.cpp` — the `static_assert` right below it
will fail to compile if the two ever fall out of sync.

---

## Step 2 — BTHome Object ID mapping in `bthome.cpp`

The `s_objects[]` table in `components/bthome/bthome.cpp` maps each BTHome
Object ID to a `sensor_type_t`, a scale factor, byte width, and signedness:

```c
static constexpr ObjDef s_objects[] = {
    { 0x01, SENSOR_BATTERY,           1.0f,   1, false },
    { 0x02, SENSOR_TEMPERATURE,       0.01f,  2, true  },
    { 0x03, SENSOR_HUMIDITY,          0.01f,  2, false },
    { 0x04, SENSOR_PRESSURE,          0.01f,  3, false },
    { 0x05, SENSOR_ILLUMINANCE,       0.01f,  3, false },
    { 0x08, SENSOR_DEWPOINT,          0.01f,  2, true  },  // ← our example
    { 0x0C, SENSOR_CAPACITOR_VOLTAGE, 0.001f, 2, false },
    { 0x20, SENSOR_RAIN,              1.0f,   1, false },  // rain status (binary)
    { 0x2E, SENSOR_HUMIDITY,          1.0f,   1, false },  // alt. humidity encoding
    { 0x44, SENSOR_WIND_SPEED,        0.01f,  2, false },
    { 0x45, SENSOR_TEMPERATURE,       0.1f,   2, true  },  // alt. temperature encoding
    { 0x46, SENSOR_UV_INDEX,          0.1f,   1, false },
    { 0x5E, SENSOR_WIND_DIRECTION,    0.01f,  2, false },
    { 0x5F, SENSOR_RAIN,              0.1f,   2, false },  // precipitation
};
```

Fields:
- `obj_id` — the BTHome Object ID (see the [BTHome spec](https://bthome.io/format/))
- `type` — which `sensor_type_t` this object maps to
- `factor` — `raw_value × factor = physical value`
- `bytes` — number of payload bytes consumed by this object
- `signed` — `true` if the raw integer is signed (two's complement)

Dewpoint (`0x08`) is already present — that's the point of this example. If
you're adding something new, insert a row here the same way.

> **Watch for object-ID collisions onto the same `sensor_type_t`.** `0x08`
> (dewpoint) and `0x0C` (capacitor voltage) used to be mapped onto
> `SENSOR_TEMPERATURE` / `SENSOR_BATTERY` — since the WS90 sends both the real
> reading and these secondary ones in the same payload, whichever arrived
> later in the byte stream silently overwrote the correct value on the same
> Matter endpoint. Give genuinely different physical quantities their own
> `sensor_type_t`, not an existing one, even if you don't wire up a Matter
> endpoint for it right away.

---

## Step 3 — Map to a Matter cluster in `matter_bridge.cpp`

Two places need a new `case`. Which pattern to use depends on whether
esp-matter exposes a documented per-cluster setter for the cluster you need
(check `components/matter_bridge/matter_bridge.cpp` — clusters using the
`clusters/*/integration.h` free-function API are the safe/documented path;
Temperature and Illuminance currently have no such helper in the pinned
esp-matter version and go through the generic
`find_measurement_cluster<T>()` registry lookup instead). For a
temperature-like value such as dewpoint, reuse the Temperature pattern:

**In `create_sensor_endpoint()`** (add a case; `flow_sensor_label()` right
above it is only relevant if you're reusing the Flow cluster, not for a
Temperature-cluster endpoint):
```cpp
case SENSOR_DEWPOINT: {
    temperature_sensor::config_t cfg = {};
    cfg.temperature_measurement.measured_value     = (int16_t)(initial_value * 100.0f);
    cfg.temperature_measurement.min_measured_value = (int16_t)-4000;
    cfg.temperature_measurement.max_measured_value = (int16_t) 8500;
    ep = temperature_sensor::create(s_node, &cfg, ENDPOINT_FLAG_NONE, nullptr);
    break;
}
```
(No `ENDPOINT_FLAG_BRIDGE` / aggregator parent — the bridge topology was
removed. HA only creates sensor entities for endpoints in ep0's own
PartsList; see the comment above `create_sensor_endpoint()` for why.)

**In `matter_bridge_update()`**, inside the `switch (type)` block:
```cpp
case SENSOR_DEWPOINT: {
    auto *cluster = find_measurement_cluster<TemperatureMeasurementCluster>(
        ep_id, TemperatureMeasurement::Id);
    if (cluster) cluster->SetMeasuredValue(Nullable<int16_t>((int16_t)(r.value * 100.0f)));
    break;
}
```

Do **not** use `attribute::update()` / `attribute::set_val()` here — those
write to esp-matter's legacy in-memory attribute store, which nothing reads
from for the real Matter bootstrap read or live reporting in this esp-matter
version (see the comment above `find_measurement_cluster()` in
`matter_bridge.cpp` for the full story of why that was a dead end for
several releases).

Matter 1.3 has no dedicated cluster for wind, rain, or UV. The workaround
used in this firmware is **FlowMeasurement**, distinguished per-endpoint via
a Fixed Label (see the README's "Home Assistant naming" section) — visible in
Home Assistant but not with a dedicated UI in Apple Home.

---

## Step 4 — Rebuild and flash

```bash
idf.py build
idf.py -p COM3 flash monitor
```

**The new endpoint does not appear immediately — it needs one more reboot
than you might expect.** Matter endpoints can only be created before
commissioning starts (see `matter_bridge_init()`'s comment on why), so a
measurement type is only ever turned into a live endpoint by the
pre-creation loop that runs at boot, using what was *already known* from
before this boot. Concretely, after flashing:
1. First boot with the new firmware: `bthome_parse()` starts recognising the
   new object ID, and the first successful poll (~90s+ after boot, once
   WiFi/CASE is up) marks the type as known and persists it — you'll see
   `"New sensor type '...' observed for ... — will be exposed as a Matter
   endpoint after next reboot"` in the log. No endpoint exists yet this
   session.
2. Reboot again (power-cycle, or `reboot` on the console): *now* the
   pre-creation loop sees the persisted type and creates the endpoint before
   commissioning/CASE starts.

(If the device was already broadcasting this measurement in earlier
sessions — as dewpoint has been all along in this worked example, just
without a Matter mapping — the type may already be marked known from
before, in which case the endpoint appears right after the first reboot.)

No re-commissioning is needed unless the endpoint count changes while an
existing fabric is active — though if you're using a Fixed Label
(FlowMeasurement endpoints), remember HA only applies those to entities at
first creation, so a fresh commissioning is needed for a *new* installation
to show the label immediately.

---

## Notes

- The number of distinct physical *devices* tracked is limited to **16** by
  `REGISTRY_MAX_SENSORS` in `components/sensor_registry/include/sensor_registry.h`;
  each device can expose as many sensor types as its BTHome payload contains
  (up to `SENSOR_TYPE_COUNT`, currently 12).
- Any BTHome device's payload is decoded via the same `bthome_parse()`
  function — no per-device special handling needed, only per-*measurement-type*
  (this doc) if the object ID isn't recognised yet.
- **Apple Home** only has native accessory types for Temperature, Humidity,
  and Illuminance — not Pressure, and not Flow-cluster-backed measurements
  (wind/rain/UV/battery). Those remain accessible via Home Assistant
  directly, or via Home Assistant's separate HomeKit Bridge integration into
  Apple Home (see `CLAUDE.md`).
