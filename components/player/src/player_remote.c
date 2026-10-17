/*
 * player_remote 实现 —— 见头文件顶部的设计说明。
 */
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include "sdkconfig.h"
#include "qqmusic.h"
#include "player.h"
#include "player_remote.h"

static const char *TAG = "plremote";

#define HTTP_TIMEOUT_MS   4000
#define BODY_MAX          (40 * 1024)     /* 队列 64 首 ≈ 13KB，给足余量 */
#define MODES_TTL_MS      (5 * 60 * 1000) /* 模式表 5 分钟刷一次（服务端加了模式也能跟上）*/

static SemaphoreHandle_t s_lock;
static remote_status_t   s_st;
static sub_song_t       *s_q;             /* PSRAM，REMOTE_QUEUE_MAX 条 */
static int               s_qn;
static uint32_t          s_qgen_seen;

/* 模式键缓存：s_modes[back][idx] = "fav" / "daily" / … */
static char  s_modes[2][8][24];
static int   s_mode_n[2];
static int64_t s_modes_at;

/* ============================================================ 小工具 */

static void base_url(char *out, size_t len)
{
    const char *h = qqmusic_host();
    if (h == NULL || h[0] == 0) h = CONFIG_APP_QQ_HOST;
    strlcpy(out, h, len);
    size_t n = strlen(out);
    while (n > 0 && out[n - 1] == '/') out[--n] = 0;
}

static const char *jstr(const cJSON *o, const char *k, char *buf, size_t len)
{
    buf[0] = 0;
    if (!cJSON_IsObject(o)) return buf;
    const cJSON *it = cJSON_GetObjectItem(o, k);
    if (cJSON_IsString(it) && it->valuestring) strlcpy(buf, it->valuestring, len);
    return buf;
}

static int jint(const cJSON *o, const char *k, int def)
{
    if (!cJSON_IsObject(o)) return def;
    const cJSON *it = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(it) ? (int)it->valuedouble : def;
}

static bool jbool(const cJSON *o, const char *k)
{
    if (!cJSON_IsObject(o)) return false;
    return cJSON_IsTrue(cJSON_GetObjectItem(o, k));
}

/* GET 一个接口，body 放 PSRAM（调用方 heap_caps_free）。非 200 当失败。 */
static esp_err_t http_get(const char *path, char **body_out, size_t *len_out)
{
    *body_out = NULL;
    if (len_out) *len_out = 0;

    char base[192], url[384];
    base_url(base, sizeof(base));
    snprintf(url, sizeof(url), "%s%s", base, path);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 1024,
        .keep_alive_enable = true,      /* 状态每秒一条，复用连接省握手 */
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (cli == NULL) return ESP_FAIL;

    esp_err_t err = esp_http_client_open(cli, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "连不上服务器 %s: %s", url, esp_err_to_name(err));
        esp_http_client_cleanup(cli);
        return err;
    }
    int content_len = esp_http_client_fetch_headers(cli);
    int status = esp_http_client_get_status_code(cli);
    size_t cap = (content_len > 0 && (size_t)content_len < BODY_MAX) ? (size_t)content_len + 1
                                                                    : 4096;
    char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        esp_http_client_close(cli);
        esp_http_client_cleanup(cli);
        return ESP_ERR_NO_MEM;
    }
    size_t used = 0;
    while (true) {
        if (used + 1 >= cap) {
            if (cap >= BODY_MAX) break;
            size_t ncap = cap * 2 > BODY_MAX ? BODY_MAX : cap * 2;
            char *nb = heap_caps_malloc(ncap, MALLOC_CAP_SPIRAM);
            if (nb == NULL) break;
            memcpy(nb, buf, used);
            heap_caps_free(buf);
            buf = nb;
            cap = ncap;
        }
        int n = esp_http_client_read(cli, buf + used, cap - 1 - used);
        if (n <= 0) break;
        used += n;
    }
    buf[used] = 0;
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);

    if (status != 200) {
        /* 服务器明确回错误时把它的 detail 打出来（任务队列空/没配账号之类） */
        ESP_LOGW(TAG, "服务器 %d: %.160s", status, buf);
        heap_caps_free(buf);
        return ESP_FAIL;
    }
    *body_out = buf;
    if (len_out) *len_out = used;
    return ESP_OK;
}

