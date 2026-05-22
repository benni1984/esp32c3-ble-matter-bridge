#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# build_and_release.sh
#
# Builds the firmware locally and creates a GitHub Release with the .bin files.
# Prerequisites: ESP-IDF v5.4 + esp-matter must be installed and sourced.
#
# Usage:
#   chmod +x build_and_release.sh
#   ./build_and_release.sh v1.0.0
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

VERSION="${1:-}"
if [ -z "$VERSION" ]; then
  echo "Usage: $0 <version>  (e.g. $0 v1.0.0)"
  exit 1
fi

# ── Check environment ─────────────────────────────────────────────────────────
if [ -z "${IDF_PATH:-}" ]; then
  echo "ERROR: IDF_PATH is not set."
  echo "Please run first:  source ~/esp/esp-idf/export.sh"
  exit 1
fi
if [ -z "${ESP_MATTER_PATH:-}" ]; then
  echo "ERROR: ESP_MATTER_PATH is not set."
  echo "Please run first:  source ~/esp/esp-matter/export.sh"
  exit 1
fi

# ── Build ─────────────────────────────────────────────────────────────────────
echo "▶  Building firmware (target: esp32c3)..."
idf.py set-target esp32c3
idf.py build

# ── Collect binaries ──────────────────────────────────────────────────────────
echo "▶  Collecting binary files..."
mkdir -p release_artifacts
cp build/bootloader/bootloader.bin           release_artifacts/bootloader.bin
cp build/partition_table/partition-table.bin  release_artifacts/partition-table.bin
cp build/ble_matter_bridge.bin                release_artifacts/ble_matter_bridge.bin

echo "   bootloader.bin         → 0x0000"
echo "   partition-table.bin    → 0x8000"
echo "   ble_matter_bridge.bin  → 0x20000"

# ── Create GitHub Release ─────────────────────────────────────────────────────
echo "▶  Creating GitHub Release '$VERSION'..."

# Create and push git tag
git tag -a "$VERSION" -m "Release $VERSION" 2>/dev/null || true
git push origin "$VERSION" 2>/dev/null || true

# Create release and upload binaries
gh release create "$VERSION" \
  release_artifacts/bootloader.bin \
  release_artifacts/partition-table.bin \
  release_artifacts/ble_matter_bridge.bin \
  --title "BLE-Matter-Bridge $VERSION" \
  --notes "Manually built from $(git rev-parse --short HEAD)"

echo ""
echo "✓ Release '$VERSION' created."
echo "  Web Installer: https://benni1984.github.io/esp32c3-ble-matter-bridge/"
