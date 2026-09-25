/*
 * 网页控制台 —— REST API 实现
 *
 * 分层约定：这里只调各模块的公开接口（player_/subsonic_/net_mgr_/settings_），
 * 不碰 LVGL、不碰音频引擎内部。所以"网页点下一首"和"按键按下一首"走的是同一条路。
 */
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "web_console.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "assets.h"
#include "app_core.h"
#include "audio_engine.h"
#include "player.h"
#include "subsonic.h"
#include "net_mgr.h"
#include "settings.h"
#include "store.h"
#include "hid_remote.h"
#include "board.h"

static const char *TAG = "web";

/* 请求体上限。前端点列表时会把元数据（title/artist/album/cover/dur_s）一起发过来，
 * 中文标题 UTF-8 后可能上百字节，所以留够余量。
 * 缓冲在 PSRAM，放宽不影响内部 SRAM。 */
#define BODY_MAX 2048

/* ============================================================ 小工具 */

/* ⚠️ 必须显式要求关连接。IDF 的 httpd 默认走 HTTP/1.1 长连接，而且**没有**服务端
 * 强制关闭的配置项（`keep_alive_enable` 其实是 TCP keepalive，不是 HTTP 的）。
 * 后果：浏览器会保持 6 条连接不放，而设备只有 max_open_sockets 个槽位 ——
 * 于是"点歌"的 POST 要排队等 LRU 踢人 + 重连，实测 0.6~1.1 秒，
 * 用户看到的就是"点一下要顿一下"。带上 Connection: close 后每个请求用完即放。
 * 代价：每次请求多一次 TCP 握手（局域网 ~2ms），远比排队划算。 */
static void no_keepalive(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Connection", "close");
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    no_keepalive(req);
    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (txt == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, txt);
    free(txt);
    return err;
}

static esp_err_t fail(httpd_req_t *req, const char *status, const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", msg);
    char *txt = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    no_keepalive(req);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    esp_err_t err = txt ? httpd_resp_sendstr(req, txt) : ESP_FAIL;
    free(txt);
    return err;
}

/* 读 JSON 请求体（PSRAM 缓冲，调用方负责 cJSON_Delete） */
static cJSON *read_body(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > BODY_MAX) {
        return NULL;
    }
    char *buf = heap_caps_malloc(req->content_len + 1, MALLOC_CAP_SPIRAM);
    if (buf == NULL) return NULL;
    int got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, buf + got, req->content_len - got);
        if (n <= 0) {
            heap_caps_free(buf);
            return NULL;
        }
        got += n;
    }
    buf[got] = 0;
    cJSON *o = cJSON_Parse(buf);
    heap_caps_free(buf);
    return o;
}

static int json_int(cJSON *o, const char *key, int def)
{
    cJSON *it = cJSON_GetObjectItem(o, key);
    return (it && cJSON_IsNumber(it)) ? (int)it->valuedouble : def;
}
static bool json_bool(cJSON *o, const char *key, bool def)
{
    cJSON *it = cJSON_GetObjectItem(o, key);
    if (it == NULL) return def;
    if (cJSON_IsBool(it)) return cJSON_IsTrue(it);
    if (cJSON_IsNumber(it)) return it->valuedouble != 0;
    return def;
}
static const char *json_str(cJSON *o, const char *key)
{
    cJSON *it = cJSON_GetObjectItem(o, key);
    return (it && cJSON_IsString(it)) ? it->valuestring : NULL;
}

static bool query_int(httpd_req_t *req, const char *key, int *out)
{
    char val[32];
    if (httpd_req_get_url_query_len(req) == 0) return false;
    if (httpd_req_get_url_query_str(req, val, sizeof(val)) != ESP_OK) return false;
    char got[32];
    if (httpd_query_key_value(val, key, got, sizeof(got)) != ESP_OK) return false;
    *out = atoi(got);
    return true;
}

/* ⚠️ esp_http_server 【不会】对 query 做 URL 解码（httpd_req_get_url_query_str 返回的是原文），
 * 所以 "q=%E6%98%A5" 拿到的是字面量 "%E6%98%A5"。不做这一步就会二次编码，
 * 表现为"英文能搜、中文搜不到"（实测踩到）。
 * 注意：故意不把 '+' 当空格 —— 前端用的是 encodeURIComponent（空格是 %20），
 * 把 '+' 转成空格会让"搜 a+b"这种查询出错。 */
static void url_decode(char *s)
{
    char *w = s;
    for (const char *r = s; *r; r++) {
        if (*r == '%' && r[1] && r[2]) {
            char hex[3] = { r[1], r[2], 0 };
            char *end = NULL;
            long v = strtol(hex, &end, 16);
            if (end && *end == 0) {
                *w++ = (char)v;
                r += 2;
                continue;
            }
        }
        *w++ = *r;
    }
    *w = 0;
}

/* 取查询串（调用方给缓冲） */
static bool query_str(httpd_req_t *req, const char *key, char *out, size_t len)
{
    char q[256];
    if (httpd_req_get_url_query_len(req) == 0) return false;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return false;
    if (httpd_query_key_value(q, key, out, len) != ESP_OK) return false;
    url_decode(out);
    return true;
}

