/*
 * ESP-Music — 板级硬件抽象（自制 PCB：ESP32-S3-WROOM-1-N16R8 + ES8388 + ST7735 + K1-K4）
 *
 * 接线（对应自制 PCB，改板请同步改这里）：
 *   SDA=GPIO1  SCL=GPIO2 | MCK=42 BCK=41 WS=39 | DOUT=GPIO40 -> ES8388 DSDIN
 *   K1 由 GPIO3 改为 GPIO38（避开 strapping 脚）
 *   无功放：ES8388 内置耳放经耦合电容直推 3.5mm 耳机座
 *   不录音：ES8388 ADC 已断电，I2S 只发不收，无 DIN
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
#include "sdkconfig.h"   /* 板型开关 CONFIG_BOARD_VARIANT_* 在这里 */

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 引脚 ---------------- */
#define BOARD_PIN_I2C_SDA   1
#define BOARD_PIN_I2C_SCL   2
#define BOARD_PIN_I2S_MCLK  42
#define BOARD_PIN_I2S_BCLK  41
#define BOARD_PIN_I2S_WS    39
#define BOARD_PIN_I2S_DOUT  40   /* ESP -> ES8388 DSDIN */

/* ---- 板型差异（只有这 3 个引脚不同）
 * 开关：CONFIG_BOARD_VARIANT_BREADBOARD（menuconfig → ESP-Music 配置，
 *       或直接往 sdkconfig.local 里加一行 CONFIG_BOARD_VARIANT_BREADBOARD=y）
 *       —— 不选就是自制 PCB V2。----------------------------------------------------
 *                    面包板（开发板+音频模块）      自制 PCB V2
 *   I2S_DIN          GPIO38（录音回读）              不接（ADC 断电，只发不收）
 *   PA_EN            GPIO47（PAM8406 功放使能）      不接（无功放，耳放直推耳机）
 *   KEY1             GPIO3（strapping 脚）           GPIO38（避开 strapping）
 */
#if CONFIG_BOARD_VARIANT_BREADBOARD
#  define BOARD_HAS_PA_EN    1
#  define BOARD_HAS_I2S_DIN  1
#  define BOARD_PIN_I2S_DIN  38
#  define BOARD_PIN_PA_EN    47
#  define BOARD_PIN_KEY1     3    /* ⚠️ GPIO3 是 strapping 脚，开机按住会进 JTAG */
#else   /* 自制 PCB V2 */
#  define BOARD_HAS_PA_EN    0    /* 无功放：ES8388 内置耳放经耦合电容直推耳机 */
#  define BOARD_HAS_I2S_DIN  0    /* 不录音：ES8388 ADC 已断电，I2S 只发不收 */
#  define BOARD_PIN_KEY1     38   /* ⚠️ GPIO3 是 strapping 脚，上电被拉低会进 JTAG */
#endif

/* ---- 板型派生的新硬件开关（旋转编码器 / TF 卡）----
 * 为什么由板型【派生】而不是各给一个 Kconfig bool：
 *   sdkconfig 里一旦写进过值，Kconfig 的 default 就不会再跟着
 *   BOARD_VARIANT_BREADBOARD 重新求值 —— 换了板型会留下"面包板变体 + 硬件开关=开"
 *   这种自相矛盾的状态。派生就没有这个状态：改 sdkconfig.local 里那一行板型，
 *   两个开关一起跟着变。
 * 调试覆盖：CONFIG_BOARD_FORCE_ENCODER / CONFIG_BOARD_FORCE_TF
 *   （见 main/Kconfig.projbuild；只在面包板上临时接线验证时才需要）。*/
#if CONFIG_BOARD_VARIANT_BREADBOARD && !CONFIG_BOARD_FORCE_ENCODER
#  define BOARD_HAS_ENCODER  0
#else
#  define BOARD_HAS_ENCODER  1
#endif
#if CONFIG_BOARD_VARIANT_BREADBOARD && !CONFIG_BOARD_FORCE_TF
#  define BOARD_HAS_TF       0
#else
#  define BOARD_HAS_TF       1
#endif

#define BOARD_PIN_LCD_SCLK  4
#define BOARD_PIN_LCD_MOSI  5
#define BOARD_PIN_LCD_RST   6
#define BOARD_PIN_LCD_DC    7
#define BOARD_PIN_LCD_CS    15
#define BOARD_PIN_LCD_BLK   16

#define BOARD_PIN_KEY2      8
#define BOARD_PIN_KEY3      18
#define BOARD_PIN_KEY4      17

/* ---------------- V2 新增硬件（旋转编码器 / TF 卡）----------------
 * 引脚来源：pcb/接线表与引脚定义.md。
 * 面包板变体下这些宏仍然定义着（方便对照/临时接线调试），但驱动组件按
 * BOARD_HAS_ENCODER / BOARD_HAS_TF 条件编译，不会真的去碰这些脚。
 *
 * ⚠️ TF 卡与屏幕【共用 SPI2】：CLK=IO4、MOSI=IO5，靠 CS 区分（屏 IO15 / 卡 IO12）。
 *    DAT1/DAT2 悬空 → 只能 SPI 模式，不是 SD 1-bit 模式。 */
#define BOARD_PIN_ENC_A     9
#define BOARD_PIN_ENC_B     10
#define BOARD_PIN_ENC_SW    11    /* 编码器按下；低电平有效（与 K1-K4 一致） */

#define BOARD_PIN_TF_CS     12
#define BOARD_PIN_TF_MISO   13
#define BOARD_PIN_TF_CD     14    /* 卡检测；低电平 = 有卡 */

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

/* ---------------- 共享 SPI 总线（屏幕 + TF 卡）----------------
 * 这条总线原来只在 ui_display.c 里初始化。提到 board 只是给它一个明确归属，
 * **初始化时机完全没变**（仍是 ui_start → ui_display_init 里第一次调用时建），
 * 目的是让新增的 TF 卡驱动有一个正当的"总线已就绪"入口，而不是反过来依赖 ui。
 *
 * 幂等：重复调用返回 ESP_OK（不会像裸 spi_bus_initialize 那样返回
 * ESP_ERR_INVALID_STATE，再被 ESP_ERROR_CHECK 直接 abort）。
 *
 * ⚠️ 顺序要求：先有总线，才能挂 SD —— esp_vfs_fat_sdspi_mount() 内部的
 *    sdspi_host_init_device() 要求总线已初始化（见 IDF 的 driver/sdspi_host.h）。
 *    开机第 3 步 ui_start() 已经建好总线，而 SD 是用户触发、远在其后，所以天然满足。 */
esp_err_t board_spi_bus_init(void);

/* SPI 总线的单次传输上限（字节）。取值 = 屏幕的 DMA 局部缓冲 160×16×2 = 5120，
 * 与搬到 board.c 之前【完全一致】—— 不要为了 TF 卡调大：SD 一次最多一个
 * 512B 扇区，5120 已经远超需要。 */
#define BOARD_SPI_MAX_TRANSFER  5120

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
