# ESP32-C3 Matter Bridge — WS90 Weather Station

The **Ecowitt WS90** is a great solar-powered outdoor weather station, but its
data is normally locked behind the Ecowitt/WSView cloud app — there's no
official way to get it into Apple Home, Home Assistant, or any other
Matter-based smart home setup without a subscription or a vendor-specific
integration. This project fixes that: a $3 **ESP32-C3 Super Mini** turns your
WS90's readings into native, local Matter endpoints that any Matter
controller can pair with directly — no cloud account, no vendor app, no
gateway, no recurring cost. Point Apple Home or Home Assistant at it and the
weather station just shows up like any other smart home sensor.

Under the hood it's a small **Matter Bridge**: it polls WS90 data from a
Shelly BLE relay over HTTP and exposes the measurements as native Matter
devices on your local network.

```
[Ecowitt WS90]  ──BLE──►  [Shelly PM Mini]  ──HTTP──►  [ESP32-C3]  ──WiFi/Matter──►  [Apple Home]
                                                                                    ──►  [Home Assistant]
```

The WS90 is bridged via one or more Shelly PM Mini devices, found
automatically: on first start (and again if all known relays go
unreachable), the ESP32 scans its local subnet for hosts serving the Shelly
RPC API, confirms each candidate via a real `BLE.CloudRelay.ListInfos` call,
and polls whichever responds — no fixed IPs to configure. See
[Shelly discovery](#shelly-discovery) below.

Exposed Matter endpoints: Battery · Temperature · Humidity · Pressure ·
Illuminance · Wind Speed · Wind Direction · Rain · UV Index (9 endpoints total)

---

## Requirements

| What | Version / Source |
|------|-----------------|
| ESP-IDF | v5.4 |
| esp-matter | latest (`main` branch) |
| Python | ≥ 3.10 (required by ESP-IDF) |
| GitHub CLI | optional, needed for `build_and_release.sh` |

### Installing ESP-IDF + esp-matter

```bash
# 1. Clone ESP-IDF
git clone --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
cd ~/esp/esp-idf && git checkout v5.4 && ./install.sh
source ~/esp/esp-idf/export.sh

# 2. Clone esp-matter
git clone --recursive https://github.com/espressif/esp-matter.git ~/esp/esp-matter
cd ~/esp/esp-matter && ./install.sh
source ~/esp/esp-matter/export.sh
```

> **Tip:** Add both `source` commands to your `~/.bashrc` / `~/.zshrc` so the
> environment variables are set automatically in every new shell.

---

## Build and flash (CLI)

```bash
# Clone this repository
git clone https://github.com/benni1984/esp32c3-ble-matter-bridge.git
cd esp32c3-ble-matter-bridge

# Activate the environment (if not already done)
source ~/esp/esp-idf/export.sh
source ~/esp/esp-matter/export.sh

# Set the target chip
idf.py set-target esp32c3

# Build
idf.py build

# Flash (adjust port: COM3 on Windows, /dev/ttyUSB0 on Linux/macOS)
idf.py -p COM3 flash monitor
```

---

## Web Installer

The easiest way to flash — no driver, no CLI needed:

**[👉 benni1984.github.io/esp32c3-ble-matter-bridge](https://benni1984.github.io/esp32c3-ble-matter-bridge/)**

Requires **Chrome** or **Edge** (Web Serial API).
Firefox and Safari are not supported.

---

## Building firmware locally & creating a release

### Option A — Windows (recommended, no local ESP-IDF needed)

Requirements:
- [Docker Desktop](https://www.docker.com/products/docker-desktop/) (running)
- [GitHub CLI](https://cli.github.com/) (`gh auth login` once)

```powershell
.\build_docker.ps1 v1.0.0
```

The script runs the build inside the official `espressif/idf:v5.4` Docker container
(the same environment as CI), so no local ESP-IDF installation is needed.
esp-matter is cached in a Docker named volume — the **first run** downloads ~500 MB and
takes 20–30 minutes; **subsequent runs** are much faster.

### Option B — Linux / macOS (ESP-IDF + esp-matter installed locally)

```bash
# Activate environment (ESP-IDF + esp-matter must be installed)
source ~/esp/esp-idf/export.sh
source ~/esp/esp-matter/export.sh

# Build + create GitHub Release (requires the 'gh' CLI)
chmod +x build_and_release.sh
./build_and_release.sh v1.0.0
```

### Option C — GitHub Actions (CI)

Pushing a `v*` tag triggers the build automatically — no local toolchain needed:

```bash
git tag v1.0.0 && git push --tags
```

The first Docker-based CI build takes ~20 minutes (esp-matter + connectedhomeip setup).
Subsequent builds are significantly faster thanks to caching.

---

In all cases the script/CI builds the firmware, creates a git tag, and uploads the three
`.bin` files as a GitHub Release — the web installer will then work automatically.

---

## Initial setup (commissioning)

The ESP32-C3 acts only as a **BLE peripheral** (Matter commissioning advertisement).
It does **not** scan for BLE sensors — sensor data arrives over WiFi via the Shelly
HTTP relay.

The pairing code and discriminator are derived from the device's base MAC address
(FNV-1a hash → deterministic, no factory partition needed). For MAC
`70:AF:09:01:51:24` the discriminator is **1562**.

1. Flash the firmware (CLI or web installer).
2. The serial monitor prints a QR code string every 5 seconds for 3 minutes:
   ```
   Matter QR code data: MT:Y3.13OTB00KA0648G00
   Manual pairing code: 3497-982-7337
   ```
3. **Apple Home**: Add Accessory → More Options → scan the QR code
4. **Home Assistant**: Settings → Integrations → Matter → Add device → scan the QR code

After commissioning, the ESP32 starts the Shelly poller automatically and the
WS90 endpoints appear in your controller within seconds.

On subsequent **reboots** with existing commissioning the poller starts immediately —
no re-commissioning needed.

> **Apple Home tip:** direct Matter pairing (step 3, or sharing the device
> from HA to a second fabric) can fail with a generic "Pairing failed" —
> Apple's controller is stricter than Home Assistant's about the multi-endpoint,
> non-bridged topology this firmware uses (see [`CLAUDE.md`](CLAUDE.md) for why).
> The reliable path into Apple Home is via **Home Assistant's HomeKit Bridge
> integration** instead: pair the ESP32 with HA's Matter integration as
> above, then add HA's separate "HomeKit Bridge" integration (not the Matter
> one) and include the WS90 sensor entities in its filter. This re-exposes
> the already-working HA entities over classic HomeKit, sidestepping the
> Matter-topology conflict entirely. Only `temperature`, `humidity`, and
> `illuminance` have a native HomeKit sensor type — wind/rain/UV/pressure
> won't show up in Apple Home regardless of pairing method, since HomeKit
> itself has no accessory type for them.

---

## IPv6 note

`CONFIG_LWIP_IPV6_AUTOCONFIG=n` is set in `sdkconfig.defaults` to prevent the
device from obtaining a global IPv6 address. Matter CASE session establishment
must use IPv4; a global IPv6 address confuses some controllers.

---

## Monitoring sensor status (serial monitor)

```
I (1234) main:            BLE-Matter-Bridge starting
I (2345) main:            Already commissioned — starting Shelly poller
I (3456) shelly_poller:   WiFi up — starting Shelly poll loop (2 URL(s))
I (4567) shelly_poller:   WS90 poll OK: 9 readings
I (4567) main:            BLE [FC:4D:6A:13:3D:0D] WS90 readings=9
```

The Matter shell is also available via serial (enabled by `CONFIG_ENABLE_CHIP_SHELL=y`):
```
matter help
matter device factoryreset   # clears all fabric data → allows re-commissioning
```

---

## Adding a new measurement type

See [`docs/adding_a_sensor.md`](docs/adding_a_sensor.md) for a step-by-step guide
to exposing additional WS90 measurements (e.g. wind gust) through the Matter bridge.

---

## Known limitations

| Limitation | Reason |
|-----------|--------|
| WiFi Matter only (no Thread) | ESP32-C3 has no IEEE 802.15.4 radio |
| Wind, rain, UV visible in Home Assistant only | Matter 1.3 has no dedicated clusters for these |
| Single sensor source (WS90 via Shelly relay) | Only one WS90 payload/MAC is currently parsed |
| Subnet scan skipped on very large networks | Safety cap at 1024 hosts (see Shelly discovery) |
| Max 16 endpoints | Adjustable via `REGISTRY_MAX_SENSORS` in `sensor_registry.h` |

---

## Shelly discovery

No IPs to configure: `shelly_poller` scans the ESP32's own subnet for Shelly
relays automatically.

1. On first poll start it reads its own IP + netmask and computes the local
   subnet, then scans. If no relay has been found yet, it rescans on every
   10-second poll tick (nothing to lose by retrying while blind); once at
   least one relay is known, a rescan only happens if every known relay stops
   responding, throttled to at most once every 5 minutes.
2. It probes every host in that subnet for an open TCP port 80, in batches of
   8 concurrent non-blocking connects. The per-batch connect timeout is
   **800 ms** — tuned up from an initial 200 ms, which on real hardware missed
   known-good relays inconsistently due to WiFi/BLE radio-sharing jitter (see
   `shelly_poller.cpp` for the story). A full /24 scan takes roughly 25
   seconds — acceptable given how infrequently it runs — without saturating
   `CONFIG_LWIP_MAX_SOCKETS`.
3. Each host with port 80 open gets a real `GET /rpc/BLE.CloudRelay.ListInfos`
   request — only a genuine Shelly relay responding with valid WS90 data is
   added to the poll list. Everything else on port 80 (routers, other IoT
   gear) is silently ignored.

This assumes the ESP32 and the Shelly relays share one broadcast domain/subnet
(true for typical flat home networks, including WiFi mesh systems like Deco
where multiple SSIDs bridge onto the same subnet). Networks with genuine
VLAN-level isolation between the ESP32 and the Shellys won't be discoverable
this way — see `shelly_poller_add_url()` in `shelly_poller.h` to fall back to
a fixed IP in that case.

---

## Home Assistant: "Fluss (…)" naming and m³/h unit on wind/rain/UV/battery

Wind speed, wind direction, rain, UV index, and battery are all bridged through
Matter's **FlowMeasurement** cluster, since Matter has no dedicated clusters for
them (see Known Limitations above). Firmware v1.7.0+ attaches a **Fixed Label**
to each of these five endpoints (`ha_entitylabel` = `Wind Speed` / `Wind
Direction` / `Rain` / `UV Index` / `Battery`), which is why Home Assistant
shows them as distinguishable entities instead of `Flow (1)`, `Flow (6)`, etc.
(A **fresh commissioning** is required for this to take effect — HA only
applies Fixed Labels when an entity is first created, not retroactively via
re-interview. Remove and re-pair the device once if you're upgrading from an
older firmware version.)

However, two things are **hardcoded in Home Assistant's own Matter
integration** for any `FlowMeasurement`-backed entity and cannot be changed
from the device side at all:

- The name always gets a **"Flow"** (translated: "Fluss") prefix —
  `name = f"{name} ({label})"` in `homeassistant/components/matter/entity.py`.
- The unit is always **m³/h** —
  `native_unit_of_measurement=UnitOfVolumeFlowRate.CUBIC_METERS_PER_HOUR` in
  `homeassistant/components/matter/sensor.py`.

The underlying *values* are correct (wind speed in m/s, wind direction in °,
rain in mm, battery in %) — only the label and unit HA displays are wrong,
because the Matter cluster itself is semantically "flow rate", not what we're
actually carrying over it. To get a properly named/unitized sensor, wrap the
raw entity in a [template sensor](https://www.home-assistant.io/integrations/template/)
in Home Assistant (`configuration.yaml` or a packages file) — no firmware
change needed, and it survives future re-pairings:

```yaml
template:
  - sensor:
      - name: "WS90 Wind Speed"
        unique_id: ws90_wind_speed
        # Replace with your actual entity_id (Developer Tools → States).
        state: "{{ states('sensor.ws90_weather_bridge_fluss_wind_speed') }}"
        unit_of_measurement: "m/s"
        device_class: wind_speed
        state_class: measurement

      - name: "WS90 Wind Direction"
        unique_id: ws90_wind_direction
        state: "{{ states('sensor.ws90_weather_bridge_fluss_wind_direction') }}"
        unit_of_measurement: "°"
        state_class: measurement

      - name: "WS90 Rain"
        unique_id: ws90_rain
        state: "{{ states('sensor.ws90_weather_bridge_fluss_rain') }}"
        unit_of_measurement: "mm"
        state_class: measurement

      - name: "WS90 UV Index"
        unique_id: ws90_uv_index
        state: "{{ states('sensor.ws90_weather_bridge_fluss_uv_index') }}"
        state_class: measurement

      - name: "WS90 Battery"
        unique_id: ws90_battery
        state: "{{ states('sensor.ws90_weather_bridge_fluss_battery') }}"
        unit_of_measurement: "%"
        device_class: battery
        state_class: measurement
```

After adding this, hide or ignore the original `Flow (…)` entities on your
dashboard and use the new `WS90 …` ones instead.

---

## License

MIT
