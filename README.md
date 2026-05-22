# ESP32-C3 BLE-Matter-Bridge

An ESP32-C3 Super Mini acts as a **Matter Bridge**: it receives BLE advertisements
from sensors (Shelly BLU, Ecowitt WS90, and others) and exposes them as native Matter
devices on your local network — no cloud, no gateway required.

```
[Shelly BLU / WS90]  ──BLE──►  [ESP32-C3]  ──WiFi/Matter──►  [Apple Home]
                                                             ──►  [Home Assistant]
```

Supported measurements: Temperature · Humidity · Pressure · Illuminance ·
Wind Speed · Wind Direction · Rainfall · UV Index · Battery

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

1. After flashing, the serial monitor prints a QR code string:
   ```
   Matter QR code data: MT:Y3.13OTB00KA0648G00
   Manual pairing code: 3497-982-7337
   ```
2. **Apple Home**: Add Accessory → More Options → scan the QR code
3. **Home Assistant**: Settings → Integrations → Matter → Add device → scan the QR code

After commissioning, the ESP32 automatically starts BLE scanning.
As soon as a BTHome sensor comes into range, it appears within a few seconds
as a new device in Apple Home / Home Assistant.

---

## Monitoring sensor status (serial monitor)

```
I (1234) main:          BLE-Matter-Bridge starting
I (2345) bthome:        temperature = 21.45
I (2345) bthome:        humidity    = 58.12
I (2345) matter_bridge: New sensor registered: WS90-A4B2
I (2345) matter_bridge: Created endpoint 2 for WS90-A4B2 / temperature
I (2345) matter_bridge: Created endpoint 3 for WS90-A4B2 / humidity
```

The Matter shell is also available via serial (enabled by `CONFIG_ENABLE_CHIP_SHELL=y`):
```
matter help
matter device factoryreset   # clears all fabric data → allows re-commissioning
```

---

## Adding a new sensor

See [`docs/adding_a_sensor.md`](docs/adding_a_sensor.md) for a step-by-step guide.

---

## Known limitations

| Limitation | Reason |
|-----------|--------|
| WiFi Matter only (no Thread) | ESP32-C3 has no IEEE 802.15.4 radio |
| Wind, rain, UV visible in Home Assistant only | Matter 1.3 has no dedicated clusters for these |
| Encrypted BTHome packets are skipped | Requires a per-device binding key |
| Max 16 sensors | Adjustable via `REGISTRY_MAX_SENSORS` in `sensor_registry.h` |

---

## License

MIT
