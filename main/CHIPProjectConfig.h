#pragma once

// ─── Matter Device Identity ───────────────────────────────────────────────────
// VID 0xFFF1 / PID 0x8000 are Espressif test IDs.
// Replace with your own Vendor ID from the CSA if you plan to distribute.
#define CHIP_DEVICE_CONFIG_DEVICE_VENDOR_ID       0xFFF1
#define CHIP_DEVICE_CONFIG_DEVICE_PRODUCT_ID      0x8000
#define CHIP_DEVICE_CONFIG_DEVICE_PRODUCT_NAME    "BLE-Matter-Bridge"
#define CHIP_DEVICE_CONFIG_DEVICE_VENDOR_NAME     "DIY"
#define CHIP_DEVICE_CONFIG_DEVICE_HARDWARE_VERSION 1
#define CHIP_DEVICE_CONFIG_DEVICE_SOFTWARE_VERSION 1
#define CHIP_DEVICE_CONFIG_DEVICE_SOFTWARE_VERSION_STRING "1.0.0"

// Device type: Bridge (0x000E as per Matter spec)
#define CHIP_DEVICE_CONFIG_DEVICE_TYPE            0x000E

// ─── Commissioning ────────────────────────────────────────────────────────────
// Discriminator and passcode are randomly generated at first boot and
// stored in NVS.  You can override them here for development.
// #define CHIP_DEVICE_CONFIG_USE_TEST_SETUP_PIN_CODE  20202021
// #define CHIP_DEVICE_CONFIG_USE_TEST_SETUP_DISCRIMINATOR 0xF00

// ─── Memory ───────────────────────────────────────────────────────────────────
// Default pool size of 8 exhausts under concurrent CASE sessions on ESP32-C3.
#define CHIP_SYSTEM_CONFIG_PACKETBUFFER_POOL_SIZE 25

// ─── Debug ────────────────────────────────────────────────────────────────────
#define CHIP_DEVICE_CONFIG_ENABLE_TEST_DEVICE_IDENTITY  0
