idf_component_register(
    SRCS "src/subsonic.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_http_client
        json
        mbedtls
        nvs_flash
        esp_hw_support
        esp_new_jpeg
)
