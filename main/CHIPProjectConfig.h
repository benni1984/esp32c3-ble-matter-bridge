#pragma once

// ─── Matter Device Identity ───────────────────────────────────────────────────
// VID 0xFFF1 / PID 0x8000 are Espressif test IDs.
// Replace with your own Vendor ID from the CSA if you plan to distribute.
#define CHIP_DEVICE_CONFIG_DEVICE_VENDOR_ID       0xFFF1
#define CHIP_DEVICE_CONFIG_DEVICE_PRODUCT_ID      0x8000
#define CHIP_DEVICE_CONFIG_DEVICE_PRODUCT_NAME    "WS90 Weather Bridge"
#define CHIP_DEVICE_CONFIG_DEVICE_VENDOR_NAME     "multihead.de"
#define CHIP_DEVICE_CONFIG_DEVICE_HARDWARE_VERSION 1
#define CHIP_DEVICE_CONFIG_DEVICE_HARDWARE_VERSION_STRING "ESP32-C3"
#define CHIP_DEVICE_CONFIG_DEVICE_SOFTWARE_VERSION 181
#define CHIP_DEVICE_CONFIG_DEVICE_SOFTWARE_VERSION_STRING "1.5.81"

// Device type: TemperatureSensor (0x0302) — primary sensor type.
// Do NOT use 0x000E (Bridge/Aggregator): matter-server sees DT=000E in mDNS
// and treats ALL sub-endpoints as bridge children, subscribing only to
// Identify+BDI attributes and skipping measurement clusters → HA only sees
// Identify buttons, never sensor entities.
#define CHIP_DEVICE_CONFIG_DEVICE_TYPE            0x0302

// ─── Commissioning ────────────────────────────────────────────────────────────
// Discriminator and passcode are randomly generated at first boot and
// stored in NVS.  You can override them here for development.
// #define CHIP_DEVICE_CONFIG_USE_TEST_SETUP_PIN_CODE  20202021
// #define CHIP_DEVICE_CONFIG_USE_TEST_SETUP_DISCRIMINATOR 0xF00

// ─── Memory ───────────────────────────────────────────────────────────────────
// 12 buffers: 10 exhausted during StatusReport after successful CASE (BLE still active).
// 15 pre-allocates 5 KB more heap which starves mbedTLS P256 ECDH (BIGNUM OOM).
// 12 gives 2 extra buffers for StatusReport while keeping ~2 KB margin for ECDH.
// 25 caused abort() in AES/SHA crypto.
#define CHIP_SYSTEM_CONFIG_PACKETBUFFER_POOL_SIZE 12

// ─── Debug ────────────────────────────────────────────────────────────────────
#define CHIP_DEVICE_CONFIG_ENABLE_TEST_DEVICE_IDENTITY  0