/* 往数组里加一首歌（多种接口共用同一形状，前端只写一套渲染） */
static void add_song(cJSON *arr, int i, const sub_song_t *s, int playing_index)
{
    cJSON *o = cJSON_CreateObject();
    if (i >= 0) cJSON_AddNumberToObject(o, "i", i);
    cJSON_AddStringToObject(o, "id", s->id);
    cJSON_AddStringToObject(o, "title", s->title);
    cJSON_AddStringToObject(o, "artist", s->artist);
    cJSON_AddStringToObject(o, "album", s->album);
    cJSON_AddStringToObject(o, "cover", s->cover);
    cJSON_AddNumberToObject(o, "dur_s", s->duration_s);
    if (s->starred) cJSON_AddBoolToObject(o, "starred", true);
    if (i >= 0 && i == playing_index) cJSON_AddBoolToObject(o, "playing", true);
    cJSON_AddItemToArray(arr, o);
}

/* ============================================================ 页面本体 */

static esp_err_t h_index(httpd_req_t *req)
{
    asset_t a;
    if (!assets_get_by_name("index.html.gz", &a) || a.fmt != ASSET_FMT_WEB_GZ) {
        return fail(req, "503 Service Unavailable",
                    "网页资源没烧进 assets 分区（跑 tools/gen_assets.py 后 idf.py flash）");
    }
    /* 浏览器声明支持 gzip 才发压缩体（手机上都会声明） */
    char enc[32];
    bool gz = false;
    if (httpd_req_get_hdr_value_str(req, "Accept-Encoding", enc, sizeof(enc)) == ESP_OK
        && strstr(enc, "gzip")) {
        gz = true;
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    }
    no_keepalive(req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    if (gz) {
        /* ★ mmap 的指针直接发给 socket，零拷贝（httpd 内部会分块发） */
        return httpd_resp_send(req, (const char *)a.data, a.size);
    }
    return fail(req, "406 Not Acceptable", "请用支持 gzip 的浏览器");
}

/* ============================================================ 状态 */

static esp_err_t h_status(httpd_req_t *req)
{
    player_status_t st;
    player_status_get(&st);
    ae_state_t aes = audio_engine_state();

    const char *state = "IDLE";
    switch (aes) {
        case AE_STATE_BUFFERING: state = "BUFFERING"; break;
        case AE_STATE_PLAYING:   state = "PLAYING";   break;
        case AE_STATE_FINISHED:  state = "FINISHED";  break;
        case AE_STATE_ERROR:     state = "ERROR";     break;
        default:                 state = "IDLE";      break;
    }
    if (audio_engine_is_paused()) state = "PAUSED";

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "state", state);
    cJSON_AddNumberToObject(o, "pos_ms", audio_engine_position_ms());
    cJSON_AddNumberToObject(o, "dur_ms", st.duration_ms);
    /* ⚠️ 暂停时引擎状态仍是 PLAYING（暂停是独立标志），所以 playing 要额外排除暂停。
     *    否则网页会显示"播放中"却不走时间。前端自己也算了一层 && !paused 做兜底，
     *    但契约本身该是对的。 */
    cJSON_AddBoolToObject(o, "playing", aes == AE_STATE_PLAYING && !audio_engine_is_paused());
    cJSON_AddBoolToObject(o, "paused", audio_engine_is_paused());
    cJSON_AddNumberToObject(o, "vol", player_volume_get());
    cJSON_AddBoolToObject(o, "muted", player_is_muted());
    cJSON_AddNumberToObject(o, "bright", player_brightness());
    cJSON_AddNumberToObject(o, "gen", st.gen);      /* ★ 每次换歌 +1，前端靠它对账 */

    cJSON *song = cJSON_AddObjectToObject(o, "song");
    cJSON_AddStringToObject(song, "id", st.song_id);
    cJSON_AddStringToObject(song, "title", st.title);
    cJSON_AddStringToObject(song, "artist", st.artist);
    cJSON_AddStringToObject(song, "album", st.album);
    cJSON_AddStringToObject(song, "cover", st.cover_id);
    cJSON_AddBoolToObject(song, "starred", st.starred);

    cJSON *q = cJSON_AddObjectToObject(o, "queue");
    cJSON_AddNumberToObject(q, "index", player_queue_index());
    cJSON_AddNumberToObject(q, "count", player_queue_count());
    cJSON_AddStringToObject(q, "source", player_src_name(player_source()));
    cJSON_AddNumberToObject(q, "gen", player_queue_gen());

    /* M6：收藏镜像的状态。前端靠 starred.gen 变化决定要不要重拉 /api/starred
     * （收藏页现在是本地镜像立刻返回 + 后台对服务器）。 */
    cJSON *star = cJSON_AddObjectToObject(o, "starred");
    bool star_synced = false;
    player_starred_list(NULL, 0, &star_synced);
    int64_t star_age = player_starred_age_ms();
    cJSON_AddNumberToObject(star, "gen", player_starred_gen());
    cJSON_AddNumberToObject(star, "pending", player_starred_pending());
    cJSON_AddBoolToObject(star, "synced", star_synced);
    cJSON_AddNumberToObject(star, "age_s", star_age < 0 ? -1 : (double)(star_age / 1000));

    cJSON *net = cJSON_AddObjectToObject(o, "net");
    cJSON_AddBoolToObject(net, "connected", net_mgr_is_connected());
    cJSON_AddStringToObject(net, "ip", net_mgr_ip_str());
    cJSON_AddStringToObject(net, "ssid", net_mgr_ssid());
    cJSON_AddNumberToObject(net, "rssi", net_mgr_rssi());
    cJSON_AddBoolToObject(net, "ap_on", net_mgr_ap_is_on());
    cJSON_AddStringToObject(net, "ap_ssid", net_mgr_ap_ssid());
    cJSON_AddStringToObject(net, "ap_pass", net_mgr_ap_pass());

    cJSON *fw = cJSON_AddObjectToObject(o, "fw");
    cJSON_AddNumberToObject(fw, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddStringToObject(fw, "idf", esp_get_idf_version());

    /* M7：蓝牙遥控状态（网页上要能看"连上了没 / 开关是开还是关"） */
    cJSON *bt = cJSON_AddObjectToObject(o, "hid");
    cJSON_AddBoolToObject(bt, "available", hid_remote_available());
    cJSON_AddBoolToObject(bt, "connected", hid_remote_connected());
    cJSON_AddBoolToObject(bt, "mode", hid_remote_mode());
    cJSON_AddStringToObject(bt, "state", hid_remote_state_str());
    cJSON_AddStringToObject(bt, "peer", hid_remote_peer_name());

    return send_json(req, o);
}

/* ============================================================ 队列 / 列表 */

static esp_err_t h_queue(httpd_req_t *req)
{
    int from = 0, n = 50;
    query_int(req, "from", &from);
    query_int(req, "n", &n);
    if (n <= 0 || n > 64) n = 50;

    int total = player_queue_count();
    int play  = player_queue_index();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "total", total);
    cJSON_AddNumberToObject(o, "from", from);
    cJSON *items = cJSON_AddArrayToObject(o, "items");
    sub_song_t s;
    for (int i = from; i < total && i < from + n; i++) {
        if (player_queue_get(i, &s)) add_song(items, i, &s, play);
    }
    return send_json(req, o);
}

