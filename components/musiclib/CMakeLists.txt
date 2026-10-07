idf_component_register(
    SRCS "src/musiclib.c"
    INCLUDE_DIRS "include"
    REQUIRES
        # 曲库是"卡上的东西"，路径常量 SD_ROOT 与挂载状态都来自 sd_card
        sd_card
        # sub_song_t（曲目结构）定义在 subsonic.h —— 全工程共用这一个结构
        subsonic
        # opendir/readdir/stat（VFS + FATFS）
        vfs
        # esp_rom_crc32_le：索引文件的完整性校验
        esp_rom
        # esp_random：离线模式的"随机 30 首"
        esp_hw_support
        freertos
)
