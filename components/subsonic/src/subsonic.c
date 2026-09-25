#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "subsonic.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "mbedtls/md5.h"
#include "nvs.h"
#include "cJSON.h"
#include "esp_jpeg_dec.h"
#include "esp_jpeg_common.h"

static const char *TAG = "nav";

/* 定义在后面，前面的 subsonic_search 要用 */
static esp_err_t get_song_list(const char *url, const char *key, const char *sub,
                               const char *what, sub_song_t *out, int max, int *got);

#define NVS_NS_NAV      "nav"
#define SUB_API_VER     "1.16.1"
#define SUB_CLIENT_NAME "esp-music"      /* 别用 DSub/SubMusic */
#define HTTP_BODY_MAX   (96 * 1024)      /* 30 首歌的 JSON 约 10~20KB */

static char s_host[160];
static char s_user[48];
static char s_pass[64];
static int  s_max_kbps;
static char s_transcode[8];              /* "mp3" / "aac" / "raw" */

static bool nvs_get_str_or(const char *key, char *dst, size_t len, const char *fallback)
{
    nvs_handle_t h;
    dst[0] = 0;
    if (nvs_open(NVS_NS_NAV, NVS_READONLY, &h) == ESP_OK) {
        size_t l = len;
        if (nvs_get_str(h, key, dst, &l) == ESP_OK && dst[0]) {
            nvs_close(h);
            return true;
        }
        nvs_close(h);
    }
    strlcpy(dst, fallback ? fallback : "", len);
    return false;
}