static esp_err_t h_recent(httpd_req_t *req)
{
    int total = player_recent_count();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "total", total);
    cJSON *items = cJSON_AddArrayToObject(o, "items");
    sub_song_t s;
    for (int i = 0; i < total; i++) {
        if (player_recent_get(i, &s)) add_song(items, i, &s, -1);
    }
    return send_json(req, o);
}

/* 收藏列表 —— M6 起**直接读本地镜像**（毫秒级返回），同时请求后台跟服务器对一次。
 *
 * 为什么改：以前这里每次现拉 getStarred2（几百毫秒），而且**没网时整个接口失败**，
 * 收藏页就空了。现在不管有没有网都能看到上次的列表；对完服务器后 gen 会变，
 * 前端在 /api/status 里看到 gen 变了会自动重拉（见 index.html 的 applyStatus）。
 * 想强制马上对一次就带 ?refresh=1（前端"我的收藏"页的下拉刷新用）。 */
static esp_err_t h_starred(httpd_req_t *req)
{
    int force = 0;
    query_int(req, "refresh", &force);

    sub_song_t *songs = heap_caps_malloc(PLAYER_STAR_MAX * sizeof(sub_song_t), MALLOC_CAP_SPIRAM);
    if (songs == NULL) return fail(req, "500 Internal Server Error", "内存不足");
    bool synced = false;
    int got = player_starred_list(songs, PLAYER_STAR_MAX, &synced);

    cJSON *o = cJSON_CreateObject();
    cJSON *items = cJSON_AddArrayToObject(o, "items");
    for (int i = 0; i < got; i++) add_song(items, -1, &songs[i], -1);
    cJSON_AddNumberToObject(o, "total", got);
    cJSON_AddBoolToObject(o, "cached", true);        /* 这份来自本地镜像，不是现拉的 */
    cJSON_AddBoolToObject(o, "synced", synced);      /* 已确认与服务器一致 */
    cJSON_AddNumberToObject(o, "pending", player_starred_pending());
    cJSON_AddNumberToObject(o, "gen", player_starred_gen());
    heap_caps_free(songs);

    player_starred_refresh(force != 0);   /* 异步：这次请求不等网络 */
    return send_json(req, o);
}

