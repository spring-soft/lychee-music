/*
 * ESP-Music — 板级实现
 *
 * 移植自真机验证过的 ~/esp32s3-backup/native_es8388_test/main/main.c：
 *   - k_es8388_init[] 寄存器序列（含 I2C 100kHz、写后读回校验+重试）
 *   - PCNT 测 WS 频率、I2C 空闲电平诊断、寄存器 dump
 *
 * 引脚以【已确认的厂商源码】为准：~/esp32s3-backup/vendor_es8388/main/config.h
 *   AUDIO_I2S_GPIO_DOUT = GPIO_NUM_40 / AUDIO_I2S_GPIO_DIN = GPIO_NUM_38
 * ⚠️ native_es8388_test 里的 `#define PIN_DOUT 38 / PIN_DIN 40` 是**错的**
 *    （与它自己的文件头注释、厂商源码、以及放过音乐的 esp-claw yaml 全都矛盾）。
 */
#include <string.h>
#include <math.h>
#include <inttypes.h>
#include "board.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/pulse_cnt.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "soc/soc_caps.h"       /* SOC_I2S_SUPPORTS_APLL */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";

/* ---------------- ES8388 寄存器 ---------------- */
#define R_CONTROL1      0x00
#define R_CONTROL2      0x01
#define R_CHIPPOWER     0x02
#define R_ADCPOWER      0x03  /* ⚠️ 反逻辑：位=1 表示【断电】 */
#define R_DACPOWER      0x04
#define R_MASTERMODE    0x08
#define R_DACCONTROL1   0x17  /* 数据格式 + 字长 */
#define R_DACCONTROL2   0x18  /* MCLK/fs 比值(bit5) */
#define R_DACCONTROL3   0x19  /* bit2 = 静音 */
#define R_DACCONTROL4   0x1A  /* 左 DAC 数字音量 */
#define R_DACCONTROL5   0x1B  /* 右 DAC 数字音量 */
#define R_DACCONTROL16  0x26
#define R_DACCONTROL17  0x27
#define R_DACCONTROL20  0x2A
#define R_DACCONTROL21  0x2B  /* bit7 = 使能 DAC 通路 */
#define R_DACCONTROL24  0x2E  /* LOUT1 模拟音量 */
#define R_DACCONTROL25  0x2F  /* ROUT1 模拟音量 */
#define R_DACCONTROL26  0x30  /* LOUT2 模拟音量 */
#define R_DACCONTROL27  0x31  /* ROUT2 模拟音量 */

/* 模拟音量刻度（用户实测 + 厂商文档校准）：
 *   0x1E = 0dB，每步 1.5dB，0x00 = -45dB
 * （方案初稿曾写 -30dB，以 0x1E/-45dB 为准，见 00_config.lua 的锚点注释）
 */
#define ES_ANALOG_0DB     0x1E
#define ES_ANALOG_STEP_DB 1.5f
#define ES_ANALOG_MIN_DB  45.0f
/* 数字音量刻度（ES8388 手册）：0x00 = 0dB，每步 0.5dB，0xC0 = -96dB */
#define ES_DIGITAL_STEP_DB 0.5f
#define ES_DIGITAL_MAX     0xC0

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;
static i2s_chan_handle_t       s_tx  = NULL;
static uint32_t                s_fs  = 48000;
/* 硬件上 ES8388 寄存器 0x18 当前的值（单速 0x02 / 双速 0x20）。
 * 跨档换采样率时要靠它判断需不需要重写，避免每次都多一轮 I2C。 */
static int                     s_es_dacctrl2 = 0x02;
/* 硬件上 ES8388 寄存器 0x17 当前的值（16bit = 0x18 / 24bit = 0x00）。
 * 它跟着输出格式走：16bit 源和 24bit 源的采样率可能完全相同（都是 44.1k），
 * 所以判断"要不要重配"必须把格式一起看，光看采样率会漏掉切换。 */
static int                     s_es_dacctrl1 = 0x18;
/* 当前输出格式。默认 16bit = 本次改动之前的行为。 */
static board_audio_fmt_t       s_fmt = BOARD_AUDIO_FMT_16;
/* 输出格式 → (ES8388 0x17 值, I2S 槽位宽) 的唯一映射表。
 *
 * 0x17 的字长编码来自官方驱动 espressif__esp_codec_dev（es8388.c 的
 * `| (bits << 3)` + es_common.h 的枚举）：
 *     BIT_LENGTH_16BITS = 0x03 → 0x18
 *     BIT_LENGTH_24BITS = 0x00 → 0x00
 * 槽位宽为什么 24bit 必须用 32：见 board.h 里 board_audio_fmt_t 的注释（整除约束）。
 *
 * 放在这里（而不是跟 clk_plan 一起）是因为 board_es8388_check_critical() 要用它拼
 * 期望值，而那个函数在前面。 */
typedef struct {
    int      dacctrl1;    /* 写进 ES8388 寄存器 0x17 */
    int      slot_bits;   /* I2S 槽位宽：16 或 32 */
} fmt_plan_t;

