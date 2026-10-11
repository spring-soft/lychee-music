idf_component_register(
    SRCS "src/sd_card.c"
    INCLUDE_DIRS "include"
    REQUIRES
        board
        # fatfs 提供 vfs/esp_vfs_fat.h（esp_vfs_fat_sdspi_mount / info / sdcard_unmount）；
        # 它自己会传递引入 sdmmc 与 esp_driver_sdspi，这里仍显式列出，
        # 免得将来 IDF 调整传递依赖时突然编不过。
        fatfs
        sdmmc
        esp_driver_sdspi
        # sd_card.c 里用 SPI2_HOST 表示"和屏幕同一条总线"
        esp_driver_spi
        esp_driver_gpio
)
