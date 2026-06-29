idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        shelly_poller
        bthome
        sensor_registry
        matter_bridge
        nvs_flash
        esp_wifi
        esp_event
        log
)