static esp_err_t http_cmd(const char *path)
{
    char *body = NULL;
    esp_err_t e = http_get(path, &body, NULL);
    if (body) heap_caps_free(body);
    return e;
}

/* ============================================================ 解析 */

static void parse_status(const char *json, remote_status_t *st)
{
    memset(st, 0, sizeof(*st));
    cJSON *r = cJSON_Parse(json);
    if (r == NULL) {
        snprintf(st->err, sizeof(st->err), "状态不是合法 JSON");
        return;
    }
    st->valid  = true;
    st->mpv_ok = jbool(r, "mpv_ok");
    jstr(r, "state", st->state, sizeof(st->state));
    st->pos_ms = (uint32_t)jint(r, "pos_ms", 0);
    st->dur_ms = (uint32_t)jint(r, "dur_ms", 0);
    st->volume = jint(r, "volume", 0);
    st->gen    = (uint32_t)jint(r, "gen", 0);
    jstr(r, "error", st->err, sizeof(st->err));

    const cJSON *song = cJSON_GetObjectItem(r, "song");
    jstr(song, "id",     st->id,     sizeof(st->id));
    jstr(song, "title",  st->title,  sizeof(st->title));
    jstr(song, "artist", st->artist, sizeof(st->artist));
    jstr(song, "album",  st->album,  sizeof(st->album));
    jstr(song, "cover",  st->cover,  sizeof(st->cover));
    jstr(song, "suffix", st->suffix, sizeof(st->suffix));
    st->back = jint(song, "back", 0);

    const cJSON *q = cJSON_GetObjectItem(r, "queue");
    st->q_index = jint(q, "index", -1);
    st->q_count = jint(q, "count", 0);
    jstr(q, "mode",     st->q_mode,     sizeof(st->q_mode));
    jstr(q, "mode_key", st->q_mode_key, sizeof(st->q_mode_key));
    cJSON_Delete(r);
}

static int parse_queue(const char *json, sub_song_t *out, int max)
{
    cJSON *r = cJSON_Parse(json);
    if (r == NULL) return 0;
    const cJSON *items = cJSON_GetObjectItem(r, "items");
    int n = 0;
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, items) {
        if (n >= max) break;
        sub_song_t *s = &out[n];
        memset(s, 0, sizeof(*s));
        jstr(it, "id",     s->id,     sizeof(s->id));
        jstr(it, "title",  s->title,  sizeof(s->title));
        jstr(it, "artist", s->artist, sizeof(s->artist));
        jstr(it, "album",  s->album,  sizeof(s->album));
        jstr(it, "cover",  s->cover,  sizeof(s->cover));
        jstr(it, "suffix", s->suffix, sizeof(s->suffix));
        s->back = (uint8_t)jint(it, "back", 0);
        /* 服务器给的是 dur_ms，本地结构用秒 */
        s->duration_s = (uint32_t)(jint(it, "dur_ms", 0) / 1000);
        s->size = (uint32_t)jint(it, "size", 0);
        n++;
    }
    cJSON_Delete(r);
    return n;
}

/* ============================================================ 生命周期 */

esp_err_t player_remote_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) return ESP_FAIL;
    }
    if (s_q == NULL) {
        s_q = heap_caps_calloc(REMOTE_QUEUE_MAX, sizeof(sub_song_t), MALLOC_CAP_SPIRAM);
        if (s_q == NULL) {
            ESP_LOGE(TAG, "远程队列分配失败（%u KB）",
                     (unsigned)(REMOTE_QUEUE_MAX * sizeof(sub_song_t) / 1024));
            return ESP_ERR_NO_MEM;
        }
    }
    ESP_LOGI(TAG, "远程播放器就绪（服务器 = %s）", qqmusic_host());
    return ESP_OK;
}

