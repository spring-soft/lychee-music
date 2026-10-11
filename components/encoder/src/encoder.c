/*
 * encoder —— 旋转编码器实现（见 encoder.h 的硬件说明与职责边界）
 *
 * 实现要点：
 *   1) 正交解码用 **PCNT 硬件**，不用 GPIO 中断：机械编码器抖动厉害，
 *      软件消抖要额外定时器+状态机，PCNT 有毛刺滤波，且不占 CPU。
 *   2) 用 IDF 标准的两通道 x4 接法（A 相边沿 + B 相电平，另一通道反过来），
 *      每个边沿都计数 → 一格 4 个计数（HYCW03 是 12 节点/12 脉冲）。
 *   3) 20ms 轮询一次读数并清零：不读会绕回（PCNT 是 16 位有符号），
 *      20ms 对手感足够且几乎不占 CPU。
 *   4) 按下开关复用 espressif/button，参数与 K1-K4 完全一致（700ms 长按），
 *      这样"短按/长按/保持"三种事件的语义和按键是同一套。
 */
#include "encoder.h"
#include "board.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include <stdint.h>

static const char *TAG = "encoder";

/* ------------------------------------------------------------------
 * 每"格"（detent）产生多少个 PCNT 计数。
 *
 * HYCW03-2.8D-5P-S 规格：增量式，节点数/脉冲数 = 12/12
 *   → 一格 = 一个完整的正交周期 = A/B 各一个整周期 = 4 个边沿。
 * 下面用的是两通道 x4 接法，所以一格 = 4 个计数。
 *
 * ⚠️ 这是【真机唯一需要校准的常数】：板子到手后拧一格，看串口日志里
 *    encoder_raw_counts() 涨了几个 ——
 *      涨 4  → 正确
 *      涨 2  → 改成 2
 *      涨 8  → 改成 8（说明编码器是 24 脉冲/12 节点那类）
 *    改大了转一格走不动，改小了转一格跳两行。
 * ------------------------------------------------------------------ */
#define ENCODER_PULSES_PER_DETENT 4

/* 轮询周期。人手最快的连续旋转大约 10~20 格/秒，20ms 一次足够跟上，
 * 而且每次只读一个寄存器。 */
#define ENCODER_POLL_US (20 * 1000)

/* 加速档：一次轮询里攒到这么多计数就按倍率走（快速旋转时不必拧十圈）。
 * 阈值按"格"算：攒到 3 格（≈150 格/秒）开始加速。 */
#define ENCODER_ACCEL1_COUNTS (3 * ENCODER_PULSES_PER_DETENT)
#define ENCODER_ACCEL2_COUNTS (6 * ENCODER_PULSES_PER_DETENT)

/* 单次最多报这么多步：兜底防异常尖峰（正常旋转永远到不了 20 格/20ms）。 */
#define ENCODER_MAX_STEPS 20

#if BOARD_HAS_ENCODER

#include "driver/pulse_cnt.h"
#include "driver/gpio.h"
#include "iot_button.h"
#include "button_gpio.h"

/* 长按阈值与 ui_keys.c 保持一致：太短会在"想连转两下"时误触发别的动作。 */
#define ENC_SW_LONG_MS   700
#define ENC_SW_DEBOUNCE  60

static pcnt_unit_handle_t    s_unit;
static pcnt_channel_handle_t s_chA;
static pcnt_channel_handle_t s_chB;
static button_handle_t       s_sw;
static esp_timer_handle_t    s_timer;

static encoder_rotate_cb_t s_on_rotate;
static encoder_press_cb_t  s_on_press;

static int               s_remainder;    /* 不足一格的零头，留到下次 */
static volatile int32_t  s_raw_total;    /* 诊断：累计原始计数 */
static volatile int32_t  s_clamped;      /* 诊断：被 ENCODER_MAX_STEPS 夹掉的步数 */

/* ------------------------------------------------------------------ 轮询
 *
 * 在 esp_timer 任务里跑 —— 与 espressif/button 的按键回调同一个任务，
 * 所以 ui_keys.c 那边的线程假设（一如既往地直接调 ui_xxx）完全不变。
 */
