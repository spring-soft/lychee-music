/*
 * qqmusic —— QQ 音乐契约层客户端（实现）
 *
 * 服务端在 F:\wsl\qqmusic（Docker），契约见那边的 README。
 * 本文件只干两件事：HTTP 拿 JSON、把 JSON 映射成 sub_song_t。
 */
#include <string.h>
#include <inttypes.h>
#include "qqmusic.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "cJSON.h"

static const char *TAG = "qqmusic";

#define QQ_NVS_NS       "qqcfg"
#define HTTP_BODY_MAX   (96 * 1024)     /* 列表 JSON 也就几十 KB，给足余量 */

/* Kconfig 没定义时的兜底（值就是本机跑服务的地址） */
#ifndef CONFIG_APP_QQ_HOST
#define CONFIG_APP_QQ_HOST "http://192.168.0.108:3210"
#endif

static char s_host[QQ_HOST_MAX];

/* ============================================================ 模式表
 * 顺序 = 屏上「模式」二级菜单的显示顺序。
 * 服务端 /v1/modes 返回的是同一份清单（slug 要一致）。 */
static const struct { const char *name; const char *slug; } k_modes[QQ_MODE_COUNT] = {
    [QQ_MODE_FAV]     = { "收藏",     "fav"     },
    [QQ_MODE_DAILY]   = { "每日30首", "daily"   },
    [QQ_MODE_MILLION] = { "百万收藏", "million" },
    [QQ_MODE_NEWREC]  = { "新歌推荐", "newrec"  },
    [QQ_MODE_GUESS]   = { "刷歌",     "guess"   },
    [QQ_MODE_RADAR]   = { "雷达",     "radar"   },
};

const char *qq_mode_name(qq_mode_t m) { return (m < QQ_MODE_COUNT) ? k_modes[m].name : ""; }
const char *qq_mode_slug(qq_mode_t m) { return (m < QQ_MODE_COUNT) ? k_modes[m].slug : ""; }

/* ============================================================ 配置 */

static bool nvs_get_str_or(const char *key, char *dst, size_t len, const char *fallback)
{
    nvs_handle_t h;
    bool ok = false;
    if (nvs_open(QQ_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t n = len;
        if (nvs_get_str(h, key, dst, &n) == ESP_OK && dst[0]) ok = true;
        nvs_close(h);
    }
    if (!ok) {
        /* 没有存过 → 用 Kconfig 默认值。注意存的是**没有结尾斜杠**的地址 */
        strlcpy(dst, fallback ? fallback : "", len);
    }
    return ok;
}

esp_err_t qqmusic_init(void)
{
    bool from_nvs = nvs_get_str_or("host", s_host, sizeof(s_host), CONFIG_APP_QQ_HOST);
    /* 去掉结尾斜杠，免得拼出 //v1/list */
    size_t n = strlen(s_host);
    while (n > 0 && s_host[n - 1] == '/') s_host[--n] = 0;
    ESP_LOGI(TAG, "配置来源: %s  host=%s", from_nvs ? "NVS" : "Kconfig", s_host);
    return s_host[0] ? ESP_OK : ESP_ERR_INVALID_STATE;
}

bool qqmusic_configured(void) { return s_host[0] != 0; }
const char *qqmusic_host(void) { return s_host; }

esp_err_t qqmusic_set_config(const char *host, bool save)
{
    if (host == NULL || host[0] == 0) return ESP_ERR_INVALID_ARG;
    strlcpy(s_host, host, sizeof(s_host));
    size_t n = strlen(s_host);
    while (n > 0 && s_host[n - 1] == '/') s_host[--n] = 0;

    if (save) {
        nvs_handle_t h;
        esp_err_t err = nvs_open(QQ_NVS_NS, NVS_READWRITE, &h);
        if (err != ESP_OK) return err;
        err = nvs_set_str(h, "host", s_host);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
        if (err != ESP_OK) return err;
    }
    ESP_LOGW(TAG, "服务地址已设为 %s", s_host);
    return ESP_OK;
}

/* ============================================================ HTTP */

/* GET 一个 URL，把 body 拿回（PSRAM，调用方负责 free）。
 * 写法照抄 subsonic.c 的 http_get_body_ex —— 尤其是"Content-Length 可能缺失
 * （chunked）"那条坑：不能按固定大小读，否则 JSON 会被截断、cJSON_Parse 失败。 */
static esp_err_t http_get(const char *url, char **body_out, size_t *len_out,
                          int *status_out, int timeout_ms)
{
    *body_out = NULL;
    if (len_out) *len_out = 0;
    if (status_out) *status_out = 0;

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = timeout_ms,
        .buffer_size = 4096,
        .keep_alive_enable = true,      /* 元数据请求频繁，复用连接 */
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return ESP_FAIL;

    esp_err_t err = esp_http_client_open(cli, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "连接失败 %s: %s", esp_err_to_name(err), url);
        esp_http_client_cleanup(cli);
        return err;
    }
    int content_len = esp_http_client_fetch_headers(cli);
    int status = esp_http_client_get_status_code(cli);
    if (status_out) *status_out = status;

    size_t cap = (content_len > 0 && (size_t)content_len < HTTP_BODY_MAX) ? (size_t)content_len + 1 : 8192;
    char *body = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!body) {
        esp_http_client_close(cli);
        esp_http_client_cleanup(cli);
        return ESP_ERR_NO_MEM;
    }
    size_t used = 0;
    while (true) {
        if (used + 1 >= cap) {
            if (cap >= HTTP_BODY_MAX) {
                ESP_LOGW(TAG, "响应体到上限 %u KB，可能被截断", (unsigned)(HTTP_BODY_MAX / 1024));
                break;
            }
            size_t ncap = cap * 2;
            if (ncap > HTTP_BODY_MAX) ncap = HTTP_BODY_MAX;
            char *nb = heap_caps_malloc(ncap, MALLOC_CAP_SPIRAM);
            if (!nb) break;
            memcpy(nb, body, used);
            heap_caps_free(body);
            body = nb;
            cap = ncap;
        }
        int n = esp_http_client_read(cli, body + used, cap - 1 - used);
        if (n <= 0) break;
        used += (size_t)n;
    }
    body[used] = 0;
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);

    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "HTTP %d: %.200s", status, body);
        heap_caps_free(body);
        return ESP_ERR_INVALID_RESPONSE;
    }
    *body_out = body;
    if (len_out) *len_out = used;
    return ESP_OK;
}

