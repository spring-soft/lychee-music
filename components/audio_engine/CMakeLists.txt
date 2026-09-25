idf_component_register(
    SRCS
        "src/audio_engine.c"
        "src/audio_ring.c"
        "src/audio_source_http.c"
    INCLUDE_DIRS "include"
    REQUIRES
        board
        esp_audio_codec
        esp_http_client
    # ⚠️ M1 专用：把测试音频嵌进固件（M2 起改成网络流，M3 后删掉这一段和 audio/ 目录）
    EMBED_FILES
        "audio/test_melody.mp3"
        "audio/test_melody.flac"
)