static void poll_cb(void *arg)
{
    (void)arg;
    if (s_unit == NULL) return;

    int c = 0;
    if (pcnt_unit_get_count(s_unit, &c) != ESP_OK) return;
    /* 先读后清。清了才不会绕回（16 位有符号）；
     * "读"和"清"之间进来的边沿最多丢 1 个，对旋钮完全无所谓。 */
    pcnt_unit_clear_count(s_unit);

    s_raw_total += c;
    s_remainder += c;

    int steps = s_remainder / ENCODER_PULSES_PER_DETENT;
    s_remainder -= steps * ENCODER_PULSES_PER_DETENT;
    if (steps == 0) return;

    /* 加速：按"这一轮攒了多少"决定倍率 */
    int abs_c = c < 0 ? -c : c;
    int mult  = 1;
    if (abs_c >= ENCODER_ACCEL2_COUNTS)      mult = 4;
    else if (abs_c >= ENCODER_ACCEL1_COUNTS) mult = 2;
    steps *= mult;

    if (steps > ENCODER_MAX_STEPS) {
        s_clamped += steps - ENCODER_MAX_STEPS;
        steps = ENCODER_MAX_STEPS;
    } else if (steps < -ENCODER_MAX_STEPS) {
        s_clamped += -ENCODER_MAX_STEPS - steps;
        steps = -ENCODER_MAX_STEPS;
    }

    if (s_on_rotate) {
        int dir = (steps > 0) ? ENCODER_DIR_CW : ENCODER_DIR_CCW;
        int n   = (steps > 0) ? steps : -steps;
        s_on_rotate(dir, n);
    }
}

/* ------------------------------------------------------------------ 按下开关
 * 回调里只做"转发成 kind 再交出去"，不碰 UI。
 * ⚠️ kind 用 (void*)(intptr_t) 传：与 ui_keys.c 的写法一致。 */
static void sw_short(void *handle, void *usr)
{
    (void)handle;
    if (s_on_press) s_on_press((int)(intptr_t)usr);
}