/* URL 百分号编码（中文/空格）。写法与 subsonic_search 里那段一致 */
static void url_encode(const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    for (const char *p = in; *p && o + 4 < out_len; p++) {
        unsigned char c = (unsigned char)*p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
            || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            o += snprintf(out + o, out_len - o, "%%%02X", c);
        }
    }
    out[o] = 0;
}

/* ============================================================ JSON -> sub_song_t */

static void copy_str(char *dst, size_t len, const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsString(v) && v->valuestring) {
        strlcpy(dst, v->valuestring, len);
    } else {
        dst[0] = 0;
    }
}

static uint32_t get_u32(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsNumber(v) && v->valuedouble > 0) return (uint32_t)v->valuedouble;
    return 0;
}

/* 把服务端的 items[i] 映射成设备通用的 sub_song_t。
 * 字段是**对着这个结构剪的**（服务端 README 里有说明）。 */
static void json_to_song(const cJSON *it, sub_song_t *s)
{
    memset(s, 0, sizeof(*s));
    copy_str(s->id,     sizeof(s->id),     it, "id");      /* QQ 的 songmid，16 字符 */
    copy_str(s->title,  sizeof(s->title),  it, "title");
    copy_str(s->artist, sizeof(s->artist), it, "artist");
    copy_str(s->album,  sizeof(s->album),  it, "album");
    copy_str(s->cover,  sizeof(s->cover),  it, "cover");   /* 专辑 mid，拼封面直链用 */
    copy_str(s->suffix, sizeof(s->suffix), it, "suffix");  /* flac / mp3 */
    s->duration_s = get_u32(it, "duration");
    s->size       = get_u32(it, "size");
    s->starred    = false;      /* 后端标记在阶段 2 加 */
}

