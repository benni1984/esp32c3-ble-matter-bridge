cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED ENV{ESP_MATTER_PATH})
    message(FATAL_ERROR
        "ESP_MATTER_PATH is not set.\n"
        "Please source esp-matter/export.sh before building:\n"
        "  source /path/to/esp-matter/export.sh")
endif()

set(ESP_MATTER_PATH $ENV{ESP_MATTER_PATH})
set(MATTER_SDK_PATH ${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip)

# project.cmake sets up IDF cmake hooks — must come before project().
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# EXTRA_COMPONENT_DIRS must be set BEFORE project() so the IDF component
# scanner (which runs inside project()) finds esp_matter, esp_matter_console,
# esp_matter_bridge, and the connectedhomeip ESP32 config components.
set(EXTRA_COMPONENT_DIRS
    "${ESP_MATTER_PATH}/components"
    "${MATTER_SDK_PATH}/config/esp32/components")

project(ble_matter_bridge VERSION 1.0.0)

# Compile flags required by the connectedhomeip / Matter SDK (after project()).
idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H;-fpermissive;-Wno-overloaded-virtual" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)
# -Wno-error=unused-result  silences GCC 14 nodiscard strictness in ESP32DnssdImpl.cpp
idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security;-Wno-error=cpp;-Wno-error=unused-result" APPEND)