esp_err_t subsonic_init(void)
{
    bool from_nvs = nvs_get_str_or("host", s_host, sizeof(s_host), CONFIG_APP_NAV_HOST);
    nvs_get_str_or("user", s_user, sizeof(s_user), CONFIG_APP_NAV_USER);
    nvs_get_str_or("pass", s_pass, sizeof(s_pass), CONFIG_APP_NAV_PASS);
    nvs_get_str_or("transcode", s_transcode, sizeof(s_transcode), CONFIG_APP_NAV_TRANSCODE);
    s_max_kbps = CONFIG_APP_NAV_MAX_KBPS;
    nvs_handle_t h;
    if (nvs_open(NVS_NS_NAV, NVS_READONLY, &h) == ESP_OK) {
        int32_t kb = 0;
        if (nvs_get_i32(h, "max_kbps", &kb) == ESP_OK && kb >= 32 && kb <= 320) {
            s_max_kbps = (int)kb;
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "配置来源: %s  host=%s user=%s 转码=%s %d kbps（密码%s）",
             from_nvs ? "NVS" : "Kconfig", s_host, s_user, s_transcode, s_max_kbps,
             s_pass[0] ? "已设" : "空");
    return s_host[0] ? ESP_OK : ESP_ERR_INVALID_STATE;
}

const char *subsonic_host(void) { return s_host; }
bool subsonic_configured(void) { return s_host[0] != 0; }

/* u/t/s 认证参数（salt 每次随机，t = md5(pass+salt) 小写 hex） */
static void append_auth_ex(char *url, size_t len, const char *user, const char *pass)
{
    char salt[16];
    snprintf(salt, sizeof(salt), "%08" PRIx32, esp_random());
    unsigned char digest[16];
    char md5hex[33];
    mbedtls_md5_context ctx;
    mbedtls_md5_init(&ctx);
    mbedtls_md5_starts(&ctx);
    mbedtls_md5_update(&ctx, (const unsigned char *)pass, strlen(pass));
    mbedtls_md5_update(&ctx, (const unsigned char *)salt, strlen(salt));
    mbedtls_md5_finish(&ctx, digest);
    mbedtls_md5_free(&ctx);
    for (int i = 0; i < 16; i++) snprintf(md5hex + i * 2, 3, "%02x", digest[i]);

    size_t cur = strlen(url);
    snprintf(url + cur, len - cur, "&u=%s&t=%s&s=%s&v=%s&c=%s",
             user, md5hex, salt, SUB_API_VER, SUB_CLIENT_NAME);
}

static void append_auth(char *url, size_t len) { append_auth_ex(url, len, s_user, s_pass); }

/* GET 一个 .view 接口，把 body 拿回来（PSRAM，调用方负责 free）
 * max_bytes > 0 时，超过就提前放弃（封面用，见下面对 WebP 的说明） */
static esp_err_t http_get_body_ex(const char *url, char **body_out, size_t *len_out,
                                  int *status_out, size_t max_bytes, int timeout_ms)
{
    *body_out = NULL;
    if (len_out) *len_out = 0;
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = timeout_ms,
        .buffer_size = 4096,
        .keep_alive_enable = true,     /* 元数据请求频繁，复用连接 */
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return ESP_FAIL;

    esp_err_t err = esp_http_client_open(cli, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "连接失败: %s", esp_err_to_name(err));
        esp_http_client_cleanup(cli);
        return err;
    }
    int content_len = esp_http_client_fetch_headers(cli);
    int status = esp_http_client_get_status_code(cli);
    if (status_out) *status_out = status;

    /* 有 Content-Length 且明显超限时，连读都不读 —— 省掉一次几万字节的下载 */
    if (max_bytes > 0 && content_len > 0 && (size_t)content_len > max_bytes) {
        ESP_LOGW(TAG, "响应体 %d 字节 > 上限 %u，放弃（URL 尾 %.60s）",
                 content_len, (unsigned)max_bytes, url + (strlen(url) > 60 ? strlen(url) - 60 : 0));
        esp_http_client_close(cli);
        esp_http_client_cleanup(cli);
        return ESP_ERR_INVALID_SIZE;
    }

    /* ⚠️ Content-Length 可能缺失（chunked）——不能按 8KB 定死，否则 30 首歌的 JSON 会被截断
     *    表现为 cJSON_Parse 失败（第一次实测就踩到了） */
    size_t cap = (content_len > 0 && (size_t)content_len < HTTP_BODY_MAX) ? (size_t)content_len + 1
                                                                         : 8192;
    char *body = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!body) {
        esp_http_client_close(cli);
        esp_http_client_cleanup(cli);
        return ESP_ERR_NO_MEM;
    }
    size_t used = 0;
    while (true) {
        if (used + 1 >= cap) {                     /* 满了就翻倍，直到上限 */
            if (cap >= HTTP_BODY_MAX) {
                ESP_LOGW(TAG, "响应体达到上限 %u KB，可能被截断", (unsigned)(HTTP_BODY_MAX / 1024));
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
        used += n;
        /* 没有 Content-Length 的情况：读到超限也要停（否则 69KB 的 WebP 会一路吃下去） */
        if (max_bytes > 0 && used > max_bytes) {
            ESP_LOGW(TAG, "响应体已超上限 %u 字节，放弃", (unsigned)max_bytes);
            esp_http_client_close(cli);
            esp_http_client_cleanup(cli);
            heap_caps_free(body);
            return ESP_ERR_INVALID_SIZE;
        }
    }
    body[used] = 0;
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);

    ESP_LOGI(TAG, "HTTP %d，响应体 %u 字节（Content-Length=%d，前 4 字节 %02X %02X %02X %02X）",
             status, (unsigned)used, content_len,
             used > 0 ? body[0] : 0, used > 1 ? body[1] : 0,
             used > 2 ? body[2] : 0, used > 3 ? body[3] : 0);
    if (status != 200) {
        ESP_LOGE(TAG, "HTTP %d: %.120s", status, body);
        heap_caps_free(body);
        return ESP_FAIL;
    }
    if (used == 0) {
        ESP_LOGE(TAG, "响应体为空");
        heap_caps_free(body);
        return ESP_FAIL;
    }
    *body_out = body;
    if (len_out) *len_out = used;
    return ESP_OK;
}

/* 元数据请求都在局域网内，正常 100~300ms；给 6 秒是"明显坏了"的意思。
 * ⚠️ 这个超时的真正意义不是"等多久"，而是"httpd 会被阻塞多久" ——
 * 实测一次 10 秒的连接超时会让**新连接等 9.1 秒**（SYN 被反复丢弃重传），
 * 因为 httpd 的处理器在同步调用里，期间不 accept 新连接。 */
#define HTTP_TIMEOUT_META_MS  6000
/* 校验服务器地址用更短的：地址填错时要快速失败，别让用户干等 */
#define HTTP_TIMEOUT_PING_MS  4000

static esp_err_t http_get_body_lim(const char *url, char **body_out, size_t *len_out,
                                  int *status_out, size_t max_bytes)
{
    return http_get_body_ex(url, body_out, len_out, status_out, max_bytes, HTTP_TIMEOUT_META_MS);
}

static esp_err_t http_get_body(const char *url, char **body_out, size_t *len_out, int *status_out)
{
    return http_get_body_ex(url, body_out, len_out, status_out, 0, HTTP_TIMEOUT_META_MS);
}

