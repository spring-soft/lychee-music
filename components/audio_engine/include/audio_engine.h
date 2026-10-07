/*
 * ESP-Music 音频引擎
 *
 *   源(内存/HTTP) --fetch--> ring_in(128KB,PSRAM) --decode--> ring_pcm(128KB,PSRAM) --out--> I2S/ES8388
 *
 * 三个任务各司其职，靠环形缓冲的阻塞天然形成背压：
 *   - fetch : 低优先级、core 0（M2 里 HTTP/TLS 也在这，别跟 UI 抢 core 1）
 *   - decode: core 1，每帧 vTaskDelay(1) 把 UI 的最坏延迟钉在 1 tick
 *   - out   : core 1 最高优先级，写 I2S（阻塞写天然按音频时钟节流）
 *
 * 真实播放位置 = 已交给 DMA 的 PCM 帧数 - DMA 深度（不是墙钟，不会漂移）
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AE_STATE_IDLE = 0,
    AE_STATE_BUFFERING,
    AE_STATE_PLAYING,
    AE_STATE_PAUSED,
    AE_STATE_FINISHED,
    AE_STATE_ERROR,
} ae_state_t;

/* 音频源抽象：M1 用内存，M2 换成 HTTP 流，离线模式再换成本地文件，引擎其余部分不变 */
typedef struct ae_source {
    esp_err_t (*open)(struct ae_source *s, const char *uri);   /* uri 可为 NULL = 用创建时的 */
    /* 返回读到的字节数；0 = 流结束(EOF)；<0 = 错误 */
    int       (*read)(struct ae_source *s, uint8_t *buf, size_t want);
    void      (*close)(struct ae_source *s);
    /* 释放整个源（ctx + uri + 自身）。可选，但每个新源都该实现。
     * 为什么要它：player 换歌时要释放"上一首的源"，而源的类型是运行时才知道的
     * （在线=HTTP、离线=本地文件）—— 以前 player 直接调 ae_source_http_free()，
     * 加了文件源之后那样写就会用错释放函数。 */
    void      (*free)(struct ae_source *s);
    void       *ctx;
    const char *uri;      /* 创建时带的 URL/标识（HTTP/文件源用） */
} ae_source_t;

/* 通用释放：转调 s->free（没有就兜底放掉 ctx/uri/自身）。任何源都能用它。 */
void       ae_source_free(ae_source_t *s);

esp_err_t  audio_engine_init(void);
esp_err_t  audio_engine_play(ae_source_t *src);
void       audio_engine_stop(void);
void       audio_engine_pause(bool pause);

ae_state_t audio_engine_state(void);
const char *audio_engine_state_str(void);
const char *audio_engine_format_str(void);
/* 暂停是独立标志（状态机里不出 PAUSED：暂停时仍在往 I2S 写静音，位置冻结） */
bool       audio_engine_is_paused(void);

/* 真实播放位置（ms）。暂停时冻结 */
uint32_t   audio_engine_position_ms(void);
/* seek 用：告诉引擎"这一首是从第 ms 毫秒开始发的流"。⚠️ 必须在 audio_engine_play()
 * 之后调（play 内部会把基准清 0） */
void       audio_engine_set_pos_base(uint32_t ms);
/* 已解码出的总时长（ms）—— 进度/时长参考（M2 起用 Subsonic 的 duration 更准） */
uint32_t   audio_engine_decoded_ms(void);
uint32_t   audio_engine_sample_rate(void);
/* 解码器报出来的【源文件】位深（播放页显示用）。
 * ⚠️ 注意这不是"到达 ES8388 的位深" —— ae_normalize() 会把 24/32bit 截成 16bit
 *    再送进 I2S（I2S 槽也是写死的 16bit）。显示源位深是为了让用户看清【文件的质量】，
 *    24 说明这是高解析文件，不代表芯片真的按 24bit 在放。 */
uint8_t    audio_engine_bit_depth(void);
int        audio_engine_underruns(void);
/* 诊断：网络取流 / 解码消耗的累计字节数（配合时间算速率，见 .c 里的 rate_log） */
uint64_t   audio_engine_fetch_bytes(void);
uint64_t   audio_engine_decode_bytes(void);
size_t     audio_engine_ring_used_in(void);
/* ring_in 的总容量（字节）。屏幕/网页要显示"缓冲占用百分比"就得知道分母 ——
 * 别在 UI 侧硬编码 512KB，那是 audio_engine.c 里的 RING_IN_BYTES，改了会不一致。 */
size_t     audio_engine_ring_in_cap(void);
/* 解码后的 PCM 环（ring_pcm）—— 和 ring_in 一样，UI 侧别硬编码 */
size_t     audio_engine_ring_pcm_used(void);
size_t     audio_engine_ring_pcm_cap(void);

/* ===== 「听歌缓存」的已用量 / 总量 =====
 * 这是给界面显示用的一个整体指标：把所有为播放数据预留的缓冲加在一起，
 * 看眼下装了多少。
 *   分子 = ring_in(压缩流) + ring_pcm(解码后 PCM) 里实际存着的数据
 *   分母 = ring_in + ring_pcm + 解码输出暂存 + 解码暂存 的总容量
 * 分母为什么把"暂存"也算进去：那些内存同样是【为听歌预留的】，
 * 只算两个环的话分子分母会一直贴着 100%（网络跟得上时两个环都是满的），
 * 那个数字就完全没有信息量了。 */
size_t     audio_engine_audio_cache_used(void);
size_t     audio_engine_audio_cache_cap(void);
size_t     audio_engine_ring_used_pcm(void);
size_t     audio_engine_ring_in_high_water(void);
bool       audio_engine_is_error(void);

/* ---------------- 内存源（M1 验证用，也可用于本地小文件） ---------------- */
ae_source_t *ae_source_memory_new(const uint8_t *data, size_t len);
void         ae_source_memory_free(ae_source_t *s);

/* ---------------- HTTP 流式源（M2 起） ---------------- */
ae_source_t *ae_source_http_new(const char *url);
void         ae_source_http_free(ae_source_t *s);

/* ---------------- 本地文件源（离线模式） ----------------
 * path 是 VFS 路径，如 "/sdcard/音乐/周杰伦 - 稻香.flac"。
 * 读盘是阻塞的，只应在 audio_engine 的 fetch 任务（core 0、低优先级）里被调用。
 * ⚠️ 不支持 seek（见 audio_source_file.c 顶部说明）。 */
ae_source_t *ae_source_file_new(const char *path);
void         ae_source_file_free(ae_source_t *s);

/* 内嵌测试音频（⚠️ M1 专用，M2 接上 Navidrome 后连同 audio/ 目录一起删掉） */
const uint8_t *ae_embed_test_audio(bool flac, size_t *len);

#ifdef __cplusplus
}
#endif
