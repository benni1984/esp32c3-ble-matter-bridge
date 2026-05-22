<#
.SYNOPSIS
    Builds the BLE-Matter-Bridge firmware using Docker and creates a GitHub Release.

.DESCRIPTION
    Runs the build inside the official espressif/idf:v5.4 Docker container
    (same environment as CI), so no local ESP-IDF installation is needed on Windows.
    esp-matter is cached in a Docker named volume between runs.

    Requirements:
      - Docker Desktop  https://www.docker.com/products/docker-desktop/
      - GitHub CLI      https://cli.github.com/

.PARAMETER Version
    Release version tag, e.g. v1.0.0

.EXAMPLE
    .\build_docker.ps1 v1.0.0
#>

param(
    [Parameter(Mandatory = $true, HelpMessage = "Version tag, e.g. v1.0.0")]
    [string]$Version
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# ── Prerequisite checks ───────────────────────────────────────────────────────

if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
    Write-Host ""
    Write-Host "ERROR: Docker not found." -ForegroundColor Red
    Write-Host "Install Docker Desktop from: https://www.docker.com/products/docker-desktop/" -ForegroundColor Yellow
    Write-Host "Make sure Docker Desktop is running before retrying." -ForegroundColor Yellow
    exit 1
}

if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
    Write-Host ""
    Write-Host "ERROR: GitHub CLI (gh) not found." -ForegroundColor Red
    Write-Host "Install from: https://cli.github.com/" -ForegroundColor Yellow
    exit 1
}

# ── Verify Docker is running ──────────────────────────────────────────────────

try {
    docker info 2>&1 | Out-Null
} catch {
    Write-Host "ERROR: Docker Desktop is not running. Please start it and retry." -ForegroundColor Red
    exit 1
}

$ProjectPath = (Get-Location).Path
Write-Host ""
Write-Host "BLE-Matter-Bridge firmware build" -ForegroundColor Cyan
Write-Host "  Project : $ProjectPath"
Write-Host "  Version : $Version"
Write-Host "  Image   : espressif/idf:v5.4"
Write-Host "  Cache   : Docker volume 'esp-matter-cache'"
Write-Host ""
Write-Host "Note: First run downloads esp-matter (~500 MB) and may take 20-30 minutes."
Write-Host "      Subsequent runs use the cached volume and are much faster."
Write-Host ""

# ── Bash script that runs inside the container ────────────────────────────────
# Single-quoted here-string: PowerShell will NOT expand $variables,
# so bash receives them as-is.

$bashScript = @'
set -e

echo "=== Checking esp-matter cache ==="
if [ ! -f /opt/esp/esp-matter/export.sh ]; then
    echo "--- Cloning esp-matter (first run)..."
    git clone --depth 1 \
        https://github.com/espressif/esp-matter.git \
        /opt/esp/esp-matter
    cd /opt/esp/esp-matter
    git submodule update --init --depth 1 \
        connectedhomeip/connectedhomeip
    echo "--- Running esp-matter install.sh..."
    ./install.sh
else
    echo "--- esp-matter cache found, skipping clone."
fi

echo ""
echo "=== Activating toolchains ==="
. $IDF_PATH/export.sh
. /opt/esp/esp-matter/export.sh

echo ""
echo "=== Building firmware ==="
cd /project
idf.py set-target esp32c3
idf.py build

echo ""
echo "=== Collecting binaries ==="
mkdir -p /project/release_artifacts
cp build/bootloader/bootloader.bin           /project/release_artifacts/bootloader.bin
cp build/partition_table/partition-table.bin  /project/release_artifacts/partition-table.bin
cp build/ble_matter_bridge.bin                /project/release_artifacts/ble_matter_bridge.bin

echo ""
echo "=== Flash offsets ==="
echo "  bootloader.bin         -> 0x0000"
echo "  partition-table.bin    -> 0x8000"
echo "  ble_matter_bridge.bin  -> 0x20000"
echo ""
echo "Build complete!"
'@

# ── Run build in Docker ───────────────────────────────────────────────────────

Write-Host "Starting Docker build..." -ForegroundColor Cyan

docker run --rm `
    --volume "${ProjectPath}:/project" `
    --volume "esp-matter-cache:/opt/esp/esp-matter" `
    --workdir "/project" `
    espressif/idf:v5.4 `
    bash -c $bashScript

if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host "ERROR: Docker build failed (exit code $LASTEXITCODE)." -ForegroundColor Red
    Write-Host "Check the output above for details." -ForegroundColor Yellow
    exit $LASTEXITCODE
}

# ── Create GitHub Release ─────────────────────────────────────────────────────

Write-Host ""
Write-Host "Creating GitHub Release '$Version'..." -ForegroundColor Cyan

# Create and push the git tag (ignore errors if it already exists)
git tag -a $Version -m "Release $Version" 2>$null
git push origin $Version 2>$null

$shortHash = git rev-parse --short HEAD
$releaseNotes = "Firmware built via Docker (espressif/idf:v5.4) from commit ``$shortHash``."

gh release create $Version `
    "release_artifacts/bootloader.bin" `
    "release_artifacts/partition-table.bin" `
    "release_artifacts/ble_matter_bridge.bin" `
    --title "BLE-Matter-Bridge $Version" `
    --notes $releaseNotes

if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host "ERROR: Failed to create GitHub Release." -ForegroundColor Red
    Write-Host "The binaries are in .\release_artifacts\ — you can upload them manually." -ForegroundColor Yellow
    exit $LASTEXITCODE
}

Write-Host ""
Write-Host "Release '$Version' created successfully!" -ForegroundColor Green
Write-Host "  Web Installer: https://benni1984.github.io/esp32c3-ble-matter-bridge/" -ForegroundColor Cyan
Write-Host ""