static esp_err_t h_search(httpd_req_t *req)
{
    char q[128];
    if (!query_str(req, "q", q, sizeof(q)) || q[0] == 0) {
        return fail(req, "400 Bad Request", "缺少 q 参数");
    }
    sub_song_t *songs = heap_caps_malloc(SUB_SONGS_MAX * sizeof(sub_song_t), MALLOC_CAP_SPIRAM);
    if (songs == NULL) return fail(req, "500 Internal Server Error", "内存不足");
    int got = 0;
    esp_err_t err = subsonic_search(q, songs, SUB_SONGS_MAX, &got);
    cJSON *o = cJSON_CreateObject();
    cJSON *items = cJSON_AddArrayToObject(o, "items");
    for (int i = 0; i < got; i++) add_song(items, -1, &songs[i], -1);
    cJSON_AddNumberToObject(o, "total", got);
    heap_caps_free(songs);
    if (err != ESP_OK && got == 0) {
        cJSON_Delete(o);
        return fail(req, "502 Bad Gateway", "搜索失败（Navidrome 不可达？）");
    }
    return send_json(req, o);
}

static esp_err_t h_sources(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "current", (int)player_source());
    cJSON_AddStringToObject(o, "current_name", player_src_name(player_source()));
    cJSON *items = cJSON_AddArrayToObject(o, "items");
    for (int i = 0; i < PLAYER_SRC_COUNT; i++) {
        cJSON *it = cJSON_CreateObject();
        cJSON_AddNumberToObject(it, "i", i);
        cJSON_AddStringToObject(it, "name", player_src_name((player_src_t)i));
        cJSON_AddItemToArray(items, it);
    }
    return send_json(req, o);
}

/* ============================================================ 封面代理
 *
 * 为什么要代理：AP 模式下手机根本没有到 Navidrome 的路由；
 * 而且直接让浏览器去请求会把 Navidrome 凭据暴露出去。
 */
static esp_err_t h_cover(httpd_req_t *req)
{
    char id[64] = {0};
    if (!query_str(req, "id", id, sizeof(id)) || id[0] == 0) {
        return fail(req, "400 Bad Request", "缺少 id 参数");
    }
    int size = 144;
    query_int(req, "size", &size);

    /* ★ 转发的是【原始 JPEG 字节】，不是 RGB565 —— 浏览器不认 RGB565。
     *   顺带也做了格式校验（Navidrome 对 WebP 封面转不了码，会原样透传）。 */
    uint8_t *jpg = NULL;
    size_t len = 0;
    esp_err_t err = subsonic_get_cover_raw(id, size, &jpg, &len);
    if (err != ESP_OK) {
        /* 404 很常见（WebP 封面），前端会退到占位图，不要刷错误日志 */
        return fail(req, "404 Not Found", "取不到封面");
    }
    no_keepalive(req);
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");
    esp_err_t e = httpd_resp_send(req, (const char *)jpg, len);
    heap_caps_free(jpg);
    return e;
}

/* ============================================================ 歌词 */
static esp_err_t h_lyrics(httpd_req_t *req)
{
    char id[64] = {0};
    if (!query_str(req, "id", id, sizeof(id)) || id[0] == 0) {
        return fail(req, "400 Bad Request", "缺少 id 参数");
    }
    char *json = NULL;
    esp_err_t err = subsonic_get_lyrics(id, &json);
    if (err != ESP_OK || json == NULL) {
        return fail(req, "502 Bad Gateway", "取歌词失败（Navidrome 不可达？）");
    }
    /* subsonic_get_lyrics 给的就是可以直接发的 JSON（没歌词时是空 lines + 200） */
    no_keepalive(req);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_sendstr(req, json);
    heap_caps_free(json);
    return e;
}

/* ============================================================ 控制命令 */

static esp_err_t h_play(httpd_req_t *req)
{
    cJSON *b = read_body(req);
    if (b == NULL) return fail(req, "400 Bad Request", "请求体不是合法 JSON");
    int i = json_int(b, "i", -2);
    const char *id = json_str(b, "id");
    if (i == -2 && id == NULL) {
        cJSON_Delete(b);
        return fail(req, "400 Bad Request", "要带 i 或 id");
    }
    if (id && id[0]) {
        /* 按歌曲 id 找：先在队列里找，找不到就让 player 处理 */
        int count = player_queue_count();
        sub_song_t s;
        int found = -1;
        for (int k = 0; k < count; k++) {
            if (player_queue_get(k, &s) && strcmp(s.id, id) == 0) { found = k; break; }
        }
        if (found >= 0) {
            player_play_index(found);
        } else {
            /* ★ 前端把列表里已有的元数据一起 POST 上来了（title/artist/cover/dur_s…），
             *   那就直接用，**不再向 Navidrome 请求一次 getSong** ——
             *   那一次往返实测要 200~800ms，正是"点一下要顿一下"的另一半原因。
             *   缺字段时 player 会自己回退去请求（慢但不坏）。 */
            cJSON *jt = cJSON_GetObjectItem(b, "title");
            if (jt && cJSON_IsString(jt) && jt->valuestring[0]) {
                sub_song_t m;
                memset(&m, 0, sizeof(m));
                strlcpy(m.id, id, sizeof(m.id));
                #define CP(field, key) do { \
                    cJSON *it = cJSON_GetObjectItem(b, key); \
                    if (it && cJSON_IsString(it)) strlcpy(m.field, it->valuestring, sizeof(m.field)); \
                } while (0)
                CP(title, "title");
                CP(artist, "artist");
                CP(album, "album");
                CP(cover, "cover");
                #undef CP
                cJSON *jd = cJSON_GetObjectItem(b, "dur_s");
                if (jd && cJSON_IsNumber(jd)) m.duration_s = (uint32_t)jd->valuedouble;
                cJSON *js = cJSON_GetObjectItem(b, "starred");
                if (js) m.starred = cJSON_IsTrue(js);
                player_play_song(&m);
            } else {
                player_play_song_id(id);
            }
        }
    } else {
        player_play_index(i);
    }
    cJSON_Delete(b);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    return send_json(req, o);
}