/* ping 的通用实现：用【给定】的凭据，不动全局配置（网页"测试连接"要用它） */
static esp_err_t ping_ex(const char *host, const char *user, const char *pass,
                         char *server_info, size_t info_len)
{
    if (host == NULL || host[0] == 0) return ESP_ERR_INVALID_ARG;
    char url[320];
    snprintf(url, sizeof(url), "%s/rest/ping.view?f=json", host);
    append_auth_ex(url, sizeof(url), user, pass);

    char *body = NULL;
    /* ping 全程用短超时：地址填错时快速失败（也少阻塞 httpd） */
    esp_err_t err = http_get_body_ex(url, &body, NULL, NULL, 0, HTTP_TIMEOUT_PING_MS);
    if (err != ESP_OK) return err;

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        ESP_LOGE(TAG, "ping 响应不是 JSON，开头: %.120s", body ? body : "");
        heap_caps_free(body);
        return ESP_FAIL;
    }
    heap_caps_free(body);
    cJSON *resp = cJSON_GetObjectItem(root, "subsonic-response");
    cJSON *status = resp ? cJSON_GetObjectItem(resp, "status") : NULL;
    bool ok = status && cJSON_IsString(status) && !strcmp(status->valuestring, "ok");
    cJSON *ver = resp ? cJSON_GetObjectItem(resp, "version") : NULL;
    cJSON *type = resp ? cJSON_GetObjectItem(resp, "type") : NULL;
    cJSON *sver = resp ? cJSON_GetObjectItem(resp, "serverVersion") : NULL;
    if (ok) {
        snprintf(server_info, info_len, "%s %s (api v%s)",
                 type && cJSON_IsString(type) ? type->valuestring : "?",
                 sver && cJSON_IsString(sver) ? sver->valuestring : "?",
                 ver && cJSON_IsString(ver) ? ver->valuestring : "?");
        ESP_LOGI(TAG, "ping ok: %s", server_info);
    } else {
        /* Navidrome 鉴权失败时返回的是 status:failed + error 对象，把原因带出来 */
        cJSON *e = resp ? cJSON_GetObjectItem(resp, "error") : NULL;
        cJSON *msg = e ? cJSON_GetObjectItem(e, "message") : NULL;
        if (msg && cJSON_IsString(msg)) {
            snprintf(server_info, info_len, "%.*s", (int)(info_len - 1), msg->valuestring);
        } else {
            snprintf(server_info, info_len, "服务器返回 status != ok");
        }
        ESP_LOGE(TAG, "ping 失败（检查 host/账号密码）: %s", server_info);
    }
    cJSON_Delete(root);
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t subsonic_ping(char *server_info, size_t info_len)
{
    if (!subsonic_configured()) return ESP_ERR_INVALID_STATE;
    return ping_ex(s_host, s_user, s_pass, server_info, info_len);
}

/* 用一组候选配置试连一次（不保存、不动全局状态） */
esp_err_t subsonic_test_config(const char *host, const char *user, const char *pass,
                               char *server_info, size_t info_len)
{
    /* 密码留空 = 沿用已存的（网页上"不改密码"的情况） */
    const char *p = (pass && pass[0]) ? pass : s_pass;
    return ping_ex(host, user && user[0] ? user : s_user, p, server_info, info_len);
}

/* 保存配置：NVS(ns "nav") + 立刻生效（不用重启） */
esp_err_t subsonic_set_config(const char *host, const char *user, const char *pass,
                              const char *transcode, int max_kbps)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_NAV, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    if (host && host[0])      nvs_set_str(h, "host", host);
    if (user && user[0])      nvs_set_str(h, "user", user);
    if (pass && pass[0])      nvs_set_str(h, "pass", pass);
    if (transcode && transcode[0]) nvs_set_str(h, "transcode", transcode);
    if (max_kbps > 0)         nvs_set_i32(h, "max_kbps", max_kbps);
    err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;

    if (host && host[0])           strlcpy(s_host, host, sizeof(s_host));
    if (user && user[0])           strlcpy(s_user, user, sizeof(s_user));
    if (pass && pass[0])           strlcpy(s_pass, pass, sizeof(s_pass));
    if (transcode && transcode[0]) strlcpy(s_transcode, transcode, sizeof(s_transcode));
    if (max_kbps > 0)              s_max_kbps = max_kbps;
    ESP_LOGW(TAG, "Navidrome 配置已保存并生效: host=%s user=%s 转码=%s %d kbps",
             s_host, s_user, s_transcode, s_max_kbps);
    return ESP_OK;
}