static fmt_plan_t fmt_plan_for(board_audio_fmt_t fmt)
{
    fmt_plan_t p;
    switch (fmt) {
        case BOARD_AUDIO_FMT_24: p.dacctrl1 = 0x00; p.slot_bits = 32; break;
        case BOARD_AUDIO_FMT_16:
        default:                 p.dacctrl1 = 0x18; p.slot_bits = 16; break;
    }
    return p;
}

static int                     s_i2c_retries = 0;
static int                     s_es_init_fails = 0;
static int                     s_vol_percent = 40;   /* 默认 40%：70% 时用户反馈"音量太大且底噪明显" */
static bool                    s_ready = false;
/* 用户主动静音（音量页 K2）。必须是"粘住"的状态：切歌、换采样率都会
 * 临时静音再解开，如果直接 board_es8388_mute(false) 就会把用户设的静音冲掉。 */
static bool                    s_user_muted = false;

static void es_mute_apply(void) { board_es8388_mute(s_user_muted); }

void board_pa_enable(bool on)
{
#if BOARD_HAS_PA_EN
    gpio_set_level(BOARD_PIN_PA_EN, on ? 1 : 0);
#else
    /* 自制板无功放（已去掉 PAM8406），ES8388 内置耳放经耦合电容直推耳机。
     * 保留函数签名以兼容调用方，实际不做任何事。 */
    (void)on;
#endif
}

bool board_is_ready(void) { return s_ready; }
int  board_i2c_retries(void) { return s_i2c_retries; }
int  board_es8388_init_fails(void) { return s_es_init_fails; }
uint32_t board_i2s_get_rate(void) { return s_fs; }

/* ================================================================= I2C */

static uint8_t es_ro_mask(int reg)
{
    switch (reg) {
        case R_DACCONTROL1: return 0xFE;  /* bit0 只读 */
        case R_DACCONTROL2: return 0xBF;  /* bit6 只读 */
        default:            return 0xFF;
    }
}

esp_err_t board_es8388_write(int reg, int val)
{
    uint8_t buf[2] = { (uint8_t)reg, (uint8_t)val };
    return i2c_master_transmit(s_dev, buf, 2, 100);
}

int board_es8388_read(int reg)
{
    uint8_t r = (uint8_t)reg, v = 0;
    if (i2c_master_transmit_receive(s_dev, &r, 1, &v, 1, 100) != ESP_OK) {
        return -1;
    }
    return v;
}

