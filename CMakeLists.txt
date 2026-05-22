cmake_minimum_required(VERSION 3.16)

# ESP_MATTER_PATH must be set before building.
# Run: source ~/esp/esp-matter/export.sh
if(NOT DEFINED ENV{ESP_MATTER_PATH})
    message(FATAL_ERROR
        "ESP_MATTER_PATH is not set.\n"
        "Please source esp-matter/export.sh before building:\n"
        "  source /path/to/esp-matter/export.sh")
endif()

# ESP-IDF project.cmake must come before project().
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(ble_matter_bridge VERSION 1.0.0)

# esp_matter.cmake must come AFTER project().
# It adds $ESP_MATTER_PATH/components to EXTRA_COMPONENT_DIRS so that
# esp_matter, esp_matter_console, and all other Matter components are found.
include($ENV{ESP_MATTER_PATH}/cmake/esp_matter.cmake)
