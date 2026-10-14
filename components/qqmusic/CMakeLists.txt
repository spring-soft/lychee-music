idf_component_register(
    SRCS
        "src/qqmusic.c"
        "src/qqlogin.c"
        "src/qrpng.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_http_client
        json
        nvs_flash
        esp_hw_support
        # sub_song_t 是两种后端共用的歌曲结构，定义在 subsonic.h 里
        subsonic
)