void player_remote_reset(void)
{
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&s_st, 0, sizeof(s_st));
    s_qn = 0;
    s_qgen_seen = 0;
    xSemaphoreGive(s_lock);
}

void player_remote_status(remote_status_t *out)
{
    if (s_lock == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_st;
    xSemaphoreGive(s_lock);
}

uint32_t player_remote_queue_gen(void)
{
    if (s_lock == NULL) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t g = s_qgen_seen;
    xSemaphoreGive(s_lock);
    return g;
}

int player_remote_queue_copy(sub_song_t *out, int max)
{
    if (s_lock == NULL || s_q == NULL) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_qn < max ? s_qn : max;
    if (n > 0) memcpy(out, s_q, (size_t)n * sizeof(sub_song_t));
    xSemaphoreGive(s_lock);
    return n;
}

bool player_remote_online(void)
{
    if (s_lock == NULL) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool v = s_st.valid;
    xSemaphoreGive(s_lock);
    return v;
}

const char *player_remote_last_err(void)
{
    static char buf[96];
    if (s_lock == NULL) return "";
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(buf, s_st.err, sizeof(buf));
    xSemaphoreGive(s_lock);
    return buf;
}

/* ============================================================ 拉状态 */

esp_err_t player_remote_fetch(void)
{
    /* 首轮面包屑：万一又在启动时崩，RTC 黑匣子里能看出崩在哪一步（拉之前/之后）*/
    static bool first = true, first_done = true;
    if (first) {
        first = false;
        ESP_LOGW(TAG, "首次拉状态：GET %s/v1/player/status", qqmusic_host());
    }

    char *body = NULL;
    size_t len = 0;
    esp_err_t err = http_get("/v1/player/status", &body, &len);
    if (err != ESP_OK) {
        if (s_lock) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_st.valid = false;
            if (s_st.err[0] == 0) {
                snprintf(s_st.err, sizeof(s_st.err), "连不上服务器（%s）", esp_err_to_name(err));
            }
            xSemaphoreGive(s_lock);
        }
        return err;
    }

    remote_status_t st;
    parse_status(body, &st);
    heap_caps_free(body);
    if (first_done && st.valid) {
        first_done = false;
        ESP_LOGW(TAG, "首次拉状态成功：state=%s pos=%" PRIu32 "ms 歌=%s，队列 %d 首",
                 st.state, st.pos_ms, st.title[0] ? st.title : "(空)", st.q_count);
    }

    /* 队列明细 13KB 左右，只有 gen 变了才拉 */
    bool need_q = (st.gen != s_qgen_seen);
    if (!st.valid) {
        if (s_lock) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_st = st;
            xSemaphoreGive(s_lock);
        }
        return ESP_FAIL;
    }
    if (need_q) {
        char *qb = NULL;
        size_t ql = 0;
        if (http_get("/v1/player/queue", &qb, &ql) == ESP_OK) {
            int n = parse_queue(qb, s_q, REMOTE_QUEUE_MAX);
            heap_caps_free(qb);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_qn = n;
            s_qgen_seen = st.gen;
            xSemaphoreGive(s_lock);
        }
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st = st;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

/* ============================================================ 模式表 */

static esp_err_t ensure_modes(void)
{
    int64_t now = esp_timer_get_time() / 1000;
    if (s_modes_at != 0 && now - s_modes_at < MODES_TTL_MS) return ESP_OK;

    char *body = NULL;
    size_t len = 0;
    esp_err_t err = http_get("/v1/player/modes", &body, &len);
    if (err != ESP_OK) return err;

    cJSON *r = cJSON_Parse(body);
    heap_caps_free(body);
    if (r == NULL) return ESP_FAIL;

    s_mode_n[0] = s_mode_n[1] = 0;
    const cJSON *sources = cJSON_GetObjectItem(r, "sources");
    const cJSON *src = NULL;
    cJSON_ArrayForEach(src, sources) {
        int back = jint(src, "back", 0);
        if (back < 0 || back > 1) continue;
        const cJSON *modes = cJSON_GetObjectItem(src, "modes");
        const cJSON *m = NULL;
        cJSON_ArrayForEach(m, modes) {
            int idx = jint(m, "index", -1);
            if (idx < 0 || idx >= 8) continue;
            jstr(m, "mode", s_modes[back][idx], sizeof(s_modes[back][idx]));
            if (idx + 1 > s_mode_n[back]) s_mode_n[back] = idx + 1;
        }
    }
    cJSON_Delete(r);
    s_modes_at = now;
    ESP_LOGI(TAG, "模式表已更新：QQ %d 个 / Nav %d 个", s_mode_n[1], s_mode_n[0]);
    return (s_mode_n[0] + s_mode_n[1]) > 0 ? ESP_OK : ESP_FAIL;
}

/* ============================================================ 命令 */

esp_err_t player_remote_toggle(void) { return http_cmd("/v1/player/toggle"); }
esp_err_t player_remote_next(void)   { return http_cmd("/v1/player/next"); }
esp_err_t player_remote_prev(void)   { return http_cmd("/v1/player/prev"); }
esp_err_t player_remote_stop(void)   { return http_cmd("/v1/player/stop"); }

esp_err_t player_remote_seek(uint32_t pos_ms)
{
    char path[96];
    snprintf(path, sizeof(path), "/v1/player/seek?pos_ms=%" PRIu32, pos_ms);
    return http_cmd(path);
}

esp_err_t player_remote_volume(int level)
{
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    char path[64];
    snprintf(path, sizeof(path), "/v1/player/volume?level=%d", level);
    return http_cmd(path);
}

esp_err_t player_remote_play_id(int back, const char *id)
{
    char path[160];
    snprintf(path, sizeof(path), "/v1/player/play?source=%s&id=%s",
             back == 1 ? "qq" : "nav", id);
    return http_cmd(path);
}

esp_err_t player_remote_play_mode(int back, int mode_index, int index, uint32_t pos_ms)
{
    if (back < 0 || back > 1) back = 0;
    if (mode_index < 0) mode_index = 0;
    if (ensure_modes() != ESP_OK) {
        ESP_LOGE(TAG, "拿不到服务器的模式表，无法按模式起播");
        return ESP_FAIL;
    }
    if (mode_index >= s_mode_n[back] || s_modes[back][mode_index][0] == 0) {
        ESP_LOGE(TAG, "服务器没有 %s 的第 %d 个模式", back == 1 ? "QQ" : "Nav", mode_index);
        return ESP_ERR_NOT_FOUND;
    }
    char path[192];
    int n = snprintf(path, sizeof(path),
                     "/v1/player/play?source=%s&mode=%s&index=%d&limit=%d",
                     back == 1 ? "qq" : "nav", s_modes[back][mode_index],
                     index, REMOTE_QUEUE_MAX - 4);
    /* QQ「收藏」要跳过置顶的那几首（跟本机播放器同一个设置）*/
    if (back == 1 && strcmp(s_modes[back][mode_index], "fav") == 0 && player_fav_skip() > 0) {
        snprintf(path + n, sizeof(path) - n, "&skip=%d", player_fav_skip());
    }
    esp_err_t err = http_cmd(path);
    if (err != ESP_OK) return err;
    if (pos_ms > 2000) {
        /* 同一首、同一进度接管：起播后再跳过去 */
        player_remote_seek(pos_ms);
    }
    return ESP_OK;
}