static esp_err_t ok_json(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    return send_json(req, o);
}

static esp_err_t h_toggle(httpd_req_t *req) { player_toggle(); return ok_json(req); }
static esp_err_t h_next(httpd_req_t *req)   { player_next();   return ok_json(req); }
static esp_err_t h_prev(httpd_req_t *req)   { player_prev();   return ok_json(req); }

static esp_err_t h_seek(httpd_req_t *req)
{
    cJSON *b = read_body(req);
    if (b == NULL) return fail(req, "400 Bad Request", "请求体不是合法 JSON");
    int ms = json_int(b, "pos_ms", -1);
    cJSON_Delete(b);
    if (ms < 0) return fail(req, "400 Bad Request", "缺少 pos_ms");
    player_seek(ms);
    return ok_json(req);
}

static esp_err_t h_volume(httpd_req_t *req)
{
    cJSON *b = read_body(req);
    if (b == NULL) return fail(req, "400 Bad Request", "请求体不是合法 JSON");
    int v = json_int(b, "vol", -1);
    cJSON_Delete(b);
    if (v < 0 || v > 100) return fail(req, "400 Bad Request", "vol 要在 0..100");
    player_volume(v);
    return ok_json(req);
}

static esp_err_t h_mute(httpd_req_t *req)
{
    cJSON *b = read_body(req);
    if (b == NULL) return fail(req, "400 Bad Request", "请求体不是合法 JSON");
    bool on = json_bool(b, "on", true);
    cJSON_Delete(b);
    player_set_mute(on);
    return ok_json(req);
}

static esp_err_t h_bright(httpd_req_t *req)
{
    cJSON *b = read_body(req);
    if (b == NULL) return fail(req, "400 Bad Request", "请求体不是合法 JSON");
    int v = json_int(b, "bright", -1);
    cJSON_Delete(b);
    if (v < 5 || v > 100) return fail(req, "400 Bad Request", "bright 要在 5..100");
    player_set_brightness(v);
    return ok_json(req);
}

static esp_err_t h_source(httpd_req_t *req)
{
    cJSON *b = read_body(req);
    if (b == NULL) return fail(req, "400 Bad Request", "请求体不是合法 JSON");
    int i = json_int(b, "i", -1);
    bool autoplay = json_bool(b, "autoplay", true);
    cJSON_Delete(b);
    if (i < 0 || i >= PLAYER_SRC_COUNT) return fail(req, "400 Bad Request", "来源下标越界");
    player_set_source((player_src_t)i, autoplay);
    return ok_json(req);
}

static esp_err_t h_star(httpd_req_t *req)
{
    cJSON *b = read_body(req);
    if (b == NULL) return fail(req, "400 Bad Request", "请求体不是合法 JSON");
    const char *id = json_str(b, "id");
    bool on = json_bool(b, "on", true);
    char idbuf[64] = {0};
    if (id && id[0]) strlcpy(idbuf, id, sizeof(idbuf));
    cJSON_Delete(b);

    /* 不给 id = 操作当前正在放的那首 */
    if (idbuf[0] == 0) {
        player_star(on);
    } else {
        player_star_song(idbuf, on);
    }
    return ok_json(req);
}

/* ============================================================ 配置 */

static esp_err_t h_config_get(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "host", subsonic_host());
    /* ⚠️ 永远不回密码，只回"设没设" */
    cJSON_AddStringToObject(o, "transcode", "mp3");
    cJSON_AddNumberToObject(o, "max_kbps", 192);
    cJSON_AddBoolToObject(o, "configured", subsonic_configured());
    cJSON_AddBoolToObject(o, "pass_set", subsonic_has_password());
    return send_json(req, o);
}

