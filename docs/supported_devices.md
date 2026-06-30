# Supported Devices

This firmware is purpose-built to bridge the **Ecowitt WS90** weather station
into Matter via a **Shelly BLE relay** (Shelly PM Mini or similar).

The ESP32-C3 does not scan BLE directly — sensor data arrives over HTTP from
a Shelly device that acts as a BLE-to-cloud relay on the local network.

---

## How Data Reaches the ESP32

1. The WS90 broadcasts BTHome v2 BLE advertisements (Service UUID `0xFCD2`,
   MAC `FC:4D:6A:13:3D:0D`).
2. A Shelly PM Mini (at `192.168.1.81` or `192.168.1.173`) receives the
   advertisement and caches it locally.
3. The ESP32-C3 polls `http://<shelly_ip>/rpc/BLE.CloudRelay.ListInfos` every
   10 seconds, extracts the `fcd2` service-data field (base64-encoded), and
   decodes it with `bthome_parse()`.
4. Each recognised measurement type updates the corresponding Matter endpoint
   under the bridge.

Two Shelly relays are configured for redundancy — the first one to respond wins.

---

## Ecowitt WS90 "Powered by Shelly" ✅ (only supported device)

Solar-powered outdoor weather station.
The BTHome v2 payload carries all nine measurement types listed below.

| Endpoint | Measurement     | BTHome Object ID | Matter Cluster                | Apple Home | Home Assistant |
|----------|-----------------|-----------------|-------------------------------|------------|----------------|
| 1        | Battery         | `0x01`          | Flow Measurement (workaround) | ❌*         | ✅              |
| 2        | Temperature     | `0x02`          | Temperature Measurement       | ✅          | ✅              |
| 3        | Humidity        | `0x03`          | Relative Humidity Measurement | ✅          | ✅              |
| 4        | Pressure        | `0x04`          | Pressure Measurement          | ✅          | ✅              |
| 5        | Illuminance     | `0x05`          | Illuminance Measurement       | ✅          | ✅              |
| 6        | Wind Speed      | `0xD1`          | Flow Measurement (workaround) | ❌*         | ✅              |
| 7        | Wind Direction  | `0xD2`          | Flow Measurement (workaround) | ❌*         | ✅              |
| 8        | Rain            | `0xD4`          | Flow Measurement (workaround) | ❌*         | ✅              |
| 9        | UV Index        | `0x4A`          | Flow Measurement (workaround) | ❌*         | ✅              |

> \* Apple Home does not display Flow Measurement endpoints with a dedicated UI.
> The values are still present in the Matter fabric and accessible via
> Home Assistant or any Matter-compatible controller that queries all attributes.

Object IDs `0xD1`, `0xD2`, `0xD4` are Ecowitt/Shelly proprietary extensions
supported explicitly in `components/bthome/bthome.cpp`.

---

## Not Supported

### Direct BLE Sensors

The BLE scanner component has been removed. Devices that rely on direct BLE
scanning (Shelly BLU HT, ESPHome BTHome sensors, Ruuvi, Govee, etc.) are not
supported by this firmware.

To bridge a different BLE sensor, you would need to:
1. Connect it to a Shelly BLE relay that caches its advertisements.
2. Add the new sensor type to `bthome.h` and `bthome.cpp`.
3. Update `shelly_poller.cpp` to look up the correct MAC / service UUID.
4. Map the new type to a Matter cluster in `matter_bridge.cpp`.

See [`docs/adding_a_sensor.md`](adding_a_sensor.md) for the detailed steps.

### Thread / Matter-over-Thread

The ESP32-C3 has no IEEE 802.15.4 radio. Only WiFi Matter is supported.

### Binary / Event Sensors

The firmware only creates Matter endpoints for **numeric float measurements**.
Binary states (button press, door open/closed, motion) are not handled.

---

## Shelly Relay Requirements

Any Shelly device that exposes the `BLE.CloudRelay.ListInfos` RPC endpoint and
has the WS90 in range works as a relay. Tested with **Shelly PM Mini Gen3**.

The relay must be on the same IPv4 subnet (or routable from) the ESP32's WiFi
interface. The IPs are hardcoded in `main.cpp` — mDNS discovery is unreliable
across VLAN boundaries.