bool subsonic_has_password(void) { return s_pass[0] != 0; }

/* 曲库搜索（M5 网页的搜索框）。artist/album 都关掉，只要歌 */
esp_err_t subsonic_search(const char *query, sub_song_t *out, int max, int *got)
{
    *got = 0;
    if (!subsonic_configured() || query == NULL || query[0] == 0) return ESP_ERR_INVALID_ARG;
    if (max > SUB_SONGS_MAX) max = SUB_SONGS_MAX;

    /* query 要 URL 编码（中文/空格），否则 Navidrome 收不到 */
    char enc[256];
    size_t o = 0;
    for (const char *p = query; *p && o + 4 < sizeof(enc); p++) {
        unsigned char c = (unsigned char)*p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
            || c == '-' || c == '_' || c == '.' || c == '~') {
            enc[o++] = (char)c;
        } else {
            o += snprintf(enc + o, sizeof(enc) - o, "%%%02X", c);
        }
    }
    enc[o] = 0;

    char url[512];
    snprintf(url, sizeof(url),
             "%s/rest/search3.view?query=%s&songCount=%d&artistCount=0&albumCount=0&f=json",
             s_host, enc, max);
    append_auth(url, sizeof(url));
    return get_song_list(url, "searchResult3", "song", "搜索", out, max, got);
}

static void copy_str(char *dst, size_t len, const cJSON *obj, const char *key)
{
    const cJSON *it = cJSON_GetObjectItem(obj, key);
    if (it && cJSON_IsString(it) && it->valuestring) {
        strlcpy(dst, it->valuestring, len);
    } else {
        dst[0] = 0;
    }
}

/* 把 JSON 里的一个 song 数组填进 out[]。getRandomSongs / getStarred2 共用。 */
static int parse_song_array(const cJSON *arr, sub_song_t *out, int max)
{
    int n = 0;
    if (!cJSON_IsArray(arr)) return 0;
    cJSON *s = NULL;
    cJSON_ArrayForEach(s, arr) {
        if (n >= max) break;
        sub_song_t *d = &out[n];
        memset(d, 0, sizeof(*d));
        copy_str(d->id, sizeof(d->id), s, "id");
        copy_str(d->title, sizeof(d->title), s, "title");
        copy_str(d->artist, sizeof(d->artist), s, "artist");
        copy_str(d->album, sizeof(d->album), s, "album");
        copy_str(d->cover, sizeof(d->cover), s, "coverArt");
        copy_str(d->suffix, sizeof(d->suffix), s, "suffix");
        const cJSON *dur = cJSON_GetObjectItem(s, "duration");
        const cJSON *sz  = cJSON_GetObjectItem(s, "size");
        const cJSON *st  = cJSON_GetObjectItem(s, "starred");
        if (cJSON_IsNumber(dur)) d->duration_s = (uint32_t)dur->valuedouble;
        if (cJSON_IsNumber(sz))  d->size = (uint32_t)sz->valuedouble;
        d->starred = (st != NULL);
        if (d->id[0]) n++;
    }
    return n;
}

/*
 * 通用取歌单：请求一个 .view，从 subsonic-response.<key>.<sub> 取 song 数组。
 * path 里可以带 %d 之类的（由调用方先 snprintf 好）。
 */
