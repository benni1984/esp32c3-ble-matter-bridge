idf_component_register(
    SRCS "sensor_registry.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        bthome
        nvs_flash
        log
        console
)
