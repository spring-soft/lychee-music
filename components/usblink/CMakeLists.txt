idf_component_register(
    SRCS "src/usblink.c"
    INCLUDE_DIRS "include"
    REQUIRES
        driver
        esp_timer
        freertos
)