static esp_err_t get_song_list(const char *url, const char *key, const char *sub,
                               const char *what, sub_song_t *out, int max, int *got)
{
    *got = 0;
    if (!subsonic_configured()) return ESP_ERR_INVALID_STATE;
    if (max > SUB_SONGS_MAX) max = SUB_SONGS_MAX;

    char *body = NULL;
    esp_err_t err = http_get_body(url, &body, NULL, NULL);
    if (err != ESP_OK) return err;

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        ESP_LOGE(TAG, "%s: JSON 解析失败，响应体 %u 字节，开头: %.200s",
                 what, (unsigned)strlen(body), body);
        heap_caps_free(body);
        return ESP_FAIL;
    }
    heap_caps_free(body);

    cJSON *resp = cJSON_GetObjectItem(root, "subsonic-response");
    cJSON *node = resp ? cJSON_GetObjectItem(resp, key) : NULL;
    cJSON *songs = node ? cJSON_GetObjectItem(node, sub) : NULL;
    int n = parse_song_array(songs, out, max);
    cJSON_Delete(root);
    *got = n;
    ESP_LOGI(TAG, "%s: 取到 %d 首歌（第一首: \"%s\" - %s, %" PRIu32 "s, %s）",
             what, n, n ? out[0].title : "-", n ? out[0].artist : "-",
             n ? out[0].duration_s : 0, n ? out[0].suffix : "-");
    /* ⚠️ 解析成功就返回 ESP_OK —— **0 条也是成功**，不是错误。
     * 以前这里 `n > 0 ? ESP_OK : ESP_FAIL`，后果是"搜不到这首歌"被当成
     * "Navidrome 不可达"：实测搜 zzzznotexist → /api/search 返回
     * `502 {"error":"搜索失败（Navidrome 不可达？）"}`，而服务器其实好得很。
     * 顺带修好另一种情况：**收藏为 0 首的用户**以前会被永久判成"拉收藏失败"。
     * "有没有歌"交给调用方按 *got 判断（load_source 那边本来就有 got <= 0 的处理）。 */
    return ESP_OK;
}

esp_err_t subsonic_get_random(sub_song_t *out, int max, int *got)
{
    *got = 0;
    if (!subsonic_configured()) return ESP_ERR_INVALID_STATE;
    if (max > SUB_SONGS_MAX) max = SUB_SONGS_MAX;
    char url[384];
    snprintf(url, sizeof(url), "%s/rest/getRandomSongs.view?size=%d&f=json", s_host, max);
    append_auth(url, sizeof(url));
    return get_song_list(url, "randomSongs", "song", "随机歌曲", out, max, got);
}

esp_err_t subsonic_get_starred(sub_song_t *out, int max, int *got)
{
    *got = 0;
    if (!subsonic_configured()) return ESP_ERR_INVALID_STATE;
    if (max > SUB_SONGS_MAX) max = SUB_SONGS_MAX;
    /* getStarred2 是 id3 版的升级（getStarred 已废弃，字段更少） */
    char url[384];
    snprintf(url, sizeof(url), "%s/rest/getStarred2.view?f=json", s_host);
    append_auth(url, sizeof(url));
    return get_song_list(url, "starred2", "song", "我的收藏", out, max, got);
}

