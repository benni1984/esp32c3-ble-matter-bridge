# ESP32-C3 BLE-Matter-Bridge

Ein ESP32-C3 Super Mini fungiert als **Matter Bridge**: Er empfängt BLE-Advertisements
von Sensoren (Shelly BLU, Ecowitt WS90 u.a.) und stellt sie als echte Matter-Geräte
im lokalen Netz bereit — ohne Cloud, ohne Gateway.

```
[Shelly BLU / WS90]  ──BLE──►  [ESP32-C3]  ──WiFi/Matter──►  [Apple Home]
                                                             ──►  [Home Assistant]
```

Unterstützte Messwerte: Temperatur · Luftfeuchtigkeit · Luftdruck · Helligkeit ·
Windgeschwindigkeit · Windrichtung · Niederschlag · UV-Index · Batterie

---

## Voraussetzungen

| Was | Version / Quelle |
|-----|-----------------|
| ESP-IDF | v5.4 |
| esp-matter | aktuell (`main`-Branch) |
| Python | ≥ 3.10 (für ESP-IDF) |
| GitHub CLI | optional, für `gh`-Befehle |

### ESP-IDF + esp-matter installieren

```bash
# 1. ESP-IDF klonen
git clone --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
cd ~/esp/esp-idf && git checkout v5.4 && ./install.sh
source ~/esp/esp-idf/export.sh

# 2. esp-matter klonen
git clone --recursive https://github.com/espressif/esp-matter.git ~/esp/esp-matter
cd ~/esp/esp-matter && ./install.sh
source ~/esp/esp-matter/export.sh
```

> Tipp: Beide `source`-Befehle in dein `~/.bashrc` / `~/.zshrc` eintragen,
> damit die Umgebungsvariablen in jeder neuen Shell gesetzt sind.

---

## Projekt bauen und flashen

```bash
# Dieses Repo klonen
git clone https://github.com/benni1984/esp32c3-ble-matter-bridge.git
cd esp32c3-ble-matter-bridge

# Umgebung aktivieren (falls noch nicht geschehen)
source ~/esp/esp-idf/export.sh
source ~/esp/esp-matter/export.sh

# Zielplattform setzen
idf.py set-target esp32c3

# Bauen
idf.py build

# Flashen (USB-Port anpassen: COM3 unter Windows, /dev/ttyUSB0 unter Linux)
idf.py -p COM3 flash monitor
```

---

## Web-Installer

Der einfachste Weg zum Flashen — kein Treiber, keine CLI:

**[👉 benni1984.github.io/esp32c3-ble-matter-bridge](https://benni1984.github.io/esp32c3-ble-matter-bridge/)**

Benötigt **Chrome** oder **Edge** (Web Serial API).
Firefox und Safari werden nicht unterstützt.

---

## Firmware lokal bauen

Wenn du die Firmware selbst kompilieren und einen Release erstellen möchtest:

```bash
# Umgebung aktivieren (ESP-IDF + esp-matter müssen installiert sein)
source ~/esp/esp-idf/export.sh
source ~/esp/esp-matter/export.sh

# Bauen + Release auf GitHub erstellen (braucht 'gh' CLI)
chmod +x build_and_release.sh
./build_and_release.sh v1.0.0
```

Das Script baut die Firmware, erstellt einen Git-Tag und lädt die drei `.bin`-Dateien
als GitHub Release hoch — danach funktioniert der Web-Installer automatisch.

**GitHub Actions (CI):** Alternativ genügt ein Tag-Push zum Auslösen des Builds:
```bash
git tag v1.0.0 && git push --tags
```
Der CI-Build mit Docker dauert beim ersten Mal ~20 Minuten (esp-matter + connectedhomeip).
Folgebuilds sind durch Caching deutlich schneller.

---

## Ersteinrichtung (Commissioning)

1. Nach dem Flashen erscheint im Serial Monitor ein QR-Code-Datensatz:
   ```
   Matter QR code data: MT:Y3.13OTB00KA0648G00
   Manual pairing code: 3497-982-7337
   ```
2. **Apple Home**: Neues Gerät hinzufügen → Mehr Optionen → QR-Code scannen
3. **Home Assistant**: Einstellungen → Integrationen → Matter → Gerät hinzufügen → QR-Code scannen

Nach dem Commissioning startet der ESP32 automatisch mit dem BLE-Scan.
Sobald ein BTHome-Sensor in Reichweite ist, erscheint er innerhalb weniger Sekunden
als neues Gerät in Apple Home / Home Assistant.

---

## Sensor-Status verfolgen (Serial Monitor)

```
I (1234) main:    BLE-Matter-Bridge starting
I (2345) bthome:    temperature = 21.45
I (2345) bthome:    humidity    = 58.12
I (2345) matter_bridge: New sensor registered: WS90-A4B2
I (2345) matter_bridge: Created endpoint 2 for WS90-A4B2 / temperature
I (2345) matter_bridge: Created endpoint 3 for WS90-A4B2 / humidity
```

Im Serial Monitor stehen außerdem die Matter-Shell-Befehle zur Verfügung
(aktiviert über `CONFIG_ENABLE_CHIP_SHELL=y`):
```
matter help
matter device factoryreset   # löscht alle Fabric-Daten → ermöglicht neue Einrichtung
```

---

## Einen neuen Sensor hinzufügen

Lies [`docs/adding_a_sensor.md`](docs/adding_a_sensor.md) für eine Schritt-für-Schritt-Anleitung.

---

## Bekannte Einschränkungen

| Einschränkung | Grund |
|--------------|-------|
| Nur WiFi Matter (kein Thread) | ESP32-C3 hat kein IEEE 802.15.4 Radio |
| Wind, Regen, UV nur in Home Assistant sichtbar | Matter 1.3 hat keine dedizierten Cluster dafür |
| Verschlüsselte BTHome-Pakete werden übersprungen | Erfordert geräteindividuellen Schlüssel |
| Max. 16 Sensoren | Änderbar via `REGISTRY_MAX_SENSORS` in `sensor_registry.h` |

---

## Lizenz

MIT