static esp_err_t h_config_post(httpd_req_t *req, bool save)
{
    cJSON *b = read_body(req);
    if (b == NULL) return fail(req, "400 Bad Request", "请求体不是合法 JSON");
    const char *host = json_str(b, "host");
    const char *user = json_str(b, "user");
    const char *pass = json_str(b, "pass");
    const char *tc   = json_str(b, "transcode");
    int kb = json_int(b, "max_kbps", 0);

    char hostbuf[160] = {0}, userbuf[48] = {0}, passbuf[64] = {0}, tcbuf[8] = {0};
    if (host) strlcpy(hostbuf, host, sizeof(hostbuf));
    if (user) strlcpy(userbuf, user, sizeof(userbuf));
    if (pass) strlcpy(passbuf, pass, sizeof(passbuf));
    if (tc)   strlcpy(tcbuf, tc, sizeof(tcbuf));
    cJSON_Delete(b);

    if (hostbuf[0] == 0) return fail(req, "400 Bad Request", "服务器地址不能为空");

    char info[160] = {0};
    esp_err_t err = subsonic_test_config(hostbuf, userbuf, passbuf, info, sizeof(info));
    if (err != ESP_OK) {
        char msg[224];
        snprintf(msg, sizeof(msg), "连不上这台服务器：%s", info[0] ? info : esp_err_to_name(err));
        return fail(req, "400 Bad Request", msg);
    }
    if (!save) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "ok", true);
        cJSON_AddStringToObject(o, "server", info);
        return send_json(req, o);
    }
    err = subsonic_set_config(hostbuf, userbuf, passbuf,
                              tcbuf[0] ? tcbuf : NULL, kb > 0 ? kb : 0);
    if (err != ESP_OK) return fail(req, "500 Internal Server Error", "保存失败（NVS 写不进去？）");
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "server", info);
    return send_json(req, o);
}

static esp_err_t h_config_put(httpd_req_t *req)  { return h_config_post(req, true); }
static esp_err_t h_config_test(httpd_req_t *req) { return h_config_post(req, false); }

/* ============================================================ WiFi */

static esp_err_t h_wifi(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "connected", net_mgr_is_connected());
    cJSON_AddStringToObject(o, "ssid", net_mgr_ssid());
    cJSON_AddStringToObject(o, "ip", net_mgr_ip_str());
    cJSON_AddNumberToObject(o, "rssi", net_mgr_rssi());
    cJSON_AddBoolToObject(o, "ap_on", net_mgr_ap_is_on());
    cJSON_AddStringToObject(o, "ap_ssid", net_mgr_ap_ssid());
    cJSON_AddStringToObject(o, "ap_pass", net_mgr_ap_pass());
    return send_json(req, o);
}

static esp_err_t h_wifi_scan(httpd_req_t *req)
{
    esp_err_t err = net_mgr_scan_start();
    cJSON *o = cJSON_CreateObject();
    if (err == ESP_ERR_INVALID_STATE) {
        cJSON_AddBoolToObject(o, "scanning", true);
        cJSON_AddStringToObject(o, "note", "已经在扫了");
    } else if (err != ESP_OK) {
        cJSON_Delete(o);
        return fail(req, "500 Internal Server Error", "发起扫描失败");
    } else {
        cJSON_AddBoolToObject(o, "scanning", true);
    }
    return send_json(req, o);
}

static esp_err_t h_wifi_scan_result(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    bool busy = net_mgr_scan_busy();
    cJSON_AddBoolToObject(o, "scanning", busy);
    cJSON *items = cJSON_AddArrayToObject(o, "items");
    if (!busy) {
        int n = net_mgr_scan_count();
        bool marked = false;      /* 只把第一个同名的标成"当前"，别标一片 */
        for (int i = 0; i < n; i++) {
            char ssid[33] = {0};
            int rssi = 0;
            const char *auth = "?";
            if (!net_mgr_scan_get(i, ssid, sizeof(ssid), &rssi, &auth)) continue;
            cJSON *it = cJSON_CreateObject();
            cJSON_AddStringToObject(it, "ssid", ssid);
            cJSON_AddNumberToObject(it, "rssi", rssi);
            cJSON_AddStringToObject(it, "auth", auth);
            bool cur = !marked && strcmp(ssid, net_mgr_ssid()) == 0;
            if (cur) marked = true;
            cJSON_AddBoolToObject(it, "current", cur);
            cJSON_AddItemToArray(items, it);
        }
    }
    return send_json(req, o);
}

static esp_err_t h_wifi_set(httpd_req_t *req)
{
    cJSON *b = read_body(req);
    if (b == NULL) return fail(req, "400 Bad Request", "请求体不是合法 JSON");
    const char *ssid = json_str(b, "ssid");
    const char *pass = json_str(b, "pass");
    char sbuf[64] = {0}, pbuf[64] = {0};
    if (ssid) strlcpy(sbuf, ssid, sizeof(sbuf));
    if (pass) strlcpy(pbuf, pass, sizeof(pbuf));
    cJSON_Delete(b);

    esp_err_t err = net_mgr_set_sta(sbuf, pbuf[0] ? pbuf : NULL);
    if (err != ESP_OK) return fail(req, "400 Bad Request", "SSID 不合法或 WiFi 没起来");
    return ok_json(req);
}

static esp_err_t h_ap(httpd_req_t *req)
{
    cJSON *b = read_body(req);
    if (b == NULL) return fail(req, "400 Bad Request", "请求体不是合法 JSON");
    bool on = json_bool(b, "on", net_mgr_ap_is_on());
    const char *ssid = json_str(b, "ssid");
    const char *pass = json_str(b, "pass");
    char sbuf[64] = {0}, pbuf[64] = {0};
    if (ssid) strlcpy(sbuf, ssid, sizeof(sbuf));
    if (pass) strlcpy(pbuf, pass, sizeof(pbuf));
    cJSON_Delete(b);

    esp_err_t err = net_mgr_ap_apply(on, sbuf[0] ? sbuf : NULL, pbuf[0] ? pbuf : NULL);
    if (err != ESP_OK) return fail(req, "500 Internal Server Error", "热点设置失败");
    return ok_json(req);
}

