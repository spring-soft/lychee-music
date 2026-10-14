/*
 * qrpng —— 把 QQ 契约层返回的登录二维码 PNG 解成【点阵】
 *
 * 为什么要有这个文件（而不是把 PNG 直接丢给 LVGL 显示）：
 *   · 契约层 `POST /v1/login/qr` 返回的是一张 **150×150 的 PNG**（实测：
 *     1bit 调色板、无隔行、2 色调色板、IDAT 346 字节），而屏幕只有 **160×128**。
 *   · 直接缩放着显示会把模块边界糊掉（150/29 不是整数），手机就扫不动了。
 *   · 而且这张图**没有静区**（四周留白）—— 原图直接给 OpenCV 是解不出来的，
 *     必须放大 + 补白边才行（2026-10-14 实测确认）。
 *   所以正确做法只有一个：**解出模块点阵，再按整数倍重画**。本文件负责前半段。
 *
 * 只认这一种最简形态，其它一律返回 ESP_ERR_NOT_SUPPORTED —— 宁可报"二维码格式
 * 不支持"，也不要画一张错的码出去：画错了用户只看到"手机扫不出来"。
 *   · 位深 1 / 8，颜色类型 0（灰度）或 3（调色板）
 *   · 非隔行
 *   · 8 位调色板按亮度判黑白（不假设 0 一定是白 —— 实测的这张正好 0=白 1=黑）
 *
 * 模块数怎么定：QR 的版本决定模块数（21/25/29/…），PNG 里没有这个信息，
 * 而 150/29 又不是整数 —— 所以用**定位图案打分**反推：对每个候选模块数，
 * 按它把图切成 n×n 采样，看三个 7×7 定位图案对不对得上。对的那个一定赢。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* QR 版本 10 = 57 模块。QQ 登录码实测是版本 3（29 模块），留足余量 */
#define QRPNG_MAX_MODULES 57
#define QRPNG_ROW_BYTES   ((QRPNG_MAX_MODULES + 7) / 8)

typedef struct {
    int     n;                                       /* 模块数（21/25/29/…） */
    uint8_t row[QRPNG_MAX_MODULES][QRPNG_ROW_BYTES]; /* 每模块 1 bit，1 = 黑 */
} qrpng_t;

/* 把 PNG 解成点阵。失败时 out 内容未定义。
 * 常见失败：ESP_ERR_NOT_SUPPORTED（形态不认识）/ ESP_ERR_INVALID_RESPONSE（数据坏了）
 *          ESP_ERR_NOT_FOUND（图里全是白的，或者找不到定位图案） */
esp_err_t qrpng_decode(const uint8_t *png, size_t len, qrpng_t *out);

/* 第 (x,y) 个模块是不是黑的；越界返回 false */
static inline bool qrpng_dark(const qrpng_t *q, int x, int y)
{
    if (q == NULL || x < 0 || y < 0 || x >= q->n || y >= q->n) return false;
    return (q->row[y][x >> 3] >> (7 - (x & 7))) & 1;
}

/* 把点阵按 ASCII 图打进日志（每行 n 个字符）。
 * ⚠️ 这不是"调试残留"：这个项目的屏幕经常不可信（黑屏/没插 FPC/校准跑偏），
 *    把二维码原样打进日志，才能让 PC 侧把它渲染出来、交给 OpenCV 复核
 *    "这张码到底扫不扫得动"。看不见屏幕时这是唯一的验证手段。 */
void qrpng_dump(const qrpng_t *q);

#ifdef __cplusplus
}
#endif
