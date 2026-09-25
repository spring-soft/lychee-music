idf_component_register(
    SRCS "src/board.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_driver_i2c
        esp_driver_i2s
        esp_driver_gpio
        esp_driver_ledc
        esp_driver_pcnt
)
