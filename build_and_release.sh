#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# build_and_release.sh
#
# Baut die Firmware lokal und erstellt einen GitHub Release mit den .bin-Dateien.
# Voraussetzungen: ESP-IDF v5.4 + esp-matter müssen installiert sein.
#
# Verwendung:
#   chmod +x build_and_release.sh
#   ./build_and_release.sh v1.0.0
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

VERSION="${1:-}"
if [ -z "$VERSION" ]; then
  echo "Verwendung: $0 <version>  (z.B. $0 v1.0.0)"
  exit 1
fi

# ── Umgebung aktivieren ───────────────────────────────────────────────────────
if [ -z "${IDF_PATH:-}" ]; then
  echo "ERROR: IDF_PATH nicht gesetzt."
  echo "Führe zuerst aus:  source ~/esp/esp-idf/export.sh"
  exit 1
fi
if [ -z "${ESP_MATTER_PATH:-}" ]; then
  echo "ERROR: ESP_MATTER_PATH nicht gesetzt."
  echo "Führe zuerst aus:  source ~/esp/esp-matter/export.sh"
  exit 1
fi

# ── Build ─────────────────────────────────────────────────────────────────────
echo "▶  Baue Firmware (Target: esp32c3)…"
idf.py set-target esp32c3
idf.py build

# ── Artefakte sammeln ─────────────────────────────────────────────────────────
echo "▶  Sammle Binärdateien…"
mkdir -p release_artifacts
cp build/bootloader/bootloader.bin           release_artifacts/bootloader.bin
cp build/partition_table/partition-table.bin  release_artifacts/partition-table.bin
cp build/ble_matter_bridge.bin                release_artifacts/ble_matter_bridge.bin

echo "   bootloader.bin         → 0x0000"
echo "   partition-table.bin    → 0x8000"
echo "   ble_matter_bridge.bin  → 0x20000"

# ── GitHub Release erstellen ──────────────────────────────────────────────────
echo "▶  Erstelle GitHub Release '$VERSION'…"

# Tag erstellen und pushen
git tag -a "$VERSION" -m "Release $VERSION" 2>/dev/null || true
git push origin "$VERSION" 2>/dev/null || true

# Release erstellen und Binärdateien hochladen
gh release create "$VERSION" \
  release_artifacts/bootloader.bin \
  release_artifacts/partition-table.bin \
  release_artifacts/ble_matter_bridge.bin \
  --title "BLE-Matter-Bridge $VERSION" \
  --notes "Manuell gebaut von $(git rev-parse --short HEAD)"

echo ""
echo "✓ Release '$VERSION' erstellt."
echo "  Web-Installer: https://benni1984.github.io/esp32c3-ble-matter-bridge/"
