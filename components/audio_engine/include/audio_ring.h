/*
 * 单生产者-单消费者环形缓冲（数据在 PSRAM）
 *
 * 设计要点：
 *  - rd/wr 是单调递增的绝对计数（不回绕比较），used = wr - rd
 *  - 只有一个二进制信号量做"有新数据/有新空间"的唤醒；不追求精确计数，
 *    因为每次唤醒后都会重新计算真实的 used/free（简单且不会计数错）
 *  - 所有等待都带超时，以便切歌/停止时能立刻退出（配合外部 generation）
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

typedef struct {
    uint8_t          *buf;
    size_t            cap;
    volatile size_t   rd;
    volatile size_t   wr;
    SemaphoreHandle_t ev;        /* 二进制信号量：仅作唤醒 */
    size_t            max_used;  /* 诊断：水位高 */
    const char       *name;
} audio_ring_t;

esp_err_t audio_ring_init(audio_ring_t *r, size_t cap_bytes, const char *name);
void      audio_ring_deinit(audio_ring_t *r);
void      audio_ring_reset(audio_ring_t *r);

static inline size_t audio_ring_used(const audio_ring_t *r) { return r->wr - r->rd; }
static inline size_t audio_ring_space(const audio_ring_t *r) { return r->cap - (r->wr - r->rd); }

/* 写入：阻塞直到写完或超时。返回实际写入字节数（可能 < len） */
size_t audio_ring_write(audio_ring_t *r, const void *data, size_t len, TickType_t timeout);

/* 零拷贝写：拿到一段连续空闲区。返回可写长度（0 = 超时/满） */
size_t audio_ring_write_span(audio_ring_t *r, uint8_t **ptr, TickType_t timeout);
/* 零拷贝读：拿到一段连续已填数据。返回可读长度（0 = 超时/空） */
size_t audio_ring_read_span(audio_ring_t *r, const uint8_t **ptr, TickType_t timeout);
void   audio_ring_write_commit(audio_ring_t *r, size_t n);
void   audio_ring_read_commit(audio_ring_t *r, size_t n);

/* 数据跨越回绕点时，把尾部小块搬到头部，让可读数据变成一整段连续区。
 * 解码器只需要"足够长的连续输入"，这个函数保证不会因为回绕把一帧劈成两半。 */
void   audio_ring_defrag(audio_ring_t *r);

#ifdef __cplusplus
}
#endif