/* ============================================================ 蓝牙遥控（M7） */

static void hid_fill(cJSON *o)
{
    cJSON_AddBoolToObject(o, "available", hid_remote_available());
    cJSON_AddBoolToObject(o, "connected", hid_remote_connected());
    cJSON_AddBoolToObject(o, "mode", hid_remote_mode());
    cJSON_AddBoolToObject(o, "active", hid_remote_active());
    cJSON_AddStringToObject(o, "state", hid_remote_state_str());
    cJSON_AddStringToObject(o, "peer", hid_remote_peer_name());
    cJSON_AddNumberToObject(o, "sent", hid_remote_sent_count());
}

static esp_err_t h_hid(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    hid_fill(o);
    cJSON_AddBoolToObject(o, "ok", true);
    return send_json(req, o);
}

static esp_err_t h_hid_set(httpd_req_t *req)
{
    cJSON *b = read_body(req);
    if (b == NULL) return fail(req, "400 Bad Request", "请求体不是合法 JSON");
    bool on = json_bool(b, "on", true);
    cJSON_Delete(b);

    if (on && !hid_remote_available()) {
        /* 说清楚为什么开不了：多半是内部 SRAM 不够（见 sdkconfig.forced） */
        return fail(req, "409 Conflict", "蓝牙栈没起来（内部 SRAM 不足？看 /api/debug）");
    }
    /* 进/出"蓝牙控制模式"：设备屏幕会跟着切到遥控界面（ui_pages_tick 跟随） */
    hid_remote_set_mode(on);
    cJSON *o = cJSON_CreateObject();
    hid_fill(o);
    cJSON_AddBoolToObject(o, "ok", true);
    return send_json(req, o);
}

/* 主动发一个媒体键给已配对的对端。
 * 用途有两个：① 网页上直接当遥控器使（不用碰设备上的键）；
 * ② **验证 HID 通道通不通** —— 不用真的去按按键，就能确认对端收得到。 */
static const struct { const char *name; hid_key_t key; } s_hid_keys[] = {
    { "play",  HID_KEY_PLAY_PAUSE }, { "next",  HID_KEY_NEXT },
    { "prev",  HID_KEY_PREV },       { "volup", HID_KEY_VOL_UP },
    { "voldn", HID_KEY_VOL_DOWN },   { "mute",  HID_KEY_MUTE },
    { "stop",  HID_KEY_STOP },       { "ff",    HID_KEY_FAST_FWD },
    { "rew",   HID_KEY_REWIND },
};

static esp_err_t h_hid_send(httpd_req_t *req)
{
    char name[16] = {0};
    if (!query_str(req, "key", name, sizeof(name))) {
        return fail(req, "400 Bad Request",
                    "要带 key=play|next|prev|volup|voldn|mute|stop|ff|rew");
    }
    for (size_t i = 0; i < sizeof(s_hid_keys) / sizeof(s_hid_keys[0]); i++) {
        if (strcmp(s_hid_keys[i].name, name) != 0) continue;
        if (!hid_remote_active()) {
            return fail(req, "409 Conflict",
                        "遥控没生效（要么没连上对端，要么没进蓝牙控制模式）");
        }
        hid_remote_send(s_hid_keys[i].key);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "ok", true);
        cJSON_AddStringToObject(o, "sent", name);
        hid_fill(o);
        return send_json(req, o);
    }
    return fail(req, "400 Bad Request", "不认识的 key");
}

/* ============================================================ 诊断 */