/* 解析 {"items":[...]} / {"songs":[...]} 这种顶层数组 */
static esp_err_t parse_items(cJSON *root, sub_song_t *out, int max, int *got)
{
    *got = 0;
    if (!cJSON_IsObject(root)) return ESP_ERR_INVALID_RESPONSE;
    const cJSON *arr = cJSON_GetObjectItem(root, "items");
    if (!cJSON_IsArray(arr)) arr = cJSON_GetObjectItem(root, "songs");
    if (!cJSON_IsArray(arr)) {
        ESP_LOGW(TAG, "响应里找不到 items/songs 数组");
        return ESP_ERR_INVALID_RESPONSE;
    }
    int i = 0;
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        if (i >= max) break;
        if (!cJSON_IsObject(it)) continue;
        json_to_song(it, &out[i]);
        if (out[i].id[0] == 0) continue;      /* 没 id 的条目没法播，跳过 */
        i++;
    }
    *got = i;
    return ESP_OK;
}

/* ============================================================ 公开接口 */

esp_err_t qqmusic_health(char *msg, size_t len)
{
    if (msg && len) msg[0] = 0;
    if (!qqmusic_configured()) {
        if (msg && len) strlcpy(msg, "没配服务地址", len);
        return ESP_ERR_INVALID_STATE;
    }
    char url[256];
    snprintf(url, sizeof(url), "%s/v1/health", s_host);

    char *body = NULL;
    int status = 0;
    esp_err_t err = http_get(url, &body, NULL, &status, 8000);
    if (err != ESP_OK) {
        if (msg && len) snprintf(msg, len, "连不上服务（HTTP %d）", status);
        return err;
    }
    cJSON *root = cJSON_Parse(body);
    heap_caps_free(body);
    if (!root) {
        if (msg && len) strlcpy(msg, "响应不是合法 JSON", len);
        return ESP_ERR_INVALID_RESPONSE;
    }

    esp_err_t ret = ESP_OK;
    const cJSON *ok = cJSON_GetObjectItem(root, "ok");
    const cJSON *cred = cJSON_GetObjectItem(root, "credential");
    bool okv = cJSON_IsTrue(ok);
    const char *reason = NULL;
    if (cJSON_IsObject(cred)) {
        const cJSON *r = cJSON_GetObjectItem(cred, "reason");
        if (cJSON_IsString(r)) reason = r->valuestring;
    }
    if (okv) {
        if (msg && len) {
            /* 把 VIP 是否可见也带出来 —— 它决定能不能拿无损 */
            const cJSON *vip = cJSON_GetObjectItem(root, "vip");
            snprintf(msg, len, "凭据有效%s", cJSON_IsObject(vip) ? "，VIP 已识别" : "");
        }
    } else {
        ret = ESP_ERR_INVALID_STATE;
        if (msg && len) snprintf(msg, len, "%s", reason ? reason : "服务报不 ok（多半是凭据过期）");
    }
    cJSON_Delete(root);
    return ret;
}

esp_err_t qqmusic_list(qq_mode_t mode, sub_song_t *out, int max, int *got)
{
    *got = 0;
    if (!qqmusic_configured()) return ESP_ERR_INVALID_STATE;
    if (mode >= QQ_MODE_COUNT) return ESP_ERR_INVALID_ARG;
    if (max > QQ_SONGS_MAX) max = QQ_SONGS_MAX;

    char url[256];
    snprintf(url, sizeof(url), "%s/v1/list?mode=%s&limit=%d",
             s_host, qq_mode_slug(mode), max);

    char *body = NULL;
    esp_err_t err = http_get(url, &body, NULL, NULL, 20000);   /* 雷达/刷歌上游偶发慢 */
    if (err != ESP_OK) return err;
    cJSON *root = cJSON_Parse(body);
    heap_caps_free(body);
    if (!root) return ESP_ERR_INVALID_RESPONSE;
    err = parse_items(root, out, max, got);
    cJSON_Delete(root);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "模式「%s」拉到 %d 首", qq_mode_name(mode), *got);
    }
    return err;
}

esp_err_t qqmusic_search(const char *query, sub_song_t *out, int max, int *got)
{
    *got = 0;
    if (!qqmusic_configured() || query == NULL || query[0] == 0) return ESP_ERR_INVALID_ARG;
    if (max > QQ_SONGS_MAX) max = QQ_SONGS_MAX;

    char enc[256];
    url_encode(query, enc, sizeof(enc));

    char url[512];
    snprintf(url, sizeof(url), "%s/v1/search?q=%s&limit=%d", s_host, enc, max);

    char *body = NULL;
    esp_err_t err = http_get(url, &body, NULL, NULL, 15000);
    if (err != ESP_OK) return err;
    cJSON *root = cJSON_Parse(body);
    heap_caps_free(body);
    if (!root) return ESP_ERR_INVALID_RESPONSE;
    err = parse_items(root, out, max, got);
    cJSON_Delete(root);
    return err;
}

