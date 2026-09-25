# ESP-Music — 纯原生音乐终端（ESP32-S3 / ES8388 / ST7735 / K1-K4）
# 不依赖 esp-claw / Lua / GMF
cmake_minimum_required(VERSION 3.16)

# 敏感配置（WiFi 账号密码等）放 sdkconfig.local，不进源码树、不进版本库。
# 存在就自动叠加为 sdkconfig 默认值，无需任何环境变量。
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.local")
    list(APPEND SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.local")
endif()
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.forced")
    list(APPEND SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.forced")
endif()

# ⚠️ 光靠 SDKCONFIG_DEFAULTS 不够：它只在 sdkconfig 不存在（或某符号还没赋值）
#    时生效。sdkconfig 一旦生成过，改 sdkconfig.local / sdkconfig.forced 再
#    build+flash 都是【无效】的 —— 实测换地方改了 SSID，设备还是去连旧网络。
#    所以 configure 前强制覆盖一次（两个文件一起）。
#
# ⚠️ 还要把这两个文件登记成 configure 的依赖：否则【只改 sdkconfig.forced】
#    不会触发 cmake reconfigure，下面这段 execute_process 根本不会跑，
#    改的配置就静默不生效（实测踩到：加了 SPIRAM_TRY_ALLOCATE_WIFI_LWIP
#    重新 build，sdkconfig 里查还是 not set）。
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.local"
    "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.forced"
    "${CMAKE_CURRENT_LIST_DIR}/tools/sync_sdkconfig.py"
)
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.local" OR EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.forced")
    execute_process(
        COMMAND python3 "${CMAKE_CURRENT_LIST_DIR}/tools/sync_sdkconfig.py"
        RESULT_VARIABLE _sync_rc
        OUTPUT_VARIABLE _sync_out
        ERROR_VARIABLE _sync_err
    )
    if(_sync_rc EQUAL 0)
        string(STRIP "${_sync_out}" _sync_out)
        if(_sync_out)
            message(STATUS "${_sync_out}")
        endif()
    else()
        message(WARNING "esp-music: sync_sdkconfig.py 失败（rc=${_sync_rc}）：${_sync_err}")
    endif()
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp-music)
