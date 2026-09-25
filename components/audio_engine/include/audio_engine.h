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

/* 音频源抽象：M1 用内存，M2 换成 HTTP 流，引擎其余部分不变 */
typedef struct ae_source {
    esp_err_t (*open)(struct ae_source *s, const char *uri);   /* uri 可为 NULL = 用创建时的 */
    /* 返回读到的字节数；0 = 流结束(EOF)；<0 = 错误 */
    int       (*read)(struct ae_source *s, uint8_t *buf, size_t want);
    void      (*close)(struct ae_source *s);
    void       *ctx;
    const char *uri;      /* 创建时带的 URL/标识（HTTP 源用） */
} ae_source_t;

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
int        audio_engine_underruns(void);
size_t     audio_engine_ring_used_in(void);
size_t     audio_engine_ring_used_pcm(void);
size_t     audio_engine_ring_in_high_water(void);
bool       audio_engine_is_error(void);

/* ---------------- 内存源（M1 验证用，也可用于本地小文件） ---------------- */
ae_source_t *ae_source_memory_new(const uint8_t *data, size_t len);
void         ae_source_memory_free(ae_source_t *s);

/* ---------------- HTTP 流式源（M2 起） ---------------- */
ae_source_t *ae_source_http_new(const char *url);
void         ae_source_http_free(ae_source_t *s);

/* 内嵌测试音频（⚠️ M1 专用，M2 接上 Navidrome 后连同 audio/ 目录一起删掉） */
const uint8_t *ae_embed_test_audio(bool flac, size_t *len);

#ifdef __cplusplus
}
#endif
