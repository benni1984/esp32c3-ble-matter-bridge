# Supported Devices

This firmware bridges **any BTHome v2 BLE device** into Matter via one or
more **Shelly BLE relays** (Shelly PM Mini or similar) — no source-code
change needed to add a new physical device. The Ecowitt WS90 weather station
is the reference example below, but any BTHome v2 broadcaster works the same
way.

The ESP32-C3 does not scan BLE directly — sensor data arrives over HTTP from
Shelly device(s) that act as BLE-to-cloud relays on the local network.

---

## How Data Reaches the ESP32

1. Each BTHome v2 device broadcasts BLE advertisements (Service UUID
   `0xFCD2`).
2. Any Shelly device on the local network receives the advertisements and
   caches them locally, keyed by MAC. The ESP32 finds Shelly relays
   automatically — no IPs to configure, see
   [Shelly discovery](../README.md#shelly-discovery) in the main README.
3. The ESP32-C3 polls **every known Shelly relay** every 10 seconds
   (`http://<shelly_ip>/rpc/BLE.CloudRelay.ListInfos`), and for **every
   device** in each relay's response extracts the `fcd2` service-data field
   (base64-encoded) and decodes it with `bthome_parse()`.
4. Each recognised measurement type updates the corresponding Matter
   endpoint.

If multiple relays cache different, disjoint sets of devices (e.g. one relay
per floor), all of them are polled every cycle — not just the first one that
answers.

> **Upgrading from a firmware version older than the device-management
> commands (`block`/`unblock`/`stale`)?** Run `sensor_reg clear` once after
> flashing, then reboot. The registry's on-flash format grew slightly to add
> per-device "last seen" tracking; an older, smaller saved registry doesn't
> parse correctly against the new format. Nothing is lost beyond the
> registry itself — every device gets re-learned automatically from live BLE
> traffic within a few poll cycles.

---

## Adding a brand-new physical device

**Unencrypted devices** (most consumer BTHome sensors) need nothing —
just get the device within range of an already-discovered (or discoverable)
Shelly relay. The firmware parses it automatically and logs a "new sensor
type observed" line per measurement it sees.

**Encrypted devices** additionally need their 32-character AES-128 bind key
configured *before* they can be parsed at all — there's no way to derive a
key from the advertisement itself. Two ways to set it:
- **Web installer**: after flashing, the serial monitor stays connected and
  automatically shows a dropdown + key input for any device it sees a
  `No bindkey for <MAC>` warning for.
- **Serial console**: `bthome_key set <MAC> <32-hex-char key>` (find the key
  on the device's label or in its pairing app).

**Important — new devices/measurements only appear in Matter after a
reboot.** Matter requires the full endpoint set to exist before a
controller connects (an endpoint added mid-session crashes Home Assistant's
matter.js while it reads `descriptor.deviceTypeList`). So a genuinely new
device — or a known device broadcasting a measurement type it's never sent
before — is logged and persisted immediately, but only gets a live Matter
endpoint at the *next* boot. Reboot the ESP32 (power-cycle, or `reboot` on
the console) once you've added a device or set its bind key.

**Friendly naming**: if the device broadcasts a BLE "Local Name" (many do —
Shelly BLU H&T/Button, various ATC/Xiaomi-firmware sensors — Shelly's relay
reports it in the `name` field alongside `sdata`), that's used as the
default name automatically. Otherwise it falls back to `BTHome-XXXXXX`
(last 3 MAC bytes) — this is the case for the Ecowitt WS90, which doesn't
advertise a name. Either way, override anytime with
`sensor_reg name <MAC> <friendly name>` over the console.

**Cap**: up to 16 devices total (`REGISTRY_MAX_SENSORS`), each with as many
sensor types as its BTHome payload contains. If the table is full when a
genuinely new device shows up, the **least-recently-seen** entry is
automatically evicted to make room — a device that's stopped broadcasting
naturally yields its slot over time, with no manual step needed.

**Don't want a device tracked?** (e.g. a neighbor's sensor, picked up by a
Shelly relay near a shared wall): `sensor_reg block <MAC>` over the console,
or the web installer's "Manage devices" panel, stops the firmware from
tracking its readings — its Matter endpoints disappear at the next reboot.
`sensor_reg unblock <MAC>` reverses it. A persistently-broadcasting blocked
device is *not* affected by the automatic eviction above (it's always
"recently seen" too) — blocking is the actual way to permanently exclude one.

**Cleaning up devices that are gone for good** (removed, dead battery):
`sensor_reg stale [N]` (or the web installer panel's "Show last seen age"
toggle) lists devices not seen in at least N boot cycles (default 10) —
purely informational, nothing is deleted automatically. Review the list and
`sensor_reg del <MAC>` the ones you don't want anymore, same as before.

**Web installer "Manage devices" panel**: after connecting the serial
monitor, click "Load device list" to see every registered device with its
block/stale status, and Block/Unblock/Delete buttons per row — sends the
same `sensor_reg` commands above over the serial connection instead of
requiring a separate terminal.

---

## Ecowitt WS90 "Powered by Shelly" ✅ Reference example

Solar-powered outdoor weather station. `bthome_parse()` recognises the
following BTHome Object IDs from its payload — this same mapping applies to
*any* device broadcasting these object IDs, not just the WS90:

| Measurement     | BTHome Object ID | Matter Cluster                | Apple Home | Home Assistant |
|-----------------|-------------------|--------------------------------|------------|-----------------|
| Battery         | `0x01`            | Flow Measurement (workaround) | ❌*         | ✅              |
| Temperature     | `0x02` / `0x45`   | Temperature Measurement       | ✅          | ✅              |
| Humidity        | `0x03` / `0x2E`   | Relative Humidity Measurement | ✅          | ✅              |
| Pressure        | `0x04`            | Pressure Measurement          | ❌**        | ✅              |
| Illuminance     | `0x05`            | Illuminance Measurement       | ✅          | ✅              |
| Wind Speed      | `0x44`            | Flow Measurement (workaround) | ❌*         | ✅              |
| Wind Direction  | `0x5E`            | Flow Measurement (workaround) | ❌*         | ✅              |
| Rain            | `0x20` / `0x5F`   | Flow Measurement (workaround) | ❌*         | ✅              |
| UV Index        | `0x46`            | Flow Measurement (workaround) | ❌*         | ✅              |

> \* Apple Home does not display Flow Measurement endpoints with a dedicated UI.
> \*\* Apple HomeKit has no native accessory type for barometric pressure at all.
> Both are still present in the Matter fabric and accessible via Home Assistant
> or any Matter-compatible controller that queries all attributes — or via
> Home Assistant's separate **HomeKit Bridge** integration, which re-exposes
> HA's already-working entities to Apple Home over classic HomeKit instead of
> Matter (see `CLAUDE.md`'s "Apple Home Can't Direct-Pair This Topology"
> section for why direct Matter pairing into Apple Home is unreliable here).

Two additional object IDs are parsed but **not** currently exposed as a
Matter endpoint, since they're distinct physical quantities that must not be
confused with the readings above (see the comment in `bthome.cpp`):

| BTHome Object ID | Measurement        | Why no endpoint |
|-------------------|---------------------|------------------|
| `0x08`            | Dewpoint            | Different physical quantity from outdoor temperature (`0x02`/`0x45`) — see [`docs/adding_a_sensor.md`](adding_a_sensor.md) for how to give it one |
| `0x0C`            | Capacitor voltage   | Different physical quantity from battery percentage (`0x01`) |

All object IDs above are official [BTHome v2](https://bthome.io/format/)
identifiers, not proprietary Ecowitt/Shelly extensions — so any other device
using them (e.g. a Shelly BLU H&T's temperature/humidity, or a Shelly BLU
Button's battery level) is recognised automatically, no code changes needed.

---

## Not Supported

### A BTHome Object ID This Firmware Doesn't Recognise Yet

`bthome.cpp`'s object-ID table (`s_objects[]`) only covers the object IDs
listed above. A device broadcasting a measurement type not in that table
(e.g. CO2, VOC, dew point as its own endpoint) needs the table extended —
see [`docs/adding_a_sensor.md`](adding_a_sensor.md) for the steps. This is a
one-time addition that then works for *any* device broadcasting that object
ID, not a per-device change.

### Direct BLE Sensors (no Shelly relay)

The BLE scanner component has been removed — the ESP32-C3 only receives BLE
data relayed over HTTP by a Shelly. A BTHome device with no Shelly relay in
range isn't reachable, regardless of how standard its payload is.

### Thread / Matter-over-Thread

The ESP32-C3 has no IEEE 802.15.4 radio. Only WiFi Matter is supported.

### Binary / Event Sensors

The firmware only creates Matter endpoints for **numeric float measurements**.
Binary states (button press, door open/closed, motion) are not handled.

---

## Shelly Relay Requirements

Any Shelly device that exposes the `BLE.CloudRelay.ListInfos` RPC endpoint
works as a relay for any BTHome device in its range. Tested with **Shelly PM
Mini Gen3**.

Each relay must be reachable via plain HTTP (port 80) from the ESP32's WiFi
interface, on the same broadcast domain/subnet — the ESP32 finds them via an
automatic subnet scan, no fixed IP required. See
[Shelly discovery](../README.md#shelly-discovery) in the main README for how
this works and its one real limitation (networks with true VLAN isolation
between the ESP32 and the Shellys).
