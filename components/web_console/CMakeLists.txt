idf_component_register(
    SRCS
        "src/web_console.c"
        # ★ 网页（gzip）的字节数组，由 tools/gen_assets.py 生成。
        # 编进 app 而不是从 assets 分区读 —— 因为 **OTA 只更新 app 分区**，
        # 网页放 assets 里的话"改网页"就永远得插 USB（实测踩到过）。
        # ⚠️ 这个文件是生成物：全新克隆后要先跑 tools/gen_assets.py 再 build，
        #    和 assets.bin 是同一个前提（项目本来就有这个约定）。
        "src/web_index_gz.c"
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
        esp_http_client
        app_update
        esp_partition
        json
        esp_timer
        esp_hw_support
)
