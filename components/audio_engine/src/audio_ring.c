#include <string.h>
#include "audio_ring.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "ring";

esp_err_t audio_ring_init(audio_ring_t *r, size_t cap_bytes, const char *name)
{
    memset(r, 0, sizeof(*r));
    r->name = name;
    r->cap  = cap_bytes;
    /* 大块缓冲一律 PSRAM：内部 SRAM 只有 ~340KB，要留给协议栈/DMA/任务栈 */
    r->buf = heap_caps_malloc(cap_bytes, MALLOC_CAP_SPIRAM);
    if (r->buf == NULL) {
        ESP_LOGE(TAG, "[%s] PSRAM 分配 %u KB 失败", name, (unsigned)(cap_bytes / 1024));
        return ESP_ERR_NO_MEM;
    }
    r->ev = xSemaphoreCreateBinary();
    if (r->ev == NULL) {
        heap_caps_free(r->buf);
        r->buf = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "[%s] 环形缓冲 %u KB @ %p (PSRAM)", name,
             (unsigned)(cap_bytes / 1024), r->buf);
    return ESP_OK;
}

void audio_ring_deinit(audio_ring_t *r)
{
    if (r->ev)  vSemaphoreDelete(r->ev);
    if (r->buf) heap_caps_free(r->buf);
    memset(r, 0, sizeof(*r));
}

void audio_ring_reset(audio_ring_t *r)
{
    r->rd = r->wr = 0;
    r->max_used = 0;
    while (xSemaphoreTake(r->ev, 0) == pdTRUE) { /* 清空信号量 */ }
}

size_t audio_ring_write_span(audio_ring_t *r, uint8_t **ptr, TickType_t timeout)
{
    /* 满则等（带上限，避免卡死） */
    if (audio_ring_space(r) == 0) {
        xSemaphoreTake(r->ev, timeout);
    }
    size_t free_bytes = audio_ring_space(r);
    if (free_bytes == 0) {
        *ptr = NULL;
        return 0;
    }
    size_t w = r->wr % r->cap;
    size_t contiguous = r->cap - w;
    size_t n = free_bytes < contiguous ? free_bytes : contiguous;
    *ptr = r->buf + w;
    return n;
}

size_t audio_ring_read_span(audio_ring_t *r, const uint8_t **ptr, TickType_t timeout)
{
    if (audio_ring_used(r) == 0) {
        xSemaphoreTake(r->ev, timeout);
    }
    size_t used = audio_ring_used(r);
    if (used == 0) {
        *ptr = NULL;
        return 0;
    }
    size_t rd = r->rd % r->cap;
    size_t contiguous = r->cap - rd;
    size_t n = used < contiguous ? used : contiguous;
    *ptr = r->buf + rd;
    return n;
}

void audio_ring_write_commit(audio_ring_t *r, size_t n)
{
    if (n == 0) return;
    r->wr += n;
    size_t used = audio_ring_used(r);
    if (used > r->max_used) r->max_used = used;
    xSemaphoreGive(r->ev);          /* 唤醒消费者 */
}

void audio_ring_read_commit(audio_ring_t *r, size_t n)
{
    if (n == 0) return;
    r->rd += n;
    xSemaphoreGive(r->ev);          /* 唤醒生产者（有新空间） */
}

void audio_ring_defrag(audio_ring_t *r)
{
    size_t used = audio_ring_used(r);
    if (used == 0) return;
    size_t rd = r->rd % r->cap;
    if (rd + used <= r->cap) {
        return;                     /* 已经连续，无需搬 */
    }
    /* 尾部（从 rd 到缓冲区末尾）搬到头部之后，数据变连续 */
    size_t tail = r->cap - rd;
    memmove(r->buf, r->buf + rd, tail);
    size_t head_used = used - tail;
    memmove(r->buf + tail, r->buf, head_used);
    r->rd = 0;
    r->wr = used;
}

size_t audio_ring_write(audio_ring_t *r, const void *data, size_t len, TickType_t timeout)
{
    const uint8_t *src = (const uint8_t *)data;
    size_t done = 0;
    while (done < len) {
        uint8_t *dst = NULL;
        size_t n = audio_ring_write_span(r, &dst, timeout);
        if (n == 0) break;
        if (n > len - done) n = len - done;
        memcpy(dst, src + done, n);
        audio_ring_write_commit(r, n);
        done += n;
    }
    return done;
}