/* 写 → 读回校验 → 不一致重试。这是原测试定位 I2C 静默写失败的核心对策。 */
esp_err_t board_es8388_write_verified(int reg, int val)
{
    uint8_t mask = es_ro_mask(reg);
    for (int attempt = 0; attempt < 8; attempt++) {
        if (board_es8388_write(reg, val) == ESP_OK) {
            int rb = board_es8388_read(reg);
            if (rb >= 0 && ((rb ^ val) & mask) == 0) {
                if (attempt) {
                    s_i2c_retries += attempt;
                    ESP_LOGW(TAG, "[i2c] reg %02X 重试 %d 次才写对", reg, attempt);
                }
                return ESP_OK;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    s_i2c_retries += 8;
    ESP_LOGE(TAG, "[i2c] !! reg %02X 写 0x%02X 校验失败（最后读回 0x%02X）",
             reg, val, board_es8388_read(reg));
    return ESP_FAIL;
}

int board_i2c_line_check(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << BOARD_PIN_I2C_SDA) | (1ULL << BOARD_PIN_I2C_SCL),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    vTaskDelay(pdMS_TO_TICKS(20));
    int sda = gpio_get_level(BOARD_PIN_I2C_SDA);
    int scl = gpio_get_level(BOARD_PIN_I2C_SCL);
    ESP_LOGI(TAG, "[i2c] 空闲电平 SDA(GPIO%d)=%d SCL(GPIO%d)=%d  %s",
             BOARD_PIN_I2C_SDA, sda, BOARD_PIN_I2C_SCL, scl,
             (sda && scl) ? "-> 总线空闲（若扫不到设备，查音频板供电）"
                          : "-> !! 有引脚被拉低，接线/供电有问题");
    return (sda && scl) ? 0 : -1;
}

int board_i2c_scan(void)
{
    ESP_LOGI(TAG, "[i2c] 扫描 0x08..0x77 ...");
    int found = 0;
    for (int a = 0x08; a <= 0x77; a++) {
        if (i2c_master_probe(s_bus, a, 50) == ESP_OK) {
            ESP_LOGI(TAG, "[i2c]   发现 7位=0x%02X (8位=0x%02X)", a, a << 1);
            found++;
        }
    }
    if (!found) {
        ESP_LOGW(TAG, "[i2c]   没扫到任何设备");
    }
    return found;
}

esp_err_t board_i2c_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port   = I2C_NUM_0,
        .sda_io_num = BOARD_PIN_I2C_SDA,
        .scl_io_num = BOARD_PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C 总线创建失败: %s", esp_err_to_name(err));
        return err;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BOARD_ES8388_ADDR,
        .scl_speed_hz    = 100000,   /* 厂商建议 100kHz，比 400kHz 稳 */
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "添加 ES8388 设备失败: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

/* ============================================================== ES8388 */

/* 完全照抄验证过的序列 */
typedef struct { int reg, val; } regval_t;

static const regval_t k_es8388_init[] = {
    { R_DACCONTROL3,  0x04 },   /* 先静音 */
    { R_CONTROL2,     0x50 },
    { R_CHIPPOWER,    0x00 },
    /* ★ ADC + line-in 整个断电：这台设备只放音，从不录音。
     *   复位默认 0xFC 是"ADC 通电"的 —— 厂商给的表里【没有】这一条，所以它一直开着。
     *   esp_codec_dev 驱动在 DAC-only 场景写的正是 0xFF（注释 "power down adc and line in"）。
     *   为什么要关：ADC 那套 bias/VREF 发生器白耗电，还会往共享的模拟基准/电源上
     *   注噪声 —— 听感就是"底噪"。 */
    { R_ADCPOWER,     0xFF },
    { 0x35,           0xA0 },   /* 关内部 DLL */
    { 0x37,           0xD0 },
    { 0x39,           0xD0 },
    { R_MASTERMODE,   0x00 },   /* ★ 0 = I2S 从机（时钟由 ESP32 出） */
    { R_DACPOWER,     0xC0 },
    { R_CONTROL1,     0x12 },
    { R_DACCONTROL1,  0x18 },   /* ★ 16bit I2S */
    { R_DACCONTROL2,  0x02 },   /* ★ MCLK/fs = 256 */
    { R_DACCONTROL16, 0x00 },
    { R_DACCONTROL17, 0x90 },
    { R_DACCONTROL20, 0x90 },
    { R_DACCONTROL21, 0x80 },
    { 0x2D,           0x00 },
    { R_DACCONTROL4,  0x00 },   /* 数字 0dB */
    { R_DACCONTROL5,  0x00 },
    { R_DACCONTROL24, ES_ANALOG_0DB },
    { R_DACCONTROL25, ES_ANALOG_0DB },
    { R_DACCONTROL26, ES_ANALOG_0DB },   /* ★ 0x30/0x31 必须写，否则 LOUT2/ROUT2 白掉 */
    { R_DACCONTROL27, ES_ANALOG_0DB },
    { R_DACPOWER,     0x3C },   /* 开 DAC + 全部输出 */
};

static const int k_critical[] = { R_MASTERMODE, R_DACCONTROL1, R_DACCONTROL2, R_CONTROL1 };

void board_es8388_check_critical(void)
{
    int printed = 0;
    char line[96] = {0};
    for (size_t i = 0; i < sizeof(k_critical) / sizeof(k_critical[0]); i++) {
        int v = board_es8388_read(k_critical[i]);
        printed += snprintf(line + printed, sizeof(line) - printed, "%02X=%02X ",
                            k_critical[i], v < 0 ? 0xFF : v);
    }
    /* ⚠️ 期望值必须动态取 —— 0x17 是随输出格式变的（16bit=0x18 / 24bit=0x00）。
     *   写死"期望 17=18"的话，播 24bit 曲目时这行日志会把人误导向假故障。 */
    fmt_plan_t fp = fmt_plan_for(s_fmt);
    ESP_LOGI(TAG, "[es8388] 关键寄存器复查: %s（期望 08=00 17=%02X 18=%02X，当前输出 %dbit）",
             line, fp.dacctrl1, s_es_dacctrl2, board_i2s_out_bits());
}

void board_es8388_dump(void)
{
    static const int regs[] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x08, 0x17, 0x18, 0x19, 0x1A, 0x1B,
        0x26, 0x27, 0x2A, 0x2B, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x35, 0x37, 0x39
    };
    char line[160];
    int p = 0;
    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        int v = board_es8388_read(regs[i]);
        p += snprintf(line + p, sizeof(line) - p, "%02X=%02X ", regs[i], v < 0 ? 0xFF : v);
        if ((i % 8) == 7 || i + 1 == sizeof(regs) / sizeof(regs[0])) {
            ESP_LOGI(TAG, "[es8388] %s", line);
            p = 0;
        }
    }
}

static int es8388_init(void)
{
    int fails = 0;
    for (size_t i = 0; i < sizeof(k_es8388_init) / sizeof(k_es8388_init[0]); i++) {
        if (board_es8388_write_verified(k_es8388_init[i].reg, k_es8388_init[i].val) != ESP_OK) {
            fails++;
        }
    }
    fails += (board_es8388_write_verified(R_DACCONTROL21, 0x80) != ESP_OK);
    fails += (board_es8388_write_verified(R_DACPOWER,    0x3C) != ESP_OK);

    int v = board_es8388_read(R_DACCONTROL3);
    if (v >= 0 && board_es8388_write_verified(R_DACCONTROL3, v & ~0x04) != ESP_OK) {
        fails++;
    }
    /* 初始化表里 0x18 写的是 0x02（单速 / MCLK/fs=256）、0x17 写的是 0x18（16bit），
     * 之后没有任何一条再动它们。把软件侧的记录同步过来 —— 否则上一首 24bit / 96k
     * 留下的 0x00 / 0x20 会让下次"要不要重配"的判断出错。 */
    s_es_dacctrl2 = 0x02;
    s_es_dacctrl1 = 0x18;
    s_fmt         = BOARD_AUDIO_FMT_16;
    s_es_init_fails = fails;
    ESP_LOGI(TAG, "[es8388] 初始化完成：失败 %d 项，I2C 累计重试 %d 次", fails, s_i2c_retries);
    return fails;
}

