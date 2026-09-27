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
# ⚠️ 必须明确指定用哪个 Python，不能裸写 python3：
#    有的机器上 PATH 里的 python3/python 是 Microsoft Store 的占位程序，执行它
#    既不产出也不报错（rc=0、无输出），下面这段就【静默跳过】。实测踩到：
#    sdkconfig.forced 里的 4 项（NimBLE 角色裁剪、WiFi IRAM 优化）从来没生效过，
#    而它们是 M7 蓝牙能不能跑起来的关键，不是调优项。
#    ESP-IDF 自己的虚拟环境最可靠：export.bat / export.sh 之后 IDF_PYTHON_ENV_PATH 必然存在。
if(DEFINED ENV{IDF_PYTHON_ENV_PATH})
    file(TO_CMAKE_PATH "$ENV{IDF_PYTHON_ENV_PATH}" _idf_pyenv)
    if(WIN32)
        set(_sync_py "${_idf_pyenv}/Scripts/python.exe")
    else()
        set(_sync_py "${_idf_pyenv}/bin/python")
    endif()
endif()
if(NOT _sync_py OR NOT EXISTS "${_sync_py}")
    find_program(_sync_py NAMES python3 python)
endif()

if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.local" OR EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.forced")
    execute_process(
        COMMAND "${_sync_py}" "${CMAKE_CURRENT_LIST_DIR}/tools/sync_sdkconfig.py"
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
