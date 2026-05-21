cmake_minimum_required(VERSION 3.16)

# ESP_MATTER_PATH must be set before including ESP-IDF cmake.
# Run: source $ESP_MATTER_PATH/export.sh  (or the install script in docs/setup.md)
if(NOT DEFINED ENV{ESP_MATTER_PATH})
    message(FATAL_ERROR
        "ESP_MATTER_PATH is not set.\n"
        "Please source esp-matter/export.sh before building:\n"
        "  source /path/to/esp-matter/export.sh")
endif()

set(ENV{IDF_COMPONENT_MANAGER} "0")  # use local components, not the component registry

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(ble_matter_bridge VERSION 1.0.0)
