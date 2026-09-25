idf_component_register(
    SRCS "src/player.c"
    INCLUDE_DIRS "include"
    REQUIRES
        board
        app_core
        audio_engine
        subsonic
        settings
        store
        json
        freertos
        esp_hw_support
)
