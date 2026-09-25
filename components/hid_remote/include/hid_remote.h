/*
 * hid_remote —— 把 K1-K4 变成蓝牙 HID 遥控器（M7）
 *
 * 用途：手机/电脑配对之后，按 K1-K4 控制【对方】的播放（下一首/暂停/音量），
 * 而不是控制本机。设备本身照常当音乐终端用，两件事互不干扰：
 *   - 只在 `hid_remote_active()` 为真（栈起来了 + 已连上 + 用户开了开关）时
 *     按键才发给对端；否则按键完全走本机（ui_keys.c 的原有逻辑）。
 *
 * ⚠️ 内部 SRAM 是这块板唯一紧的资源（起播后只剩 10~15KB），而 BLE 要 40KB 量级。
 *    能不能跑起来取决于 sdkconfig.forced 里那几条（NimBLE 内存走 PSRAM +
 *    控制器活动数降到 2），**不是靠这里省出来的**。所以：
 *    - 初始化失败必须【非致命】：只让 available=false，播放/UI/网页完全不受影响；
 *    - 初始化尽量早（播放之前），那时内部 SRAM 还有 50KB 左右。
 *
 * 协议：标准 HID Consumer Control（Usage Page 0x0C），Report ID = 1，2 字节位图。
 *      用 Consumer 而不是键盘，是因为手机/电脑的播放器都认这几个 usage，
 *      不需要对方装驱动、也不需要模拟按键组合。
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 遥控能发的键（对应 HID Consumer usage） */
typedef enum {
    HID_KEY_NONE = 0,
    HID_KEY_PLAY_PAUSE,     /* 0xCD */
    HID_KEY_NEXT,           /* 0xB5 */
    HID_KEY_PREV,           /* 0xB6 */
    HID_KEY_VOL_UP,         /* 0xE9 */
    HID_KEY_VOL_DOWN,       /* 0xEA */
    HID_KEY_MUTE,           /* 0xE2 */
    HID_KEY_STOP,           /* 0xB7 */
    HID_KEY_FAST_FWD,       /* 0xB3 */
    HID_KEY_REWIND,         /* 0xB4 */
} hid_key_t;

/* 起 BLE 栈 + 注册 HID 设备 + 开始广播。**失败非致命**，返回错误码即可。 */
esp_err_t hid_remote_init(void);

/* 栈起来了（不一定连上） */
bool hid_remote_available(void);
/* 有对端连着 */
bool hid_remote_connected(void);
/* 【蓝牙控制模式】—— 用户明确"进到遥控界面"的那个状态。
 *
 * ⚠️ 刻意【不存 NVS】：这是个"当前在干什么"的状态，不是设置。
 *    存了的话重启后会落在遥控界面、按键全发给对方，用户会以为是设备坏了。
 *
 * 单一真相源：菜单第一项 / 网页 / 遥控页的 K4 都改这一个标志，
 * 界面（ui_pages_tick）跟着它切页 —— 这样网页也能远程切模式，
 * 而不需要 web_console 去碰 LVGL。 */
bool hid_remote_mode(void);
void hid_remote_set_mode(bool on);
/* 三个条件都满足（模式开 + 栈可用 + 已连上）—— 按键层只判断这一个 */
bool hid_remote_active(void);

/* 发一个按键（按下 + 20ms 后自动释放；不要在中断/定时器回调里阻塞等） */
void hid_remote_send(hid_key_t k);

/* 诊断 */
const char *hid_remote_state_str(void);   /* "未连接" / "已连接" / "不可用" */
const char *hid_remote_peer_name(void);   /* 已配对对端名字，没有则空串 */
int         hid_remote_sent_count(void);  /* 累计发出去的按键数 */

#ifdef __cplusplus
}
#endif
