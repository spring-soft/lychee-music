/*
 * sd_card —— TF 卡（SPI 模式）挂载 / 卸载
 *
 * 硬件（V2 板，见 pcb/接线表与引脚定义.md）：
 *   TF 与屏幕【共用 SPI2】—— CLK=IO4、MOSI=IO5，靠 CS 区分（屏 IO15 / 卡 IO12）；
 *   MISO=IO13，卡检测 CD=IO14。
 *   DAT1/DAT2 悬空 → 只能 SPI 模式，不是 SD 1-bit 模式。
 *
 * 为什么是"懒挂载 + 用完卸载"，而不是开机就挂上：
 *   起播后内部 SRAM 只剩 10~15KB（实测记录在 sdkconfig.forced 顶部），而 SDSPI +
 *   SDMMC 协议层要在内部 RAM 上再吃一份（DMA 缓冲、协议状态）。
 *   在线模式根本用不到卡，所以：**进离线模式才挂，回在线模式就卸**。
 *   这样"在线播放"这条已经跑通的路径完全不受影响 —— 也正是用户要的
 *   "两条线不要互相干扰"。
 *
 * ⚠️⚠️ 绝不格式化：
 *   format_if_mount_failed = false。卡上是用户的音乐，挂不上只能报错。
 *   本组件只读用户文件（写索引由 musiclib 负责，且只写它自己那一个文件）。
 *   **任何人都不许在这里加 f_mkfs / 带 format 的挂载选项。**
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 挂载点。离线曲库的路径都以它开头 */
#define SD_ROOT "/sdcard"

/* 只配置卡检测引脚，不挂载。失败不致命（离线模式会退化成"曲库为空"）。 */
esp_err_t sd_card_init(void);

/* 卡检测（CD 引脚，低电平 = 有卡）。
 * 没卡时不必去挂载 —— 挂载要等 SD 协议的超时，白卡几百毫秒。 */
bool sd_card_present(void);

/* 挂载（幂等：已挂上直接 ESP_OK）。
 * ⚠️ 前提：SPI2 总线已经初始化过 —— 屏幕在开机第 3 步 ui_start() 里建的，
 *    而挂卡是用户触发、远在其后，所以天然满足。 */
esp_err_t sd_card_mount(void);

/* 卸载（幂等）。⚠️ 调用前必须确保没有文件正在读写 —— player 会先停播再调。 */
esp_err_t sd_card_unmount(void);

bool sd_card_mounted(void);

/* ---------------- 诊断 ---------------- */
int       sd_card_mount_count(void);   /* 累计挂载成功次数（能看出有没有反复挂/卸） */
esp_err_t sd_card_last_err(void);      /* 上次挂载的错误码（ESP_OK = 没出过错） */

#ifdef __cplusplus
}
#endif