/* ============================================ 混合音量（模拟优先） */

void board_vol_set(int percent)
{
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;
    s_vol_percent = percent;

    /* 0% → -60dB，100% → 0dB */
    float atten_db = 60.0f * (1.0f - (float)percent / 100.0f);
    int analog, digital;

    if (atten_db <= ES_ANALOG_MIN_DB) {
        /* 模拟音量范围内：只动模拟，数字保持 0dB（最干净） */
        analog  = ES_ANALOG_0DB - (int)lroundf(atten_db / ES_ANALOG_STEP_DB);
        digital = 0x00;
    } else {
        /* 模拟到底（-45dB），剩余衰减交给数字 */
        analog  = 0x00;
        digital = (int)lroundf((atten_db - ES_ANALOG_MIN_DB) / ES_DIGITAL_STEP_DB);
    }
    if (analog < 0x00) analog = 0x00;
    if (analog > ES_ANALOG_0DB) analog = ES_ANALOG_0DB;
    if (digital > ES_DIGITAL_MAX) digital = ES_DIGITAL_MAX;

    board_es8388_write(R_DACCONTROL24, analog);
    board_es8388_write(R_DACCONTROL25, analog);
    board_es8388_write(R_DACCONTROL26, analog);
    board_es8388_write(R_DACCONTROL27, analog);
    board_es8388_write(R_DACCONTROL4,  digital);
    board_es8388_write(R_DACCONTROL5,  digital);

    ESP_LOGI(TAG, "[vol] %d%% → 衰减 %.1fdB → 模拟 0x%02X / 数字 0x%02X",
             percent, atten_db, analog, digital);
}

int board_vol_get(void) { return s_vol_percent; }

/* ================================================== 屏幕背光（LEDC PWM） */

#define BL_LEDC_TIMER    LEDC_TIMER_1
#define BL_LEDC_CHANNEL  LEDC_CHANNEL_1
#define BL_DUTY_MAX      ((1 << 10) - 1)      /* 10 bit */

esp_err_t board_backlight_init(void)
{
    ledc_timer_config_t t = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = BL_LEDC_TIMER,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .freq_hz         = 5000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&t);
    if (err != ESP_OK) return err;
    ledc_channel_config_t c = {
        .gpio_num   = BOARD_PIN_LCD_BLK,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = BL_LEDC_CHANNEL,
        .timer_sel  = BL_LEDC_TIMER,
        .duty       = 0,                       /* 先灭，等画面准备好再点亮，避免上电花屏 */
        .hpoint     = 0,
    };
    err = ledc_channel_config(&c);
    if (err != ESP_OK) return err;
    ESP_LOGI(TAG, "[bl] 背光 PWM 就绪 (GPIO%d, 5kHz/10bit)", BOARD_PIN_LCD_BLK);
    return ESP_OK;
}

void board_backlight_set(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    uint32_t duty = (uint32_t)BL_DUTY_MAX * percent / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL);
}

/* DACCONTROL3(0x19) bit2 = 软静音 */
void board_es8388_mute(bool mute)
{
    int v = board_es8388_read(R_DACCONTROL3);
    if (v < 0) return;
    int nv = mute ? (v | 0x04) : (v & ~0x04);
    if (nv != v) board_es8388_write_verified(R_DACCONTROL3, nv);
}

/* 用户静音：DACCONTROL3 bit2 是硬静音（真·无声），不是"音量调到很小"。
 * 之前音量页按 K2 只是把音量设成 0（总衰减 -60dB），用户反馈"不是真静音" ——
 * 模拟 -45dB + 数字 -15dB 之后还剩一点声音，而且底噪仍在。 */
void board_es8388_set_mute(bool mute)
{
    s_user_muted = mute;
    es_mute_apply();
    ESP_LOGW(TAG, "[mute] %s", mute ? "已静音（DAC 硬静音）" : "取消静音");
}

bool board_es8388_is_muted(void) { return s_user_muted; }

/* ================================================================= I2S */

