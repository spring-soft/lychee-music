idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES
        board
        app_core
        audio_engine
        net_mgr
        subsonic
        qqmusic
        player
        settings
        store
        sd_card
        musiclib
        hid_remote
        web_console
        ui
        spi_flash
        esp_app_format
)
