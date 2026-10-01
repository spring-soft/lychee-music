# BLE 编掉时（见 sdkconfig.forced 的 CONFIG_BT_ENABLED）不能再去 REQUIRES
# esp_hid / bt —— 否则会去链接一个根本没参与编译的组件。
# 源码那边同一套开关把实现换成空函数，两边必须一致。
set(_hid_ble_deps "")
if(CONFIG_BT_ENABLED)
    set(_hid_ble_deps esp_hid bt)
endif()

idf_component_register(
    SRCS "src/hid_remote.c"
    INCLUDE_DIRS "include"
    REQUIRES
        settings
        esp_timer
        esp_hw_support
        freertos
        ${_hid_ble_deps}
)
