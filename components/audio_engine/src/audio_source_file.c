/*
 * 本地文件音频源（离线模式用）
 *
 * 从 VFS 路径（如 /sdcard/音乐/周杰伦 - 稻香.flac）流式读，喂给同一个 audio_engine。
 * 与 HTTP 源的区别只有"数据从哪来" —— engine 的 fetch/decode/out 三个任务、
 * 环形缓冲、欠载统计、位置推算全都不用改。ae_source_t 这张 vtable 当年就是为这种
 * 替换留的（见 audio_engine.h 的注释：M1 用内存、M2 换 HTTP、离线再换文件）。
 *
 * ⚠️ 为什么不做 seek：ae_source_t 只有 open/read/close，没有 seek；而且压缩流也没法
 *    把"毫秒"直接换算成文件偏移（不解码就不知道第 90 秒在第几个字节）。
 *    所以 player 在离线模式下把 seek 做成"从头重播"（见 player.c 的 player_seek）。
 *    真要做，正确做法是 decode-and-discard（解出来丢掉直到目标位置），列为后续项。
 *
 * ⚠️ 读盘是阻塞的（SPI2 上和屏幕共用总线），所以只应该在 audio_engine 的 fetch 任务
 *    （core 0、低优先级）里被调用 —— 引擎本来就是这么用的。
 */
#include <stdio.h>
#include <string.h>
#include "audio_engine.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "file_src";

typedef struct {
    FILE   *fp;
    size_t  total_read;
    bool    logged_end;
} file_ctx_t;

static esp_err_t file_open(ae_source_t *s, const char *uri)
{
    file_ctx_t *c = s->ctx;
    if (c == NULL) return ESP_ERR_INVALID_STATE;

    if (c->fp) { fclose(c->fp); c->fp = NULL; }     /* 复用同一个源时先关旧的 */
    c->total_read = 0;
    c->logged_end = false;

    const char *path = uri ? uri : s->uri;          /* uri 为空 = 用构造时给的 */
    if (path == NULL || path[0] == 0) {
        ESP_LOGE(TAG, "没有路径");
        return ESP_ERR_INVALID_ARG;
    }

    c->fp = fopen(path, "rb");
    if (c->fp == NULL) {
        /* 最常见的原因：卡被拔了 / 文件被删了 / 这条 path 是上次扫描留下的。
         * 上层（player）会把它当作"这首歌播不了"，跳到下一首。 */
        ESP_LOGE(TAG, "打不开文件: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    /* 报一下文件大小，方便和"实际读了多少"对账（读得少 = 卡出问题/文件被换）。
     * 取不到（返回 0 或 -1）不视为错误，照样往下放。 */
    long sz = -1;
    if (fseek(c->fp, 0, SEEK_END) == 0) {
        sz = ftell(c->fp);
        fseek(c->fp, 0, SEEK_SET);
    }
    ESP_LOGI(TAG, "打开本地文件: %s（%ld 字节）", path, sz);
    return ESP_OK;
}

static int file_read(ae_source_t *s, uint8_t *buf, size_t want)
{
    file_ctx_t *c = s->ctx;
    if (c == NULL || c->fp == NULL) return -1;

    size_t n = fread(buf, 1, want, c->fp);
    if (n > 0) {
        c->total_read += n;
        return (int)n;
    }
    /* fread 返回 0：要么 EOF，要么真出错 —— 必须分开，
     * 出错误当 EOF 会变成"这首歌静默地短了一截"。 */
    if (ferror(c->fp)) {
        ESP_LOGE(TAG, "读文件出错，已读 %u KB", (unsigned)(c->total_read / 1024));
        return -1;
    }
    if (!c->logged_end) {
        c->logged_end = true;
        ESP_LOGI(TAG, "文件结束：共读 %u KB", (unsigned)(c->total_read / 1024));
    }
    return 0;                                       /* 0 = EOF（引擎的约定） */
}

static void file_close(ae_source_t *s)
{
    file_ctx_t *c = s->ctx;
    if (c && c->fp) { fclose(c->fp); c->fp = NULL; }
}

static void file_free(ae_source_t *s)
{
    if (s == NULL) return;
    if (s->ctx) {
        file_ctx_t *c = s->ctx;
        if (c->fp) fclose(c->fp);
        heap_caps_free(c);
    }
    if (s->uri) heap_caps_free((void *)s->uri);
    heap_caps_free(s);
}

ae_source_t *ae_source_file_new(const char *path)
{
    ae_source_t *s = heap_caps_calloc(1, sizeof(ae_source_t), MALLOC_CAP_SPIRAM);
    file_ctx_t  *c = heap_caps_calloc(1, sizeof(file_ctx_t), MALLOC_CAP_SPIRAM);
    char *path_copy = NULL;

    if (s && c) {
        if (path) path_copy = heap_caps_malloc(strlen(path) + 1, MALLOC_CAP_SPIRAM);
        if (path == NULL || path_copy) {
            if (path_copy) strcpy(path_copy, path);
            s->ctx   = c;
            s->uri   = path_copy;
            s->open  = file_open;
            s->read  = file_read;
            s->close = file_close;
            s->free  = file_free;      /* 让 player 能用统一的 ae_source_free() */
            return s;
        }
    }
    if (path_copy) heap_caps_free(path_copy);
    if (s) heap_caps_free(s);
    if (c) heap_caps_free(c);
    return NULL;
}

void ae_source_file_free(ae_source_t *s) { file_free(s); }
