idf_component_register(
    SRCS "src/settings.c"
    INCLUDE_DIRS "include"
    REQUIRES
        nvs_flash
        esp_timer
        freertos
)
