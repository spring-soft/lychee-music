idf_component_register(
    SRCS "src/assets.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_partition
        esp_hw_support
)

# 把打包好的素材注册到 "assets" 分区：idf.py flash 会自动一起烧。
# 平时只改素材（图片/字体/网页）也可以单独更新，不用重烧固件：
#   esptool.py -p /dev/ttyACM0 write_flash 0x620000 components/assets/assets.bin
set(ASSETS_BIN "${CMAKE_CURRENT_SOURCE_DIR}/assets.bin")
if(EXISTS "${ASSETS_BIN}")
    esptool_py_flash_to_partition(flash "assets" "${ASSETS_BIN}")
else()
    message(WARNING "esp-music: 还没有 ${ASSETS_BIN}，先跑 tools/gen_assets.py")
endif()
