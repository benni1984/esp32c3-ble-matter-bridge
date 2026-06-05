idf_component_register(
    SRCS "matter_bridge.cpp" "mac_commissioning_data_provider.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        bthome
        sensor_registry
        esp_matter
        esp_matter_console
        nvs_flash
        log
        esp_wifi
        esp_event
        esp_hw_support
)
