idf_component_register(
    SRCS "src/hid_remote.c"
    INCLUDE_DIRS "include"
    REQUIRES
        settings
        esp_hid
        bt
        esp_timer
        esp_hw_support
        freertos
)