/* ==================== 采样率 → 时钟方案 ====================
 * ES8388 手册把 fs 分成两档，由寄存器 0x18 的 DACFsMode（bit5）选择：
 *
 *   单速   8k – 50kHz    MCLK/fs = 256   0x18 bit4:0 = 00010  → 0x02
 *   双速  50k – 100kHz   MCLK/fs = 128   0x18 bit5=1 + 00000  → 0x20
 *
 * ⚠️ 换档【必须同时改两处】，只改一处不报错、只表现为没声音或严重失真：
 *     1) ES8388 的 0x18   —— 它按哪个比值去解 MCLK
 *     2) ESP32 的 mclk_multiple —— 它输出多快的 MCLK
 *   之前 board_i2s_set_rate() 只改了 ESP32 那侧、压根不碰 ES8388，
 *   所以只要放一个 96k 音源进来就是【静默出错】。
 *
 * bclk_div = mclk_multiple / (2 × 槽位宽)。当前是 16bit 数据装在 16bit 槽，立体声：
 *     单速 256 / 32 = 8（与实测 WS 一致）
 *     双速 128 / 32 = 4
 *
 * 注：ES8388 上限是 96kHz（官方手册 8k–96k，不是 192k）。*/
#define BOARD_FS_MIN       8000
#define BOARD_FS_MAX       96000
#define BOARD_FS_MID       50000     /* 单速/双速分界 */

typedef struct {
    int      dacctrl2;        /* 写进 ES8388 寄存器 0x18（MCLK/fs 比值） */
    uint32_t mclk_multiple;   /* I2S_MCLK_MULTIPLE_xxx */
    uint32_t bclk_div;
} clk_plan_t;

static clk_plan_t clk_plan_for(uint32_t fs, int slot_bits)
{
    clk_plan_t p;
    if (fs > BOARD_FS_MID) {
        p.dacctrl2      = 0x20;   /* bit5=1 双速 + ratio 00000(=128) */
        p.mclk_multiple = I2S_MCLK_MULTIPLE_128;
    } else {
        p.dacctrl2      = 0x02;   /* bit5=0 单速 + ratio 00010(=256) */
        p.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    }
    /* bclk_div = mclk_multiple / (2 × 槽位宽)（立体声 2 槽），必须整除：
     *   16bit 槽 → 256/32 = 8（单速）、128/32 = 4（双速）
     *   32bit 槽 → 256/64 = 4（单速）、128/64 = 2（双速）
     * ⚠️ 槽位宽改了而这里没跟着改，WS 就会变成 0.5×/2× —— 表现是半速变调，
     *    而且不报任何错。所以槽位宽是【参数】而不是常量。 */
    p.bclk_div = p.mclk_multiple / (2 * (uint32_t)slot_bits);
    return p;
}

static void i2s_fill_cfg(i2s_std_config_t *cfg, uint32_t fs, int slot_bits)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->clk_cfg.sample_rate_hz = fs;
    /* ⚠️ 查过底噪的"时钟抖动"这条路，【在 ESP32-S3 上是死的】，别再试：
     *   soc_caps.h 里 S3 的 I2S 只有 SOC_I2S_SUPPORTS_XTAL 和 SOC_I2S_SUPPORTS_PLL_F160M，
     *   【没有】SOC_I2S_SUPPORTS_APLL —— 音频专用 PLL 在 S3 上不给 I2S 用。
     *   所以 MCLK 只能从 160MHz 分数分频得到：48k 时 160/12.288 = 13.0208 不是整数，
     *   瞬时周期有抖动。XTAL(40MHz) 更差（40/12.288 = 3.255，抖动达 ±25ns）。
     *   结论：S3 上没法把 MCLK 做成整数分频，这条优化路径不存在。 */
    clk_plan_t plan = clk_plan_for(fs, slot_bits);
    cfg->clk_cfg.clk_src        = I2S_CLK_SRC_DEFAULT;   /* = PLL_F160M */
    cfg->clk_cfg.mclk_multiple  = plan.mclk_multiple;
    cfg->clk_cfg.bclk_div       = plan.bclk_div;

    /* ⚠️ 24bit 档刻意声明成 32bit data + 32bit slot，【不】用 I2S_DATA_BIT_WIDTH_24BIT：
     *   24bit 数据在 DMA 缓冲里怎么摆，IDF 有自己的约定；而我们的摆放是由
     *   audio_engine 的 ae_normalize() 自己保证的（MSB 左对齐到 32bit 字）。
     *   声明成 32bit 就完全绕开了那个约定 —— 摆错了是我们自己的错，好定位。 */
    i2s_data_bit_width_t data_bits = (slot_bits == 32) ? I2S_DATA_BIT_WIDTH_32BIT
                                                      : I2S_DATA_BIT_WIDTH_16BIT;
    cfg->slot_cfg = (i2s_std_slot_config_t)
        I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(data_bits, I2S_SLOT_MODE_STEREO);
    cfg->slot_cfg.slot_bit_width = (slot_bits == 32) ? I2S_SLOT_BIT_WIDTH_32BIT
                                                     : I2S_SLOT_BIT_WIDTH_16BIT;
    /* ⚠️ ws_width 必须跟着槽位宽：Philips I2S 的 WS 是 50% 占空比，
     *   槽位 32 而 ws_width 还写 16 的话 WS 脉宽就不对了。 */
    cfg->slot_cfg.ws_width       = slot_bits;
    cfg->gpio_cfg.mclk = BOARD_PIN_I2S_MCLK;
    cfg->gpio_cfg.bclk = BOARD_PIN_I2S_BCLK;
    cfg->gpio_cfg.ws   = BOARD_PIN_I2S_WS;
    cfg->gpio_cfg.dout = BOARD_PIN_I2S_DOUT;