esp_err_t encoder_init(encoder_rotate_cb_t on_rotate, encoder_press_cb_t on_press)
{
    s_on_rotate = on_rotate;
    s_on_press  = on_press;

    /* ---- 1) PCNT 正交解码 ---- */
    pcnt_unit_config_t ucfg = {
        /* 上下限只用来给硬件一个边界，正常靠轮询清零，不会真的触到。
         * ⚠️ 必须显式给 —— 留 0 的话上下限都是 0，计数根本走不动。 */
        .high_limit = 30000,
        .low_limit  = -30000,
    };
    esp_err_t err = pcnt_new_unit(&ucfg, &s_unit);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PCNT unit 创建失败: %s", esp_err_to_name(err));
        s_unit = NULL;      /* 后续轮询直接返回，不影响按键可用 */
        return err;
    }

    /* 毛刺滤波：1us 以内的一律不算。
     * 板子上 A/B 另有 10k+100nF 的 RC（τ≈1ms），这里是最外层保险。 */
    pcnt_glitch_filter_config_t filt = { .max_glitch_ns = 1000 };
    err = pcnt_unit_set_glitch_filter(s_unit, &filt);
    if (err != ESP_OK) {
        /* 不致命：只是少了软件侧滤波，RC 还在。 */
        ESP_LOGW(TAG, "毛刺滤波设置失败: %s（不致命，继续）", esp_err_to_name(err));
    }

    /* 通道 A：A 相做边沿、B 相做电平（电平决定加还是减） */
    pcnt_chan_config_t ccfg = {
        .edge_gpio_num  = BOARD_PIN_ENC_A,
        .level_gpio_num = BOARD_PIN_ENC_B,
    };
    err = pcnt_new_channel(s_unit, &ccfg, &s_chA);
    if (err != ESP_OK) goto fail;
    pcnt_channel_set_edge_action(s_chA, PCNT_CHANNEL_EDGE_ACTION_DECREASE,
                                       PCNT_CHANNEL_EDGE_ACTION_INCREASE);
    pcnt_channel_set_level_action(s_chA, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                        PCNT_CHANNEL_LEVEL_ACTION_INVERSE);

    /* 通道 B：把 A/B 对调，凑成 x4 */
    ccfg.edge_gpio_num  = BOARD_PIN_ENC_B;
    ccfg.level_gpio_num = BOARD_PIN_ENC_A;
    err = pcnt_new_channel(s_unit, &ccfg, &s_chB);
    if (err != ESP_OK) goto fail;
    pcnt_channel_set_edge_action(s_chB, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                       PCNT_CHANNEL_EDGE_ACTION_DECREASE);
    pcnt_channel_set_level_action(s_chB, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                        PCNT_CHANNEL_LEVEL_ACTION_INVERSE);

    err = pcnt_unit_enable(s_unit);
    if (err != ESP_OK) goto fail;
    pcnt_unit_clear_count(s_unit);
    err = pcnt_unit_start(s_unit);
    if (err != ESP_OK) goto fail;

    /* ---- 2) 按下开关（参数与 K1-K4 完全一致）---- */
    button_config_t bcfg = {
        .long_press_time  = ENC_SW_LONG_MS,
        .short_press_time = ENC_SW_DEBOUNCE,
    };
    button_gpio_config_t bgpio = {
        .gpio_num          = BOARD_PIN_ENC_SW,
        .active_level      = 0,      /* 外部 10k 上拉 + 对地开关 → 低电平按下 */
        .enable_power_save = false,
        .disable_pull      = false,  /* 内部上拉一起用，外部 10k 为准 */
    };
    err = iot_button_new_gpio_device(&bcfg, &bgpio, &s_sw);
    if (err != ESP_OK) {
        /* 开关坏了不影响旋转，降级继续 */
        ESP_LOGW(TAG, "编码器按下开关创建失败: %s（旋转仍可用）", esp_err_to_name(err));
        s_sw = NULL;
    } else {
        iot_button_register_cb(s_sw, BUTTON_SINGLE_CLICK, NULL, sw_short,
                               (void *)(intptr_t)ENCODER_PRESS_SHORT);
        iot_button_register_cb(s_sw, BUTTON_LONG_PRESS_START, NULL, sw_short,
                               (void *)(intptr_t)ENCODER_PRESS_LONG);
        iot_button_register_cb(s_sw, BUTTON_LONG_PRESS_HOLD, NULL, sw_short,
                               (void *)(intptr_t)ENCODER_PRESS_HOLD);
    }

    /* ---- 3) 轮询定时器 ---- */
    esp_timer_create_args_t tcfg = {
        .callback        = poll_cb,
        .name            = "enc_poll",
        .dispatch_method = ESP_TIMER_TASK,   /* 与 iot_button 同一上下文 */
    };
    err = esp_timer_create(&tcfg, &s_timer);
    if (err != ESP_OK) goto fail;
    err = esp_timer_start_periodic(s_timer, ENCODER_POLL_US);
    if (err != ESP_OK) goto fail;

    ESP_LOGI(TAG, "编码器就绪：A=%d B=%d SW=%d（低电平按下）｜每格 %d 计数｜轮询 %dms",
             BOARD_PIN_ENC_A, BOARD_PIN_ENC_B, BOARD_PIN_ENC_SW,
             ENCODER_PULSES_PER_DETENT, ENCODER_POLL_US / 1000);
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "编码器初始化失败: %s", esp_err_to_name(err));
    if (s_chA) { pcnt_del_channel(s_chA); s_chA = NULL; }
    if (s_chB) { pcnt_del_channel(s_chB); s_chB = NULL; }
    if (s_unit) { pcnt_unit_disable(s_unit); pcnt_del_unit(s_unit); s_unit = NULL; }
    return err;
}

int encoder_pulses_per_detent(void) { return ENCODER_PULSES_PER_DETENT; }
int encoder_raw_counts(void)        { return (int)s_raw_total; }
int encoder_clamped_counts(void)    { return (int)s_clamped; }

#else   /* !BOARD_HAS_ENCODER —— 面包板变体：全部编成空实现 */

esp_err_t encoder_init(encoder_rotate_cb_t on_rotate, encoder_press_cb_t on_press)
{
    (void)on_rotate;
    (void)on_press;
    /* 刻意打一条日志：免得"板子没反应"时怀疑是编码器代码没跑，
     * 实际是板型开关没开（面包板天然没有编码器）。 */
    ESP_LOGI(TAG, "本板型无编码器（BOARD_HAS_ENCODER=0），跳过初始化");
    return ESP_OK;
}

int encoder_pulses_per_detent(void) { return 0; }
int encoder_raw_counts(void)        { return 0; }
int encoder_clamped_counts(void)    { return 0; }

#endif  /* BOARD_HAS_ENCODER */
