# Supported Devices

This firmware acts as a **Matter bridge** for any Bluetooth Low Energy device that
broadcasts **BTHome v2** advertisements (Service UUID `0xFCD2`).  
It does not need to know about a specific device model in advance — as long as
the advertisement carries a measurement type the firmware recognises, a Matter
endpoint is created automatically.

---

## How Device Detection Works

1. The ESP32-C3 scans passively for BLE advertisements.
2. Any advertisement carrying Service UUID `0xFCD2` is treated as a BTHome v2
   packet.
3. The payload is decoded object-by-object. Each recognised measurement type
   creates one Matter endpoint under the bridge (first occurrence) or updates
   the existing endpoint (subsequent occurrences).
4. Endpoints survive a reboot — they are restored from NVS without needing the
   device to advertise again.

---

## Confirmed Compatible Devices

### Ecowitt WS90 "Powered by Shelly" ✅ (primary target)

The WS90 is a solar-powered outdoor weather station that transmits all sensor
data via BTHome v2, including Ecowitt's proprietary object IDs for wind and rain.

| Measurement       | BTHome Object ID | Matter Cluster                    | Apple Home | Home Assistant |
|-------------------|-----------------|-----------------------------------|------------|----------------|
| Temperature       | `0x02`          | Temperature Measurement           | ✅          | ✅              |
| Humidity          | `0x03`          | Relative Humidity Measurement     | ✅          | ✅              |
| Pressure          | `0x04`          | Pressure Measurement              | ✅          | ✅              |
| Illuminance       | `0x05`          | Illuminance Measurement           | ✅          | ✅              |
| UV Index          | `0x4A`          | Flow Measurement (workaround)     | ❌*         | ✅              |
| Wind Speed (avg)  | `0xD1`          | Flow Measurement (workaround)     | ❌*         | ✅              |
| Wind Gust         | `0xD3`          | Flow Measurement (workaround)     | ❌*         | ✅              |
| Wind Direction    | `0xD2`          | Flow Measurement (workaround)     | ❌*         | ✅              |
| Rain              | `0xD4`          | Flow Measurement (workaround)     | ❌*         | ✅              |
| Battery           | `0x01`          | Flow Measurement (workaround)     | ❌*         | ✅              |

> \* Apple Home does not display Flow Measurement endpoints with a dedicated UI.
> The values are still stored in the Matter fabric and accessible via
> Home Assistant or any Matter-compatible controller that queries all attributes.

Object IDs `0xD1`–`0xD4` are Ecowitt/Shelly proprietary extensions.
They are explicitly supported in `bthome.cpp`.

---

### ESPHome Devices with BTHome Component ✅

