idf_component_register(
    SRCS "ble_scanner.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        bt
        nvs_flash
        esp_event
        bthome
)

# Redirect Matter's NimBLE teardown calls to our no-op wrappers so NimBLE
# stays alive for passive BLE scanning after commissioning.
target_link_options(${COMPONENT_LIB} INTERFACE
    -Wl,--wrap=nimble_port_stop
    -Wl,--wrap=nimble_port_deinit
)
