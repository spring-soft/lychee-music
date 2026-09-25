/*
 * 设置持久化（NVS）—— 全工程唯一的设置落盘入口
 *
 * 为什么要单独一个组件：
 *   1. 现在音量在 player 里存、亮度在 UI 里存，各写一份"2 秒去抖"逻辑；
 *      M5 的网页控制台还要存 WiFi 凭据和 Navidrome 地址账号 —— 会变成四处重复。
 *   2. 写 NVS 有代价（擦写 + 磨损），按键长按连调时**必须去抖**，
 *      这个约束应该在一处实现而不是靠每个调用点自觉。
 *
 * 两种写法，按场景选：
 *   settings_set_int_lazy() —— 高频调整（音量/亮度）用，2 秒内不再变才落盘；
 *                              需要有人周期调 settings_tick()。
 *   settings_set_int()      —— 一次性动作（网页点"保存"）用，立刻落盘。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ⚠️ NVS key 最长 15 字符 */
#define SETTINGS_MAX_KEY 16
#define SETTINGS_MAX_STR 128

esp_err_t settings_init(void);

/* ---- 整数（音量、亮度、来源下标这种） ---- */
int  settings_get_int(const char *key, int def);
esp_err_t settings_set_int(const char *key, int val);          /* 立即落盘 */
void settings_set_int_lazy(const char *key, int val);          /* 去抖落盘 */

/* ---- 字符串（WiFi SSID/密码、Navidrome 地址/账号） ---- */
/* 返回 false = 键不存在（out 里是 def） */
bool settings_get_str(const char *key, char *out, size_t len, const char *def);
esp_err_t settings_set_str(const char *key, const char *val);  /* 立即落盘 */

/* 周期调用（AI 建议放 player 任务的循环里，200ms 一次足够）：
 * 把静置够 2 秒的 lazy 项落盘 */
void settings_tick(void);

/* 供诊断显示：累计落盘次数（能看到去抖有没有生效） */
int settings_commit_count(void);
/* 待落盘的项数（0 = 都存好了）。菜单里的"系统信息"用它提示"保存中" */
int settings_pending_count(void);

#ifdef __cplusplus
}
#endif
