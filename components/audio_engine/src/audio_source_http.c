/*
 * HTTP 流式音频源（M2）
 *
 * 用 esp_http_client 直接流式读，不用 GMF 的 io_http：
 *   - 能把 HTTP 响应头（Content-Length / Accept-Ranges / Content-Type）打出来，
 *     这几项正是后面判断"服务端到底转码没转码"的唯一依据（seek 走 timeOffset 还是 Range）
 *   - 读多少给多少，不会在中间再插一层不知道行为的缓冲
 *
 * ⚠️ 认证只能走 URL query —— Subsonic 的 u/p/t/s 就是这么传的，正好合适。
 *    （esp_http_client 也能加自定义头，但 Navidrome 不需要）
 */
#include <string.h>
#include <inttypes.h>
#include "audio_engine.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"

static const char *TAG = "http_src";

typedef struct {
    esp_http_client_handle_t cli;
    int64_t  content_len;      /* -1 = chunked / 未知 */
    bool     accept_ranges;
    char     content_type[48];
    int      status;
    size_t   total_read;
} http_ctx_t;

static esp_err_t http_open(ae_source_t *s, const char *uri)
{
    http_ctx_t *c = s->ctx;
    if (c->cli) {                                  /* 复用 handle 时先清掉旧的 */
        esp_http_client_close(c->cli);
        esp_http_client_cleanup(c->cli);
        c->cli = NULL;
    }
    memset(&c->content_type, 0, sizeof(c->content_type));
    c->total_read = 0;
    c->accept_ranges = false;
    c->content_len = -1;

    const char *url = uri ? uri : s->uri;   /* uri 为空时用构造时给的 */
    if (url == NULL) {
        ESP_LOGE(TAG, "没有 URL");
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_config_t cfg = {
        .url        = url,
        .timeout_ms = 15000,
        /* 8KB 接收缓冲：小缓冲会让数据到达更"碎"，配合起播预取一起决定抗抖动能力 */
        .buffer_size = 8192,
        .buffer_size_tx = 1024,
        .keep_alive_enable = false,
    };
    c->cli = esp_http_client_init(&cfg);
    if (c->cli == NULL) {
        ESP_LOGE(TAG, "esp_http_client_init 失败");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(c->cli, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "连接失败: %s  (%s)", esp_err_to_name(err), url);
        esp_http_client_cleanup(c->cli);
        c->cli = NULL;
        return err;
    }

    c->content_len = esp_http_client_fetch_headers(c->cli);
    c->status = esp_http_client_get_status_code(c->cli);

    char *v = NULL;
    if (esp_http_client_get_header(c->cli, "Accept-Ranges", &v) == ESP_OK && v) {
        c->accept_ranges = (strstr(v, "bytes") != NULL);
    }
    v = NULL;
    if (esp_http_client_get_header(c->cli, "Content-Type", &v) == ESP_OK && v) {
        strlcpy(c->content_type, v, sizeof(c->content_type));
    }
    int64_t hdr_len = esp_http_client_get_content_length(c->cli);

    ESP_LOGW(TAG, "HTTP %d  len=%" PRId64 " (hdr=%" PRId64 ", %s)  Accept-Ranges=%s  type=%s",
             c->status, c->content_len, hdr_len,
             c->content_len < 0 ? "chunked/未知" : "有长度",
             c->accept_ranges ? "yes" : "no",
             c->content_type[0] ? c->content_type : "-");
    ESP_LOGI(TAG, "URL: %s", url);

    if (c->status != 200 && c->status != 206) {
        ESP_LOGE(TAG, "HTTP 状态异常 %d（期望 200/206）", c->status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static int http_read(ae_source_t *s, uint8_t *buf, size_t want)
{
    http_ctx_t *c = s->ctx;
    if (c->cli == NULL) return -1;
    int n = esp_http_client_read(c->cli, (char *)buf, want);
    if (n > 0) {
        c->total_read += n;
    } else if (n == 0) {
        ESP_LOGI(TAG, "流结束：共读 %u KB", (unsigned)(c->total_read / 1024));
    } else {
        ESP_LOGE(TAG, "读取错误 (%d)，已读 %u KB", n, (unsigned)(c->total_read / 1024));
    }
    return n;
}

static void http_close(ae_source_t *s)
{
    http_ctx_t *c = s->ctx;
    if (c->cli) {
        esp_http_client_close(c->cli);
        esp_http_client_cleanup(c->cli);
        c->cli = NULL;
    }
}

ae_source_t *ae_source_http_new(const char *url)
{
    ae_source_t *s = heap_caps_calloc(1, sizeof(ae_source_t), MALLOC_CAP_SPIRAM);
    http_ctx_t  *c = heap_caps_calloc(1, sizeof(http_ctx_t), MALLOC_CAP_SPIRAM);
    char *url_copy = NULL;
    if (s && c) {
        if (url) {
            url_copy = heap_caps_malloc(strlen(url) + 1, MALLOC_CAP_SPIRAM);
        }
        if (url == NULL || url_copy) {
            if (url_copy) strcpy(url_copy, url);
            c->content_len = -1;
            s->ctx   = c;
            s->uri   = url_copy;
            s->open  = http_open;
            s->read  = http_read;
            s->close = http_close;
            return s;
        }
    }
    if (url_copy) heap_caps_free(url_copy);
    if (s) heap_caps_free(s);
    if (c) heap_caps_free(c);
    return NULL;
}

void ae_source_http_free(ae_source_t *s)
{
    if (!s) return;
    if (s->uri) heap_caps_free((void *)s->uri);
    if (s->ctx) heap_caps_free(s->ctx);
    heap_caps_free(s);
}