#if BOARD_HAS_I2S_DIN
    cfg->gpio_cfg.din  = BOARD_PIN_I2S_DIN;
#else
    cfg->gpio_cfg.din  = I2S_GPIO_UNUSED;   /* 自制板不录音：ES8388 ADC 已断电 */
#endif
}

static esp_err_t i2s_init(uint32_t fs)
{
    s_fs = fs;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;   /* 欠载自动补 0，不重放 DMA 旧数据 */
    /* 显式定 DMA 深度（位置推算要用它，且它决定抗抖动的极限）：
     *   8 个描述符 × 1023 帧 = 8184 帧 ≈ 185ms @44.1k */
    chan_cfg.dma_desc_num  = BOARD_I2S_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = BOARD_I2S_DMA_FRAME_NUM;
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, NULL);
    if (err != ESP_OK) return err;
    i2s_std_config_t cfg;
    i2s_fill_cfg(&cfg, fs, 16);   /* 开机固定 16bit（= 改动前的默认），播放时按源位深切 */
    err = i2s_channel_init_std_mode(s_tx, &cfg);
    if (err != ESP_OK) return err;
    return i2s_channel_enable(s_tx);
}

/* 真正动时钟/格式的那一段（不看缓存）——board_i2s_set_format 用它 */
static esp_err_t i2s_reconfig(uint32_t sample_rate_hz, board_audio_fmt_t fmt)
{
    /* ★ 必须先静音再动时钟：DAC 还在输出时重配 I2S 时钟会"咔"一声 */
    board_es8388_mute(true);
    vTaskDelay(pdMS_TO_TICKS(30));      /* 让 ES8388 的静音真的生效（内部有去加重/软斜坡） */

    fmt_plan_t fp = fmt_plan_for(fmt);
    clk_plan_t plan = clk_plan_for(sample_rate_hz, fp.slot_bits);

    /* ★ 先让 ES8388 知道该按哪个 MCLK/fs 比值解，再改 ESP32 的时钟。
     *   跨档（≤50k ↔ >50k）时必须重写 0x18，否则 ES8388 还按旧比值解新时钟
     *   —— 表现是没声音或严重失真，而且不报任何错。 */
    if (s_es_dacctrl2 != plan.dacctrl2) {
        if (board_es8388_write_verified(R_DACCONTROL2, plan.dacctrl2) == ESP_OK) {
            s_es_dacctrl2 = plan.dacctrl2;
        } else {
            ESP_LOGE(TAG, "[clk] ES8388 0x18 写 0x%02X 失败 —— %" PRIu32
                          " Hz 可能失真（单速/双速没切过去）", plan.dacctrl2, sample_rate_hz);
        }
    }
    /* 同理：字长（0x17）必须在数据格式换过去【之前】写好，否则 ES8388 会按旧的
     * 字长去解新数据 —— 16bit 被当 24bit 解，表现是噪音/极小声，同样不报错。 */
    if (s_es_dacctrl1 != fp.dacctrl1) {
        if (board_es8388_write_verified(R_DACCONTROL1, fp.dacctrl1) == ESP_OK) {
            s_es_dacctrl1 = fp.dacctrl1;
        } else {
            ESP_LOGE(TAG, "[clk] ES8388 0x17 写 0x%02X 失败 —— %dbit 输出可能噪音",
                     fp.dacctrl1, board_i2s_out_bits());
        }
    }

    i2s_std_config_t cfg;
    i2s_fill_cfg(&cfg, sample_rate_hz, fp.slot_bits);
    i2s_channel_disable(s_tx);
    esp_err_t err = i2s_channel_reconfig_std_clock(s_tx, &cfg.clk_cfg);
    if (err == ESP_OK) {
        err = i2s_channel_reconfig_std_slot(s_tx, &cfg.slot_cfg);
    }
    if (err == ESP_OK) {
        s_fs  = sample_rate_hz;
        s_fmt = fmt;
    }
    i2s_channel_enable(s_tx);
    vTaskDelay(pdMS_TO_TICKS(50));      /* 新时钟稳定 + DMA 缓冲先填上静音 */

    es_mute_apply();                    /* 用户静音要保持，不能在这里被解开 */
    /* 用 100ms 窗口量才准（20ms 窗口的量化误差能到几百 Hz，会误判成"时钟不准"） */
    int ws = board_measure_ws_hz(100);
    /* ⚠️ 实测 WS 是验证这次改动【最关键】的一项：槽位宽从 16 改到 32 时，
     *   bclk_div 必须跟着从 /32 变成 /64，否则 WS 会变成一半（听感就是半速变调）。
     *   所以把槽位宽和它对应的理论 bclk_div 都打进日志，便于一眼核对。 */
    ESP_LOGW(TAG, "[clk] 输出格式切到 %dbit（%dbit 槽, bclk_div=%" PRIu32
                  ", ES8388 0x17=%02X, 0x18=%02X, MCLK/fs=%" PRIu32 "）: %s，"
                  "实测 WS=%d / 期望 %" PRIu32 "（偏差 %d Hz / %d‰）",
             board_i2s_out_bits(), fp.slot_bits, plan.bclk_div,
             fp.dacctrl1, plan.dacctrl2, plan.mclk_multiple / sample_rate_hz,
             esp_err_to_name(err), ws, sample_rate_hz,
             ws - (int)sample_rate_hz,
             (int)((int64_t)(ws - (int)sample_rate_hz) * 1000 / (int)sample_rate_hz));
    return err;
}

