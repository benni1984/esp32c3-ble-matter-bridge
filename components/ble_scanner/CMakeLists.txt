idf_component_register(
    SRCS "ble_scanner.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        bt
        nvs_flash
        esp_event
)
