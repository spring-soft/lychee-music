/*
 * player_bt —— 蓝牙音箱的"服务器侧"连接管理（内部接口）
 *
 * 对外给 UI 的入口在 player.h（player_bt_scan / player_bt_get / ...）——
 * 那些入口只投命令，真正的 HTTP 在这里由 player 任务同步调用。
 *
 * 为什么单独一个文件：和 player_remote.c 同构。
 *   · 服务器契约层（qqmusic_host()）同时管着 /v1/bt/ 和 /v1/player/audio 两组接口；
 *   · 扫描/连接是阻塞的（1~8 秒），只能放 player 任务；
 *   · 列表是 PSRAM 里的一份快照，UI 只读。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PLAYER_BT_LIST_MAX 12   /* 屏上最多显示 5 行，留一倍多；多出来的扫描结果没意义 */

esp_err_t player_bt_init(void);   /* 只建锁 + 分配 PSRAM 列表，不联网 */

/* ---- 下面这些会发 HTTP（阻塞），只能从 player 任务调 ---- */
void player_bt_task_scan(void);
void player_bt_task_refresh(void);
void player_bt_task_connect(const char *addr);
/* 让服务器的音频输出切到蓝牙（bt=true）或切回 USB DAC（bt=false）。
 * 这是 POST /v1/player/audio?target=bt|dac —— 给 handle_out_switch 用。 */
esp_err_t player_bt_audio_target(bool bt);

#ifdef __cplusplus
}
#endif
