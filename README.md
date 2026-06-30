# ESP32-C3 Matter Bridge — WS90 Weather Station

An ESP32-C3 Super Mini acts as a **Matter Bridge**: it polls WS90 weather-station
data from a Shelly BLE relay over HTTP and exposes the measurements as native
Matter devices on your local network — no cloud, no gateway required.

```
[Ecowitt WS90]  ──BLE──►  [Shelly PM Mini]  ──HTTP──►  [ESP32-C3]  ──WiFi/Matter──►  [Apple Home]
                                                                                    ──►  [Home Assistant]
```

The WS90 is bridged via two redundant Shelly PM Mini devices at
`192.168.1.81` and `192.168.1.173` — the ESP32 polls both and uses whichever
responds first (`BLE.CloudRelay.ListInfos` RPC).

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
| Single sensor source (WS90 via Shelly relay) | Shelly IPs are hardcoded; mDNS is unreliable across VLANs |
| Max 16 endpoints | Adjustable via `REGISTRY_MAX_SENSORS` in `sensor_registry.h` |

---

## License

MIT