/* 收藏/取消收藏（与 Navidrome 双向同步） */
static esp_err_t star_action(const char *view, const char *song_id)
{
    if (!subsonic_configured()) return ESP_ERR_INVALID_STATE;
    char url[384];
    snprintf(url, sizeof(url), "%s/rest/%s.view?id=%s&f=json", s_host, view, song_id);
    append_auth(url, sizeof(url));

    char *body = NULL;
    esp_err_t err = http_get_body(url, &body, NULL, NULL);
    if (err != ESP_OK) return err;
    /* 这类接口没有实体数据，只看 subsonic-response.status */
    bool ok = strstr(body, "\"status\":\"ok\"") != NULL;
    if (!ok) ESP_LOGE(TAG, "%s 失败: %.200s", view, body);
    heap_caps_free(body);
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t subsonic_star(const char *song_id, bool on)
{
    if (!song_id || !song_id[0]) return ESP_ERR_INVALID_ARG;
    return star_action(on ? "star" : "unstar", song_id);
}

/* offset_s = 0 时不带 timeOffset */
static esp_err_t stream_url_ex(const sub_song_t *song, int max_kbps, uint32_t offset_s,
                               char *out, size_t out_len)
{
    if (!subsonic_configured()) return ESP_ERR_INVALID_STATE;
    if (max_kbps <= 0) max_kbps = s_max_kbps;

    bool raw = !strcmp(s_transcode, "raw");
    if (raw) {
        snprintf(out, out_len, "%s/rest/stream.view?id=%s&format=raw&f=json", s_host, song->id);
    } else {
        /* ⚠️ format 必须显式给：只给 maxBitRate 时 Navidrome 默认给 Opus（ESP32 上不划算） */
        snprintf(out, out_len, "%s/rest/stream.view?id=%s&format=%s&maxBitRate=%d&estimateContentLength=true&f=json",
                 s_host, song->id, s_transcode[0] ? s_transcode : "mp3", max_kbps);
    }
    /* ★ seek：转码流没有 Accept-Ranges（实测），所以只能让服务端从 timeOffset 开始转。
     *   这是 OpenSubsonic 的 transcodeOffset 扩展，Navidrome 0.63 支持。
     *   ⚠️ 必须在 append_auth 之前拼，且只有真转码时才有效（raw 时会被静默忽略）。 */
    if (offset_s > 0) {
        size_t cur = strlen(out);
        snprintf(out + cur, out_len - cur, "&timeOffset=%" PRIu32, offset_s);
    }
    append_auth(out, out_len);
    return ESP_OK;
}

esp_err_t subsonic_stream_url(const sub_song_t *song, int max_kbps, char *out, size_t out_len)
{
    return stream_url_ex(song, max_kbps, 0, out, out_len);
}

esp_err_t subsonic_stream_url_at(const sub_song_t *song, int max_kbps, uint32_t offset_s,
                                 char *out, size_t out_len)
{
    return stream_url_ex(song, max_kbps, offset_s, out, out_len);
}

esp_err_t subsonic_get_lyrics_json(const char *song_id, char **json_out)
{
    if (!subsonic_configured()) return ESP_ERR_INVALID_STATE;
    char url[320];
    snprintf(url, sizeof(url), "%s/rest/getLyricsBySongId.view?id=%s&f=json", s_host, song_id);
    append_auth(url, sizeof(url));
    return http_get_body(url, json_out, NULL, NULL);
}

/*
 * 歌词（归一化后给网页用）。调用方负责 heap_caps_free(*json_out)。
 *
 * Navidrome 的原始结构是
 *   subsonic-response.lyricsList.structuredLyrics[0] = {synced:bool, line:[{start,value}]}
 * 这里统一成
 *   {"synced":bool, "total":N, "lines":[{"t":<ms>,"text":"..."}]}
 * 让前端只写一套解析。
 *
 * ⚠️ 没歌词时返回 **ESP_OK + 一个空 lines 的 JSON**，不是错误 —— 大部分纯音乐都是这种，
 *    前端要显示"暂无歌词"而不是报错。实测服务器对纯音乐会给一行占位文本
 *    "此歌曲为没有填词的纯音乐，请您欣赏"，这种情况也归一化成空。
 */
esp_err_t subsonic_get_lyrics(const char *song_id, char **json_out)
{
    *json_out = NULL;
    if (!subsonic_configured() || song_id == NULL || song_id[0] == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    char url[320];
    snprintf(url, sizeof(url), "%s/rest/getLyricsBySongId.view?id=%s&f=json", s_host, song_id);
    append_auth(url, sizeof(url));

    char *body = NULL;
    esp_err_t err = http_get_body(url, &body, NULL, NULL);
    if (err != ESP_OK) return err;
    cJSON *root = cJSON_Parse(body);
    heap_caps_free(body);
    if (!root) return ESP_FAIL;

    cJSON *resp = cJSON_GetObjectItem(root, "subsonic-response");
    cJSON *list = resp ? cJSON_GetObjectItem(resp, "lyricsList") : NULL;
    cJSON *sl   = list ? cJSON_GetObjectItem(list, "structuredLyrics") : NULL;

    cJSON *out  = cJSON_CreateObject();
    cJSON *arr  = cJSON_AddArrayToObject(out, "lines");
    bool synced = false;
    int n = 0;

    /* structuredLyrics 是数组（同一首歌可能有多份歌词），取第一份 */
    cJSON *first = cJSON_IsArray(sl) ? cJSON_GetArrayItem(sl, 0) : NULL;
    if (first) {
        cJSON *sw = cJSON_GetObjectItem(first, "synced");
        synced = cJSON_IsTrue(sw);
        cJSON *lines = cJSON_GetObjectItem(first, "line");
        if (lines == NULL) lines = cJSON_GetObjectItem(first, "lines");   /* 兼容另一种拼写 */
        cJSON *ln = NULL;
        cJSON_ArrayForEach(ln, lines) {
            cJSON *v = cJSON_GetObjectItem(ln, "value");
            if (v == NULL) v = cJSON_GetObjectItem(ln, "text");
            if (!cJSON_IsString(v)) continue;
            cJSON *o = cJSON_CreateObject();
            cJSON *st = cJSON_GetObjectItem(ln, "start");
            if (st == NULL) st = cJSON_GetObjectItem(ln, "offset");
            cJSON_AddNumberToObject(o, "t", cJSON_IsNumber(st) ? st->valuedouble : 0);
            cJSON_AddStringToObject(o, "text", v->valuestring);
            cJSON_AddItemToArray(arr, o);
            n++;
        }
    }

    /* 纯音乐占位：服务器只给一行"此歌曲为没有填词的纯音乐"，当成没有歌词 */
    bool placeholder = false;
    if (n == 1) {
        cJSON *only = cJSON_GetArrayItem(arr, 0);
        cJSON *tx = cJSON_GetObjectItem(only, "text");
        if (tx && cJSON_IsString(tx)
            && (strstr(tx->valuestring, "纯音乐") || strstr(tx->valuestring, "没有填词"))) {
            placeholder = true;
        }
    }
    if (placeholder) {
        cJSON_DeleteItemFromArray(arr, 0);
        n = 0;
        synced = false;
    }

    cJSON_AddNumberToObject(out, "total", n);
    cJSON_AddBoolToObject(out, "synced", n > 0 && synced);
    cJSON_Delete(root);

    *json_out = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    if (*json_out == NULL) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "歌词 %s: %d 行%s", song_id, n,
             n == 0 ? "（无）" : (synced ? "（同步）" : "（无时间轴）"));
    return ESP_OK;
}

/* ============================================================ 封面 */

/* 原始封面字节（校验过是 JPEG）—— 网页控制台的封面代理要用它直接转给浏览器，
 * 不能给 RGB565（浏览器不认）。调用方负责 heap_caps_free(*out)。 */
esp_err_t subsonic_get_cover_raw(const char *cover_id, int size_px,
                                 uint8_t **out, size_t *len_out)
{
    *out = NULL;
    *len_out = 0;
    if (!subsonic_configured() || cover_id == NULL || cover_id[0] == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (size_px <= 0) size_px = 96;
    /* ⚠️ 别传大 size：Navidrome 有个 CVE 就是 size 过大把服务器打爆的 */
    if (size_px > 300) size_px = 300;

    char url[384];
    snprintf(url, sizeof(url), "%s/rest/getCoverArt.view?id=%s&size=%d", s_host, cover_id, size_px);
    append_auth(url, sizeof(url));

    /* ⚠️ ⚠️ Navidrome 对【内嵌封面是 WebP 的专辑】会原样透传 WebP，且无视 size 参数。
     *    原因是它用的 Go 图像库（disintegration/imaging）没有 WebP 解码器，转不了码，
     *    只能把原图给你 —— 实测一张 72px 的请求回来 69228 字节的 RIFF/WEBP，
     *    而 esp_new_jpeg 只能解 JPEG，表现是"这首歌没有封面"。
     *    所以两道闸：① 超过尺寸上限就别下了（省掉几万字节的浪费）；
     *              ② 下了也要验魔数，不是 JPEG 就当没有，别喂给解码器。 */
    size_t cap_bytes = (size_t)size_px * size_px * 2;    /* 72px → 10KB；给足余量 */
    if (cap_bytes < 16384) cap_bytes = 16384;

    char *jpeg = NULL;
    int status = 0;
    size_t jpeg_len = 0;
    esp_err_t err = http_get_body_lim(url, &jpeg, &jpeg_len, &status, cap_bytes);
    if (err == ESP_ERR_INVALID_SIZE) {
        ESP_LOGW(TAG, "封面 %s: 服务端没按 size=%d 转码（多半是 WebP，Navidrome 转不了）—— 用占位图",
                 cover_id, size_px);
        return err;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "封面下载失败 (%d): %.80s", status, jpeg ? jpeg : "");
        return err;
    }
    if (jpeg_len < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) {
        ESP_LOGW(TAG, "封面 %s 不是 JPEG（前 4 字节 %02X %02X %02X %02X）—— 用占位图",
                 cover_id, jpeg[0], jpeg_len > 1 ? jpeg[1] : 0,
                 jpeg_len > 2 ? jpeg[2] : 0, jpeg_len > 3 ? jpeg[3] : 0);
        heap_caps_free(jpeg);
        return ESP_ERR_NOT_SUPPORTED;
    }
    *out = (uint8_t *)jpeg;
    *len_out = jpeg_len;
    return ESP_OK;
}

/* 单曲详情：网页按 id 播放"搜索/收藏里的歌"时，那首歌还不在队列里，得先把元数据取回来 */
esp_err_t subsonic_get_song(const char *song_id, sub_song_t *out)
{
    if (!subsonic_configured() || song_id == NULL || song_id[0] == 0) return ESP_ERR_INVALID_ARG;
    char url[384];
    snprintf(url, sizeof(url), "%s/rest/getSong.view?id=%s&f=json", s_host, song_id);
    append_auth(url, sizeof(url));

    char *body = NULL;
    esp_err_t err = http_get_body(url, &body, NULL, NULL);
    if (err != ESP_OK) return err;
    cJSON *root = cJSON_Parse(body);
    heap_caps_free(body);
    if (!root) return ESP_FAIL;

    int n = 0;
    memset(out, 0, sizeof(*out));
    cJSON *resp = cJSON_GetObjectItem(root, "subsonic-response");
    cJSON *s    = resp ? cJSON_GetObjectItem(resp, "song") : NULL;
    if (cJSON_IsObject(s)) {
        copy_str(out->id, sizeof(out->id), s, "id");
        copy_str(out->title, sizeof(out->title), s, "title");
        copy_str(out->artist, sizeof(out->artist), s, "artist");
        copy_str(out->album, sizeof(out->album), s, "album");
        copy_str(out->cover, sizeof(out->cover), s, "coverArt");
        copy_str(out->suffix, sizeof(out->suffix), s, "suffix");
        const cJSON *dur = cJSON_GetObjectItem(s, "duration");
        const cJSON *sz  = cJSON_GetObjectItem(s, "size");
        const cJSON *st  = cJSON_GetObjectItem(s, "starred");
        if (cJSON_IsNumber(dur)) out->duration_s = (uint32_t)dur->valuedouble;
        if (cJSON_IsNumber(sz))  out->size = (uint32_t)sz->valuedouble;
        out->starred = (st != NULL);
        n = out->id[0] ? 1 : 0;
    }
    cJSON_Delete(root);
    if (!n) ESP_LOGW(TAG, "取不到歌曲 %s 的详情", song_id);
    return n ? ESP_OK : ESP_FAIL;
}

esp_err_t subsonic_get_cover_rgb565(const char *cover_id, int size_px,
                                    uint8_t *out, size_t out_cap,
                                    uint16_t *w_out, uint16_t *h_out)
{
    uint8_t *jpeg = NULL;
    size_t jpeg_len = 0;
    esp_err_t err = subsonic_get_cover_raw(cover_id, size_px, &jpeg, &jpeg_len);
    if (err != ESP_OK) return err;


    jpeg_dec_config_t cfg = {
        .output_type = JPEG_PIXEL_FORMAT_RGB565_BE,   /* 大端：与本机屏幕字节序一致 */
        .scale       = { 0, 0 },
        .clipper     = { 0, 0 },
        .rotate      = JPEG_ROTATE_0D,
        .block_enable = false,
    };
    jpeg_dec_handle_t dec = NULL;
    jpeg_error_t jerr = jpeg_dec_open(&cfg, &dec);
    if (jerr != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "JPEG 解码器打开失败: %d", jerr);
        heap_caps_free(jpeg);
        return ESP_FAIL;
    }

    jpeg_dec_io_t io = { .inbuf = (uint8_t *)jpeg, .inbuf_len = (int)jpeg_len };
    jpeg_dec_header_info_t info = {0};
    jerr = jpeg_dec_parse_header(dec, &io, &info);
    if (jerr != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "JPEG 头解析失败: %d (%u 字节)", jerr, (unsigned)jpeg_len);
        jpeg_dec_close(dec);
        heap_caps_free(jpeg);
        return ESP_FAIL;
    }
    size_t need = (size_t)info.width * info.height * 2;
    if (need > out_cap) {
        ESP_LOGE(TAG, "封面缓冲不够: 需要 %u，只有 %u", (unsigned)need, (unsigned)out_cap);
        jpeg_dec_close(dec);
        heap_caps_free(jpeg);
        return ESP_ERR_NO_MEM;
    }
    io.outbuf   = out;
    io.out_size = (int)need;
    jerr = jpeg_dec_process(dec, &io);
    jpeg_dec_close(dec);
    heap_caps_free(jpeg);
    if (jerr != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "JPEG 解码失败: %d（%ux%u，需 %u 字节，outbuf=%p 对齐 %u）",
                 jerr, info.width, info.height, (unsigned)need, out,
                 (unsigned)((uintptr_t)out & 0xF));
        return ESP_FAIL;
    }
    if (w_out) *w_out = info.width;
    if (h_out) *h_out = info.height;
    ESP_LOGI(TAG, "封面 %dx%d 解码完成（%u 字节 RGB565-BE）",
             info.width, info.height, (unsigned)need);
    return ESP_OK;
}
