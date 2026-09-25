/*
 * ESP-Music — 板级硬件抽象（ESP32-S3-COREBOARD V1.4 + ES8388/PAM8406 + ST7735 + K1-K4）
 *
 * 接线（原理图与本机原生测试双向核实，别改）：
 *   SDA=GPIO1  SCL=GPIO2 | MCK=42 BCK=41 WS=39 | ESP出(GPIO40)=音频板 DO(第5脚)
 *   ESP收(GPIO38)=音频板 DI(第7脚) | PEN=GPIO47(高有效, PAM8406 使能)
 *
 * 设计取舍：ES8388 走【直接 I2C 写寄存器 + 写后读回校验重试】，不经过 esp_codec_dev。
 * 理由：唯一被真机验证过的序列就是直写形式；且 esp_codec_dev 的 open() 会把
 * LOUT2/ROUT2(0x30/0x31) 清成 -30dB 并覆盖音量，用它反而要额外补偿。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 引脚 ---------------- */
#define BOARD_PIN_I2C_SDA   1
#define BOARD_PIN_I2C_SCL   2
#define BOARD_PIN_I2S_MCLK  42
#define BOARD_PIN_I2S_BCLK  41
#define BOARD_PIN_I2S_WS    39
#define BOARD_PIN_I2S_DOUT  40   /* ESP -> 音频板 DO(第5脚) */
#define BOARD_PIN_I2S_DIN   38   /* 音频板 DI(第7脚) -> ESP */
#define BOARD_PIN_PA_EN     47   /* PAM8406 使能，高有效 */

#define BOARD_PIN_LCD_SCLK  4
#define BOARD_PIN_LCD_MOSI  5
#define BOARD_PIN_LCD_RST   6
#define BOARD_PIN_LCD_DC    7
#define BOARD_PIN_LCD_CS    15
#define BOARD_PIN_LCD_BLK   16

#define BOARD_PIN_KEY1      3    /* ⚠️ GPIO3 是 strapping 脚，开机按住会进 JTAG */
#define BOARD_PIN_KEY2      8
#define BOARD_PIN_KEY3      18
#define BOARD_PIN_KEY4      17

#define BOARD_ES8388_ADDR   0x11 /* 7 位；8 位形式是 0x22 */

/* I2S DMA 深度（帧）：8 × 1023 = 8184 帧 ≈ 185ms @44.1k
 * 既是抗 WiFi/BT 尖峰的主力，也是"真实播放位置"推算的偏移量。
 * ⚠️ 每描述符上限 4096 字节（16bit 立体声 = 1024 帧），驱动会把 frame_num 截到 1023，
 *    所以这里直接写 1023，避免 "dma frame num is out of dma buffer size" 警告。 */
#define BOARD_I2S_DMA_DESC_NUM   8
#define BOARD_I2S_DMA_FRAME_NUM  1023
#define BOARD_I2S_DMA_FRAMES     (BOARD_I2S_DMA_DESC_NUM * BOARD_I2S_DMA_FRAME_NUM)

/* ---------------- 生命周期 ---------------- */
/* I2C → I2S → ES8388 初始化 → 关键寄存器复查 → PA 使能 */
esp_err_t board_init(void);

/* 仅初始化 I2C 总线（供屏幕校准等早期诊断用） */
esp_err_t board_i2c_init(void);

/* ---------------- 诊断（M0 的核心价值：把硬件"看得见"） ---------------- */
/* 检查 SDA/SCL 空闲电平；返回 0=总线空闲  -1=有引脚被拉低 */
int  board_i2c_line_check(void);
/* 扫描 0x08..0x77，返回发现的设备数，并逐个打印 */
int  board_i2c_scan(void);
/* 关键寄存器复查（0x08 MASTERMODE / 0x17 DACCONTROL1 / 0x18 DACCONTROL2 / 0x00 CONTROL1） */
void board_es8388_check_critical(void);
/* 常用寄存器 dump */
void board_es8388_dump(void);
/* I2C 写后校验的累计重试次数 —— 这个数大就说明 I2C 不稳（硬件层最重要的指标） */
int  board_i2c_retries(void);
/* ES8388 初始化失败的寄存器数（0 = 全对） */
int  board_es8388_init_fails(void);
/* 用 PCNT 实测 WS 频率（Hz）。期望 = 采样率。这是"时钟对不对"的唯一真相来源 */
int  board_measure_ws_hz(int window_ms);
/* 一条龙自检：行电平 → 扫描 → 初始化 → 复查 → dump → 测 WS。返回失败项数 */
int  board_selftest(void);

/* ---------------- ES8388 寄存器直访 ---------------- */
esp_err_t board_es8388_write(int reg, int val);
int       board_es8388_read(int reg);              /* 失败返回 -1 */
esp_err_t board_es8388_write_verified(int reg, int val);

/* ---------------- 静音 ---------------- */
/* 临时静音：切歌/换采样率的过渡用，调用方要负责解开 */
void board_es8388_mute(bool mute);
/* 用户主动静音：ES8388 的 DAC 硬静音（真·无声），且【粘住】——
 * 切歌、换采样率时那些临时静音解开后会恢复成这个状态，不会被冲掉 */
void board_es8388_set_mute(bool mute);
bool board_es8388_is_muted(void);

/* ---------------- 屏幕背光（GPIO16，LEDC PWM） ---------------- */
esp_err_t board_backlight_init(void);
void      board_backlight_set(int percent);   /* 0..100 */

/* ---------------- 音量（混合策略：先动模拟，模拟到底再动数字） ---------------- */
/* 0..100。模拟 0x2E-0x31：0x1E=0dB，每步 1.5dB，0x00=-45dB；超出部分才用数字 0x1A/0x1B */
void board_vol_set(int percent);
int  board_vol_get(void);

/* ---------------- 播放控制 ---------------- */
void      board_pa_enable(bool on);
esp_err_t board_i2s_write(const void *data, size_t bytes, size_t *written, TickType_t timeout);
/* 改变采样率（重配时钟，不做重采样；S3 无硬件 ASRC） */
esp_err_t board_i2s_set_rate(uint32_t sample_rate_hz);
uint32_t  board_i2s_get_rate(void);
bool      board_is_ready(void);

/* 注：MCLK 抖动这条路在 ESP32-S3 上走不通 —— S3 的 I2S 不支持 APLL
 * （soc_caps.h 只有 SOC_I2S_SUPPORTS_XTAL / PLL_F160M），详见 board.c 的注释。
 * 排查底噪请看 ADC 断电（k_es8388_init 里的 R_ADCPOWER=0xFF）那一处。 */

/* ---------------- 测试音（M1 用耳朵验收：干净单音 = 链路正常） ---------------- */
/* freq=0 或 amp=0 即静音；square=true 换音色便于区分 */
void board_play_tone(int ms, float freq_hz, int amplitude, bool square);

#ifdef __cplusplus
}
#endif
