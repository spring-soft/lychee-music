idf_component_register(
    SRCS "src/web_console.c"
    INCLUDE_DIRS "include"
    REQUIRES
        assets
        app_core
        audio_engine
        player
        subsonic
        qqmusic
        net_mgr
        settings
        store
        hid_remote
        board
        esp_http_server
        app_update
        esp_partition
        json
        esp_timer
        esp_hw_support
)
