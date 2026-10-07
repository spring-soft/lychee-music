idf_component_register(
    SRCS "src/encoder.c"
    INCLUDE_DIRS "include"
    REQUIRES
        board
        esp_driver_pcnt
        esp_driver_gpio
        esp_timer
        espressif__button
)