esp_err_t qqmusic_stream_url(const sub_song_t *song, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) return ESP_ERR_INVALID_ARG;
    out[0] = 0;
    if (!qqmusic_configured() || song == NULL || song->id[0] == 0) return ESP_ERR_INVALID_ARG;

    char enc[128];
    url_encode(song->id, enc, sizeof(enc));

    char url[320];
    snprintf(url, sizeof(url), "%s/v1/url?id=%s", s_host, enc);

    char *body = NULL;
    int status = 0;
    esp_err_t err = http_get(url, &body, NULL, &status, 12000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "取播放地址失败（HTTP %d，歌曲 %.24s）", status, song->title);
        return err;
    }
    cJSON *root = cJSON_Parse(body);
    heap_caps_free(body);
    if (!root) return ESP_ERR_INVALID_RESPONSE;

    err = ESP_ERR_INVALID_RESPONSE;
    const cJSON *u = cJSON_GetObjectItem(root, "url");
    if (cJSON_IsString(u) && u->valuestring && u->valuestring[0]) {
        /* ⚠️ 直链约 250~260 字符，调用方必须给够（player.c 里是 url[768]） */
        if (strlen(u->valuestring) + 1 > out_len) {
            ESP_LOGE(TAG, "直链太长（%u 字节 > %u），调用方缓冲不够",
                     (unsigned)strlen(u->valuestring) + 1, (unsigned)out_len);
        } else {
            strlcpy(out, u->valuestring, out_len);
            const cJSON *q = cJSON_GetObjectItem(root, "quality");
            ESP_LOGI(TAG, "拿到直链（%s，%u 字节）",
                     cJSON_IsString(q) ? q->valuestring : "?", (unsigned)strlen(out));
            err = ESP_OK;
        }
    } else {
        /* 服务端会在 detail 里说清原因（无权限/凭据过期…） */
        const cJSON *d = cJSON_GetObjectItem(root, "detail");
        ESP_LOGW(TAG, "服务端没给 url: %s", cJSON_IsString(d) ? d->valuestring : "(无 detail)");
    }
    cJSON_Delete(root);
    return err;
}

esp_err_t qqmusic_lyrics(const char *song_id, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) return ESP_ERR_INVALID_ARG;
    out[0] = 0;
    if (!qqmusic_configured() || song_id == NULL || song_id[0] == 0) return ESP_ERR_INVALID_ARG;

    char enc[128];
    url_encode(song_id, enc, sizeof(enc));

    char url[320];
    snprintf(url, sizeof(url), "%s/v1/lyric?id=%s", s_host, enc);

    char *body = NULL;
    size_t n = 0;
    esp_err_t err = http_get(url, &body, &n, NULL, 12000);
    if (err != ESP_OK) return err;

    /* 服务端返回的是 LRC 纯文本（歌词页本来就吃纯文本）。
     * ⚠️ 拿不到歌词时它会返回**空串**（200），这不是错误 —— 歌词页空白即可，别当失败。 */
    if (n == 0) {
        heap_caps_free(body);
        return ESP_ERR_NOT_FOUND;
    }
    strlcpy(out, body, out_len);
    heap_caps_free(body);
    return ESP_OK;
}

esp_err_t qqmusic_cover_url(const char *album_mid, int size, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) return ESP_ERR_INVALID_ARG;
    out[0] = 0;
    if (album_mid == NULL || album_mid[0] == 0) return ESP_ERR_INVALID_ARG;

    /* ⚠️ QQ 的 photo_new 只认这几个尺寸，其它一律 404。设备屏 160×128，固定 300 最省事。 */
    static const int k_ok[] = { 90, 120, 150, 300, 500, 800 };
    int use = 300;
    for (size_t i = 0; i < sizeof(k_ok) / sizeof(k_ok[0]); i++) {
        if (k_ok[i] >= size) { use = k_ok[i]; break; }
    }
    snprintf(out, out_len, "http://y.qq.com/music/photo_new/T002R%dx%dM000%s.jpg",
             use, use, album_mid);
    return ESP_OK;
}
