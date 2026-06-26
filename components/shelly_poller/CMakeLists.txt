idf_component_register(
    SRCS "shelly_poller.cpp"
    INCLUDE_DIRS "include"
    REQUIRES bthome esp_http_client cjson mbedtls
)