esp_err_t board_i2s_set_format(uint32_t sample_rate_hz, board_audio_fmt_t fmt)
{
    if (s_tx == NULL) return ESP_ERR_INVALID_STATE;
    /* ES8388 只支持 8k–96k（官方手册，不是 192k）。超出范围就【拒绝】而不是
     * 硬按错误的时钟放 —— 那样会变调/爆音，比明确报错难查得多。 */
    if (sample_rate_hz < BOARD_FS_MIN || sample_rate_hz > BOARD_FS_MAX) {
        ESP_LOGE(TAG, "[clk] 采样率 %" PRIu32 " Hz 超出 ES8388 能力（%d–%d Hz），拒绝切换",
                 sample_rate_hz, BOARD_FS_MIN, BOARD_FS_MAX);
        return ESP_ERR_INVALID_ARG;
    }
    /* ★ 必须把【格式】也纳入判断。只比采样率的话，16bit 44.1k → 24bit 44.1k
     *   会因为"采样率没变"而直接返回，24bit 数据就被当成 16bit 解释了。 */
    if (sample_rate_hz == s_fs && fmt == s_fmt) return ESP_OK;
    return i2s_reconfig(sample_rate_hz, fmt);
}

esp_err_t board_i2s_set_rate(uint32_t sample_rate_hz)
{
    /* 保持当前输出格式（老调用方的语义不变） */
    return board_i2s_set_format(sample_rate_hz, s_fmt);
}

int board_i2s_out_bits(void)
{
    return (s_fmt == BOARD_AUDIO_FMT_24) ? 24 : 16;
}

esp_err_t board_i2s_write(const void *data, size_t bytes, size_t *written, TickType_t timeout)
{
    if (s_tx == NULL) return ESP_ERR_INVALID_STATE;
    return i2s_channel_write(s_tx, data, bytes, written, timeout);
}

/* ============================================================== PCNT 测频 */

int board_measure_ws_hz(int window_ms)
{
    pcnt_unit_config_t ucfg = { .high_limit = 32767, .low_limit = -32768 };
    pcnt_unit_handle_t unit = NULL;
    if (pcnt_new_unit(&ucfg, &unit) != ESP_OK) return -1;
    pcnt_chan_config_t ccfg = { .edge_gpio_num = BOARD_PIN_I2S_WS, .level_gpio_num = -1 };
    pcnt_channel_handle_t chan = NULL;
    if (pcnt_new_channel(unit, &ccfg, &chan) != ESP_OK) { pcnt_del_unit(unit); return -1; }
    pcnt_channel_set_edge_action(chan, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                 PCNT_CHANNEL_EDGE_ACTION_HOLD);
    pcnt_unit_enable(unit);
    pcnt_unit_clear_count(unit);
    pcnt_unit_start(unit);
    vTaskDelay(pdMS_TO_TICKS(window_ms));
    int c = 0;
    pcnt_unit_get_count(unit, &c);
    pcnt_unit_stop(unit);
    pcnt_unit_disable(unit);
    pcnt_del_channel(chan);
    pcnt_del_unit(unit);
    /* WS 每个采样周期一个完整方波 → 边沿数 = 频率 × 2 × 窗口秒数 / 2 = 频率 × 窗口秒数 */
    return (int)((int64_t)c * 1000 / window_ms);
}

/* ============================================================== 测试音 */

