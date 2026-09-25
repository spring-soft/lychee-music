/*
 * UI：显示初始化 + LVGL 页面
 *
 * 线程模型（对应方案的任务表）：
 *   - 只有 ui 任务碰 LVGL（core 1, prio 5），别的模块通过 app_core 的命令/状态交互
 *   - lv_timer_handler() 的最坏耗时会被统计并周期打印 —— "先量化再优化"
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ui_display_init(void);

/* 建页面 + 起 ui 任务 */
esp_err_t ui_start(void);

/* 启动页（开机连网/拉歌单要 10~19 秒，给用户看得见的进度，别让人以为卡死）。
 * 任意任务可调；实际写入 LVGL 由 ui 任务在 tick 里完成。 */
void ui_splash_set(const char *text, int step, int total);
void ui_splash_finish(void);

lv_display_t *ui_display_get(void);

/* 屏幕方向/偏移运行时可变（ST7735 面板批次差异），M5 的 /api/display 也用它 */
void ui_display_set_orientation(bool mirror_x, bool mirror_y, bool swap_xy, bool invert,
                                int gap_x, int gap_y);

/* 反相（负片）与 RGB/BGR 通道顺序运行时可变 —— 校准用。
 * esp_lcd 没有运行时改 element order 的接口，所以重建 panel（公开 API）。 */
void ui_display_set_invert(bool invert);
void ui_display_set_bgr(bool bgr);

/* 载入中文字体（assets 分区里的 LVGL bin 字体，零 RAM 占用） */
const lv_font_t *ui_font_cjk(void);

/* 累计刷新像素数 / 刷新次数（诊断用：区分"重绘太多"与"等 DMA"） */
void ui_display_flush_stats(uint32_t *px, uint32_t *cnt);

/* 屏幕回读（排障用）：把接下来 chunks 块 flush 的像素打成字符画输出到串口。
 * 用途：没法看屏幕时确认"屏上到底画出了什么"。正常路径零开销。 */
void ui_display_dump_arm(uint32_t chunks);
/* 只打这个矩形内的像素，且 1 像素 = 1 字符（查单个字形用，见 ui_display.c 的说明） */
void ui_display_dump_crop(int x1, int y1, int x2, int y2);

#ifdef __cplusplus
}
#endif
