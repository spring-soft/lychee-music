/*
 * encoder —— 旋转编码器（HYCW03-2.8D-5P-S：增量式 12 节点/12 脉冲 + 按压开关）
 *
 * 硬件（V2 板；引脚与上拉见 pcb/接线表与引脚定义.md:134-144）：
 *   ENC_A = IO9    ENC_B = IO10    ENC_SW = IO11
 *   A / B / SW 各有 10k 外部上拉（R18/R19/R20），A / B 还各挂 100nF 到地
 *   （C26/C27，下地见接线表 GND 网络），编码器公共端与开关另一端接地。
 *   → 与 K1-K4 一样是【低电平有效】。
 *
 * 职责边界：本组件只做两件事 —— "正交脉冲 → 逻辑步数" 和 "按下开关 → 短/长/保持事件"。
 *   **不碰 UI、不碰 LVGL、不碰 player**。事件用回调交出去，由 ui_keys.c 注册，
 *   于是依赖方向是 ui → encoder，而不是反过来（encoder 不需要知道任何页面）。
 *
 * 板型：只有 BOARD_HAS_ENCODER=1 时才真的编进来（派生规则见 board.h）。
 *   面包板变体下 encoder_init() 是空实现、回调永不触发，
 *   调用方不需要写任何 #if —— 这一点是刻意的，免得"I 页面逻辑"里到处漏条件编译。
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 旋转方向。语义由调用方解释，约定：
 *   CW  = 顺时针 = 界面里的"向下 / 向右"
 *   CCW = 逆时针 = 界面里的"向上 / 向左" */
typedef enum {
    ENCODER_DIR_CCW = -1,
    ENCODER_DIR_CW  = +1,
} encoder_dir_t;

/* 按下事件类型。取值刻意与 ui_keys_inject() 的 kind 对齐，
 * 这样 ui_keys.c 能把编码器的按下直接喂进与物理按键完全相同的分发路径。 */
#define ENCODER_PRESS_SHORT  0
#define ENCODER_PRESS_LONG   1
#define ENCODER_PRESS_HOLD   2

/* on_rotate(dir, steps)：dir 见 encoder_dir_t，steps 恒为正数（连续快转时 >1）。
 * on_press(kind)     ：kind 见上面三个宏。
 *
 * ⚠️ 两个回调都在 **esp_timer 任务**上下文里被调用 —— 和 espressif/button 的按键
 *    回调是同一个任务（iot_button 也是 ESP_TIMER_TASK 调度）。ui_keys.c 现有代码
 *    就是这么被调用的，编码器沿用同一约定，不引入新的线程假设。 */
typedef void (*encoder_rotate_cb_t)(int dir, int steps);
typedef void (*encoder_press_cb_t)(int kind);

/* 初始化 PCNT 正交解码 + 按下开关 + 20ms 轮询定时器。
 * 失败不致命：上层记一条日志继续跑（没编码器的板子照样能用按键）。 */
esp_err_t encoder_init(encoder_rotate_cb_t on_rotate, encoder_press_cb_t on_press);

/* ---------------- 诊断 / 真机校准 ----------------
 * 板子第一次上手时对着这三行调常数，不用改任何别的代码。 */
/* 当前"每格脉冲数"（ENCODER_PULSES_PER_DETENT）。拧一格看 encoder_raw_counts()
 * 涨了几个，对不上就改 encoder.c 里那个宏。 */
int encoder_pulses_per_detent(void);
/* 累计原始 PCNT 计数（诊断：拧一格涨多少、方向对不对） */
int encoder_raw_counts(void);
/* 因单次步数过大被夹掉的计数（异常尖峰/手抖，正常应长期为 0） */
int encoder_clamped_counts(void);

#ifdef __cplusplus
}
#endif
