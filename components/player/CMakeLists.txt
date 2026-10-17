idf_component_register(
    SRCS "src/player.c" "src/player_remote.c"
    INCLUDE_DIRS "include"
    REQUIRES
        board
        app_core
        audio_engine
        subsonic
        qqmusic
        settings
        store
        json
        freertos
        esp_hw_support
        esp_http_client
        esp_timer
)
