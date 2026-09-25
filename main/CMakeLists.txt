idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES
        board
        app_core
        audio_engine
        net_mgr
        subsonic
        player
        settings
        store
        hid_remote
        web_console
        ui
        spi_flash
        esp_app_format
)
