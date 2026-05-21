idf_component_register(
    SRCS "matter_bridge.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        bthome
        sensor_registry
        esp_matter
        esp_matter_console
        nvs_flash
        esp_log
        esp_wifi
        esp_event
)