static esp_err_t h_debug(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "sram_kb", heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
    cJSON_AddNumberToObject(o, "psram_kb", heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
    cJSON_AddNumberToObject(o, "underruns", audio_engine_underruns());
    cJSON_AddNumberToObject(o, "i2c_retries", board_i2c_retries());
    cJSON_AddNumberToObject(o, "sample_rate", audio_engine_sample_rate());
    cJSON_AddNumberToObject(o, "ring_in_kb", audio_engine_ring_used_in() / 1024);
    cJSON_AddNumberToObject(o, "ring_pcm_kb", audio_engine_ring_used_pcm() / 1024);
    cJSON_AddNumberToObject(o, "settings_pending", settings_pending_count());
    cJSON_AddNumberToObject(o, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddStringToObject(o, "format", audio_engine_format_str());

    /* M6：cache 分区（最近播放/收藏/封面缓存）*/
    cJSON *fs = cJSON_AddObjectToObject(o, "cache_fs");
    cJSON_AddBoolToObject(fs, "ready", store_ready());
    cJSON_AddNumberToObject(fs, "free_kb", (double)store_free_kb());
    cJSON_AddNumberToObject(fs, "covers", store_dir_count("COVER"));
    cJSON_AddNumberToObject(fs, "writes", store_write_count());
    cJSON_AddNumberToObject(fs, "fails", store_fail_count());
    cJSON_AddNumberToObject(fs, "last_errno", store_last_errno());
    cJSON_AddNumberToObject(fs, "recent", player_recent_count());

    cJSON *bt = cJSON_AddObjectToObject(o, "hid");
    hid_fill(bt);
    return send_json(req, o);
}

/* ============================================================ 注册 */

esp_err_t web_console_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    /* ⚠️ 每个 socket 都要占 lwIP 缓冲，而这个芯片内部 SRAM 只剩二十几 KB。
     * 4 个够用（status 轮询 + 列表 + 封面），比默认 7 省一截。 */
    /* ⚠️ 内部 SRAM 在起播后只剩个位数 KB（解码器是预编译库、内存挪不到 PSRAM），
     * 所以这里每一项都往小里配：
     *   - socket 数：每个连接要一份 header 缓冲（HTTPD_MAX_REQ_HDR_LEN）+ 请求结构
     *   - 栈：JSON 拼装 + cJSON 全在 PSRAM，4KB 够
     * 配合 sdkconfig.defaults 里把 HTTPD_MAX_REQ_HDR_LEN / MAX_URI_LEN 调小。 */
    cfg.max_open_sockets   = 6;
    cfg.lru_purge_enable   = true;      /* 满了就踢最久没动的连接，别拒绝新请求 */
    cfg.stack_size         = 4096;
    cfg.max_uri_handlers   = 32;
    cfg.recv_wait_timeout  = 5;
    cfg.send_wait_timeout  = 5;

    httpd_handle_t srv = NULL;
    esp_err_t err = httpd_start(&srv, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd 启动失败: %s（是不是 SRAM 不够？当前 %u KB）",
                 esp_err_to_name(err), (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
        return err;
    }

    static const httpd_uri_t uris[] = {
        { .uri = "/",                    .method = HTTP_GET,  .handler = h_index },
        { .uri = "/api/status",          .method = HTTP_GET,  .handler = h_status },
        { .uri = "/api/lyrics",          .method = HTTP_GET,  .handler = h_lyrics },
        { .uri = "/api/queue",           .method = HTTP_GET,  .handler = h_queue },
        { .uri = "/api/recent",          .method = HTTP_GET,  .handler = h_recent },
        { .uri = "/api/starred",         .method = HTTP_GET,  .handler = h_starred },
        { .uri = "/api/search",          .method = HTTP_GET,  .handler = h_search },
        { .uri = "/api/sources",         .method = HTTP_GET,  .handler = h_sources },
        { .uri = "/api/cover",           .method = HTTP_GET,  .handler = h_cover },
        { .uri = "/api/config",          .method = HTTP_GET,  .handler = h_config_get },
        { .uri = "/api/config",          .method = HTTP_POST, .handler = h_config_put },
        { .uri = "/api/config/test",     .method = HTTP_POST, .handler = h_config_test },
        { .uri = "/api/wifi",            .method = HTTP_GET,  .handler = h_wifi },
        { .uri = "/api/wifi",            .method = HTTP_POST, .handler = h_wifi_set },
        { .uri = "/api/wifi/scan",       .method = HTTP_GET,  .handler = h_wifi_scan },
        { .uri = "/api/wifi/scan/result",.method = HTTP_GET,  .handler = h_wifi_scan_result },
        { .uri = "/api/ap",              .method = HTTP_POST, .handler = h_ap },
        { .uri = "/api/debug",           .method = HTTP_GET,  .handler = h_debug },
        { .uri = "/api/play",            .method = HTTP_POST, .handler = h_play },
        { .uri = "/api/toggle",          .method = HTTP_POST, .handler = h_toggle },
        { .uri = "/api/next",            .method = HTTP_POST, .handler = h_next },
        { .uri = "/api/prev",            .method = HTTP_POST, .handler = h_prev },
        { .uri = "/api/seek",            .method = HTTP_POST, .handler = h_seek },
        { .uri = "/api/volume",          .method = HTTP_POST, .handler = h_volume },
        { .uri = "/api/mute",            .method = HTTP_POST, .handler = h_mute },
        { .uri = "/api/bright",          .method = HTTP_POST, .handler = h_bright },
        { .uri = "/api/source",          .method = HTTP_POST, .handler = h_source },
        { .uri = "/api/star",            .method = HTTP_POST, .handler = h_star },
        { .uri = "/api/hid",             .method = HTTP_GET,  .handler = h_hid },
        { .uri = "/api/hid",             .method = HTTP_POST, .handler = h_hid_set },
        { .uri = "/api/hid/send",        .method = HTTP_GET,  .handler = h_hid_send },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        esp_err_t e = httpd_register_uri_handler(srv, &uris[i]);
        if (e != ESP_OK) ESP_LOGE(TAG, "注册 %s 失败: %s", uris[i].uri, esp_err_to_name(e));
    }

    ESP_LOGW(TAG, "网页控制台已启动: http://%s/ （热点 http://192.168.4.1/，%u 个接口，无鉴权）",
             net_mgr_ip_str(), (unsigned)(sizeof(uris) / sizeof(uris[0])));
    return ESP_OK;
}
