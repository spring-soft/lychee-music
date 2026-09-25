idf_component_register(
    SRCS "src/store.c"
    INCLUDE_DIRS "include"
    REQUIRES
        fatfs
        wear_levelling
        vfs
        esp_partition
        esp_hw_support
        freertos
)