Any ESP32/ESP8266 running ESPHome and configured with the
[BTHome sensor component](https://esphome.io/components/sensor/bthome.html)
is supported, provided:

- BTHome version is set to v2 (default since ESPHome 2023.6).
- The advertisement is **not encrypted** (see encryption note below).
- The `packet_id` option is **disabled** (see packet ID note below).

Typical ESPHome BTHome sensor types that work:

| ESPHome sensor      | Object ID | Works? |
|---------------------|-----------|--------|
| temperature         | `0x02`    | ✅      |
| humidity            | `0x03`    | ✅      |
| pressure            | `0x04`    | ✅      |
| illuminance         | `0x05`    | ✅      |
| uv_index            | `0x4A`    | ✅      |
| battery_percent     | `0x01`    | ✅      |
| wind_speed          | `0x44` / `0xD1` | ✅ |
| precipitation       | `0x5F`    | ✅      |

---

## Likely Compatible (BTHome v2, no packet ID)

These devices use standard BTHome v2 with measurements covered by the object
ID table. They should work but have not been explicitly tested.

| Device | Measurements | Notes |
|--------|-------------|-------|
| Any BTHome v2 DIY sensor | whatever you implement | Works if no unknown object IDs precede known ones |
| Inkbird IBS-TH3 Plus (BTHome mode) | temperature, humidity | Community-reported BTHome v2 support |

---

## Devices That Need a Minor Firmware Fix First

### Shelly BLU HT / BLU H&T Gen3 ⚠️

The Shelly BLU HT **does** transmit temperature, humidity, and battery via
BTHome v2 — all measurement types that the firmware handles. However, Shelly
devices prefix every advertisement with a **Packet ID** (`0x00`, 1 byte) used
for deduplication. This object ID is not in the firmware's lookup table, so the
parser stops immediately and no readings are extracted.

**Fix** (one line in `bthome.cpp` — see [`docs/adding_a_sensor.md`](adding_a_sensor.md)):

```cpp
// In s_objects[], add before the existing entries:
{ 0x00,  SENSOR_BATTERY, 0.0f, 1, false },  // Packet ID — parsed but discarded (value 0)
```

> The value `0` and `SENSOR_BATTERY` type are just placeholders; the endpoint
> will be created but ignored once the real battery reading (`0x01`) follows.
> A cleaner approach is to add a dedicated `SENSOR_IGNORE` type that skips
> endpoint creation. See `docs/adding_a_sensor.md` for details.

Once that fix is in place, the following Shelly devices become compatible:

| Device | Measurements |
|--------|-------------|
| Shelly BLU HT | temperature, humidity, battery |
| Shelly BLU H&T Gen3 | temperature, humidity, battery |
| Shelly BLU TRV | temperature, battery |

---

## Not Supported

### Binary / Event Sensors

The firmware only creates Matter endpoints for **numeric float measurements**.
Binary states (button press, door open/closed, motion detected) are not handled
because:

1. The relevant object IDs are not in `bthome.cpp`'s lookup table.
2. There is no Matter cluster mapping for generic binary events.

Affected devices: Shelly BLU Button1, Shelly BLU DoorWindow, Shelly BLU Motion.

### Encrypted Advertisements

BTHome v2 supports AES-CCM encryption (device-info byte, bit 0 = 1).
Encrypted packets are **silently dropped** — decryption requires a per-device
bind key that is not provisioned in this firmware.

> If you want to add encryption support, see the BTHome spec and add a
> per-device key store. For most passive sensors encryption is disabled by
> default.

### Non-BTHome BLE Sensors

Devices that use proprietary BLE protocols (Govee, Xiaomi MiBeacon, Ruuvi,
iNode, etc.) are not supported. They do not broadcast Service UUID `0xFCD2`
and are silently ignored by the scanner.

---

## Supported Measurement Types (Summary)

| Sensor type       | Object IDs        | Unit   | Matter cluster                  |
|-------------------|-------------------|--------|---------------------------------|
| Battery           | `0x01`            | %      | Flow Measurement (workaround)   |
| Temperature       | `0x02`            | °C     | Temperature Measurement         |
| Humidity          | `0x03`            | %      | Relative Humidity Measurement   |
| Pressure          | `0x04`            | hPa    | Pressure Measurement            |
| Illuminance       | `0x05`, `0x58`    | lux    | Illuminance Measurement         |
| UV Index          | `0x4A`            | index  | Flow Measurement (workaround)   |
| Wind Direction    | `0x44`, `0x5E`, `0xD2` | ° | Flow Measurement (workaround)  |
| Precipitation     | `0x5F`, `0xD4`    | mm/h   | Flow Measurement (workaround)   |
| Wind Speed (avg)  | `0xD1`            | m/s    | Flow Measurement (workaround)   |
| Wind Gust         | `0xD3`            | m/s    | Flow Measurement (workaround)   |

> Object IDs `0xD1`–`0xD4` are Ecowitt/Shelly WS90 proprietary.
> All others are standard [BTHome v2 spec](https://bthome.io/format/).

---

## Adding a New Device

See [`docs/adding_a_sensor.md`](adding_a_sensor.md) for a step-by-step guide.
Adding a new measurement type is typically a single row in the `s_objects[]`
table in `components/bthome/bthome.cpp`.