void board_play_tone(int ms, float freq_hz, int amplitude, bool square)
{
    enum { CHUNK = 256 };
    /* 相位必须是【每次调用】从 0 开始：跨调用保持相位会让起音落在非零相位上 → 起音一声咔 */
    float phase = 0.0f;
    const int total = (int)((int64_t)s_fs * ms / 1000);
    /* 5ms 淡入淡出：任何突变的起音/收尾都会听成"咔"，包络是最省事的消除法 */
    const int fade = s_fs * 5 / 1000;

    int16_t *buf = heap_caps_malloc(CHUNK * 2 * sizeof(int16_t),
                                    MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf) {
        ESP_LOGE(TAG, "[tone] 缓冲分配失败");
        return;
    }

    int done = 0;
    while (done < total) {
        int n = (total - done) < CHUNK ? (total - done) : CHUNK;
        for (int i = 0; i < n; i++) {
            int idx = done + i;
            float s;
            if (amplitude == 0 || freq_hz == 0.0f) {
                s = 0.0f;
            } else if (square) {
                s = (phase < 0.5f) ? 1.0f : -1.0f;
            } else {
                s = sinf(phase * 6.2831853f);
            }
            float env = 1.0f;
            if (fade > 0) {
                if (idx < fade) {
                    env = (float)idx / fade;
                } else if (idx >= total - fade) {
                    env = (float)(total - 1 - idx) / fade;
                }
            }
            int16_t v = (int16_t)(s * amplitude * env);
            buf[i * 2 + 0] = v;
            buf[i * 2 + 1] = v;
            if (freq_hz > 0.0f) {
                phase += freq_hz / (float)s_fs;
                if (phase >= 1.0f) phase -= 1.0f;
            }
        }
        /* 写完为止：部分写入要重试，否则波形中间会有断口（也是"咔"） */
        size_t off = 0;
        while (off < (size_t)n * 4) {
            size_t w = 0;
            if (board_i2s_write((uint8_t *)buf + off, (size_t)n * 4 - off, &w,
                                pdMS_TO_TICKS(2000)) != ESP_OK || w == 0) {
                ESP_LOGE(TAG, "[tone] i2s write 失败");
                heap_caps_free(buf);
                return;
            }
            off += w;
        }
        done += n;
    }
    heap_caps_free(buf);
}

/* ================================================================ 初始化 */

esp_err_t board_init(void)
{
#if BOARD_HAS_PA_EN
    /* PA 使能脚先配好（默认关，避免上电爆音） */
    gpio_config_t pa = {
        .pin_bit_mask = 1ULL << BOARD_PIN_PA_EN,
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&pa);
    gpio_set_level(BOARD_PIN_PA_EN, 0);
#endif

    board_i2c_line_check();
    if (board_i2c_init() != ESP_OK) return ESP_FAIL;
    board_i2c_scan();

    if (i2s_init(48000) != ESP_OK) {
        ESP_LOGE(TAG, "I2S 初始化失败");
        return ESP_FAIL;
    }

    int fails = es8388_init();
    board_es8388_check_critical();
    board_vol_set(s_vol_percent);
    board_pa_enable(true);
    s_ready = true;

    ESP_LOGW(TAG, "板级就绪: ES8388 失败 %d 项 / I2C 重试 %d 次", fails, s_i2c_retries);
    return fails ? ESP_FAIL : ESP_OK;
}

int board_selftest(void)
{
    int bad = 0;
    ESP_LOGI(TAG, "================= 板级自检 =================");
#if BOARD_HAS_PA_EN
    ESP_LOGI(TAG, "[hw] 板型=面包板 | SDA=%d SCL=%d | MCK=%d BCK=%d WS=%d | out=%d in=%d | PA=%d",
             BOARD_PIN_I2C_SDA, BOARD_PIN_I2C_SCL, BOARD_PIN_I2S_MCLK,
             BOARD_PIN_I2S_BCLK, BOARD_PIN_I2S_WS, BOARD_PIN_I2S_DOUT,
             BOARD_PIN_I2S_DIN, BOARD_PIN_PA_EN);
#else
    ESP_LOGI(TAG, "[hw] 板型=自制PCB V2 | SDA=%d SCL=%d | MCK=%d BCK=%d WS=%d | out=%d",
             BOARD_PIN_I2C_SDA, BOARD_PIN_I2C_SCL, BOARD_PIN_I2S_MCLK,
             BOARD_PIN_I2S_BCLK, BOARD_PIN_I2S_WS, BOARD_PIN_I2S_DOUT);
#endif

    if (board_i2c_line_check() != 0) bad++;

    if (s_bus == NULL && board_i2c_init() != ESP_OK) return bad + 1;

    int found = board_i2c_scan();
    if (found == 0) {
        ESP_LOGE(TAG, "!! 总线上没有任何设备 —— 检查音频板 VCC/GND/SDA/SCL");
        bad++;
    } else {
        int id = board_es8388_read(R_CONTROL1);
        ESP_LOGI(TAG, "[es8388] 地址 0x%02X 应答，CONTROL1(0x00)=0x%02X",
                 BOARD_ES8388_ADDR, id < 0 ? 0xFF : id);
    }

    if (s_tx == NULL && i2s_init(48000) != ESP_OK) {
        ESP_LOGE(TAG, "!! I2S 初始化失败");
        return bad + 1;
    }
    board_pa_enable(true);

    bad += es8388_init();
    board_es8388_check_critical();
    board_es8388_dump();
    board_vol_set(s_vol_percent);

    vTaskDelay(pdMS_TO_TICKS(200));
    int ws = board_measure_ws_hz(100);
    ESP_LOGI(TAG, "[clk] 48kHz 实测 WS=%d Hz（期望 ~48000）", ws);
    if (ws < 47000 || ws > 49000) {
        ESP_LOGE(TAG, "!! WS 频率不对：检查 MCLK/BCLK 接线与 mclk_multiple");
        bad++;
    }

    s_ready = true;
    ESP_LOGI(TAG, "=========== 自检结束：失败 %d 项，I2C 重试 %d 次 ===========", bad, s_i2c_retries);
    return bad;
}
