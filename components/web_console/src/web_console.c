/*
 * 网页控制台 —— REST API 实现
 *
 * 分层约定：这里只调各模块的公开接口（player_/subsonic_/net_mgr_/settings_），
 * 不碰 LVGL、不碰音频引擎内部。所以"网页点下一首"和"按键按下一首"走的是同一条路。
 */
#include <string.h>
#include <strings.h>   /* strncasecmp：判断地址有没有带 http:// */
#include <stdlib.h>
#include <inttypes.h>
#include "web_console.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "assets.h"
#include "usblink.h"
#include "player_remote.h"
#include "app_core.h"
#include "audio_engine.h"
#include "player.h"
#include "subsonic.h"
#include "qqmusic.h"
#include "net_mgr.h"
#include "settings.h"
#include "store.h"
#include "hid_remote.h"
#include "board.h"

static const char *TAG = "web";

/* 实现在下面「RTC 黑匣子」那一节，这里先声明，因为 /api/debug 要用 */
static const char *reset_reason_name(esp_reset_reason_t r);

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
    /* ⚠️⚠️ 这里的缓冲原来是 **32 字节**，踩过一个极隐蔽的坑：
     *   `httpd_req_get_url_query_str()` 在缓冲不够时会**静默截断**（不报错），
     *   于是超长的查询串里，**排在后面的参数直接消失**，取不到就用调用方的默认值 ——
     *   症状是"这个参数改了没用"，而且毫无提示。
     *
     *   实际踩到的：封面请求 `/api/cover?id=<14位mid>&size=144&back=1` = **33 字节**，
     *   只超一个字节 → 截断成 `...&back`（没有 `=1`）→ back 取到 0 →
     *   设备去 Navidrome 取 QQ 的封面 → 404。**同时**歌词请求只有 25 字节，
     *   能塞下 → 歌词是好的。于是现象变成"歌词有了、封面没有"，
     *   加上屏幕走的是内部调用（不经过 HTTP query）也正常 —— 三条线索互相矛盾，极难查。
     *
     *   教训：**这种"边界差一个字节"的参数解析，宁可用大缓冲**。
     *   现在是 192 字节（够放 id + size + back 这类组合），栈上多这 160 字节无所谓。 */
    char val[192];
    if (httpd_req_get_url_query_len(req) == 0) return false;
    if (httpd_req_get_url_query_str(req, val, sizeof(val)) != ESP_OK) return false;
    char got[64];
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
    /* ★ suffix / size 必须给前端 —— 它在"点歌时把元数据一起 POST 上来"的快路径里要用。
     *   少了这个，设备端拿 POST 上来的字段建的队列项**后缀是空的**，于是被判成
     *   "解不了这个格式" → 一律转码。症状极迷惑：**从网页点歌永远是 MP3，从屏幕点歌
     *   却是无损**（屏幕走队列，队列里的对象是从服务器解析来的，后缀是全的）。 */
    if (s->suffix[0]) cJSON_AddStringToObject(o, "suffix", s->suffix);
    if (s->size)      cJSON_AddNumberToObject(o, "size", (double)s->size);
    if (s->starred) cJSON_AddBoolToObject(o, "starred", true);
    /* ★ 这首歌属于哪个后端（sub_back_t）。前端拿封面/歌词时要把这个带上 ——
     *   Navidrome 的 cover 是 coverArt id，QQ 的是**专辑 mid**，
     *   同一个字段、两条完全不同的取图路子。不带给后端的话 QQ 歌就没封面/没歌词。 */
    cJSON_AddNumberToObject(o, "back", s->back);
    if (i >= 0 && i == playing_index) cJSON_AddBoolToObject(o, "playing", true);
    cJSON_AddItemToArray(arr, o);
}

/* ============================================================ 页面本体 */

/* ★★ 网页现在**内嵌在 app 里**（由 tools/gen_assets.py 生成这个 C 数组）。
 *
 * 为什么不继续从 assets 分区读（以前是这样）：
 *   assets 是**独立分区**，而 **OTA 只更新 app 分区** —— 实测踩到过：
 *   OTA 上传成功、槽也换了，但页面一点没变（网页还在 assets 里）。
 *   把网页编进 app 之后，"改网页"也能跟着 OTA 走 —— 而网页恰恰是最常改的。
 *   字库/图标仍留在 assets 分区（几百 KB，且很少改）。
 *
 * 代价：app 体积 +几十 KB（gzip 后的 HTML），3MB 的槽装得下。
 * ⚠️ 内容是**生成**的，改网页要跑 tools/gen_assets.py 再编译。 */
extern const unsigned char web_index_gz[];
extern const unsigned int  web_index_gz_len;

static esp_err_t h_index(httpd_req_t *req)
{
    if (web_index_gz_len == 0) {
        return fail(req, "503 Service Unavailable",
                    "内嵌网页为空（跑 tools/gen_assets.py 再编译）");
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
        return httpd_resp_send(req, (const char *)web_index_gz, web_index_gz_len);
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

    /* ★ 设备=服务器时，本机引擎是停的（它不出声），状态全在 player_status 里
     *   （那是从服务器镜像过来的）。所以要换数据源，否则网页会一直显示 IDLE。 */
    bool remote = (player_out() == PLAYER_OUT_SERVER);
    if (remote) {
        state = st.playing ? "PLAYING" : (st.paused ? "PAUSED" : "IDLE");
    }

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "state", state);
    cJSON_AddNumberToObject(o, "pos_ms", remote ? st.position_ms : audio_engine_position_ms());
    cJSON_AddNumberToObject(o, "dur_ms", st.duration_ms);
    /* ★ 服务器模式：把服务器那边镜像过来的错误也带上。
     *   典型：USB DAC 被拔掉时服务器回 "USB DAC 已拔出（插上会自动继续）"，
     *   不带上网页只能显示"暂停"，用户会以为播放器坏了。 */
    if (remote) {
        remote_status_t rs;
        player_remote_status(&rs);
        cJSON_AddBoolToObject(o, "srv_online", rs.valid);
        cJSON_AddStringToObject(o, "srv_error", rs.err);
    }
    /* ⚠️ 暂停时引擎状态仍是 PLAYING（暂停是独立标志），所以 playing 要额外排除暂停。
     *    否则网页会显示"播放中"却不走时间。前端自己也算了一层 && !paused 做兜底，
     *    但契约本身该是对的。 */
    cJSON_AddBoolToObject(o, "playing",
                          remote ? (st.playing && !st.paused)
                                 : (aes == AE_STATE_PLAYING && !audio_engine_is_paused()));
    cJSON_AddBoolToObject(o, "paused", remote ? st.paused : audio_engine_is_paused());
    cJSON_AddNumberToObject(o, "vol", player_volume_get());
    cJSON_AddBoolToObject(o, "muted", player_is_muted());
    cJSON_AddNumberToObject(o, "bright", player_brightness());
    cJSON_AddNumberToObject(o, "gen", st.gen);      /* ★ 每次换歌 +1，前端靠它对账 */
    /* ★ 播放设备：0=本机（ESP 出声）/ 1=服务器（那台接 USB DAC 的机器出声）*/
    cJSON_AddNumberToObject(o, "out", (int)player_out());
    cJSON_AddStringToObject(o, "out_name", player_out_name(player_out()));

    cJSON *song = cJSON_AddObjectToObject(o, "song");
    cJSON_AddStringToObject(song, "id", st.song_id);
    cJSON_AddStringToObject(song, "title", st.title);
    cJSON_AddStringToObject(song, "artist", st.artist);
    cJSON_AddStringToObject(song, "album", st.album);
    cJSON_AddStringToObject(song, "cover", st.cover_id);
    cJSON_AddBoolToObject(song, "starred", st.starred);
    /* ★ 当前歌属于哪个后端 —— 前端拿封面/歌词时要带上（QQ 和 Navidrome 两条路） */
    cJSON_AddNumberToObject(song, "back", st.back);

    /* ★ 音频规格：解码格式 / 采样率 / 位深 / 是否直出 / 缓冲水位 / 欠载次数。
     *   屏幕播放页右栏已经有一份同样的信息，网页这边也要能看 ——
     *   排查"这首歌怎么变成 MP3 了"的时候，网页是最顺手的入口（不用盯串口）。
     *   ⚠️ raw 是"最近一次取流"的属性，不是"当前这首歌"的：断流续播会重新
     *      拼 URL，而续播带 timeOffset 必然落到转码，这个值会翻成 false。 */
    cJSON *au = cJSON_AddObjectToObject(o, "audio");
    cJSON_AddStringToObject(au, "fmt", audio_engine_format_str());
    cJSON_AddNumberToObject(au, "fs", audio_engine_sample_rate());
    cJSON_AddNumberToObject(au, "bits", audio_engine_bit_depth());
    /* ★ 源位深与实际输出位深是两个数，现在它们可以不一样 —— 这正是重点：
     *   bits     = 源文件位深（解码器报的）
     *   out_bits = 实际送进 I2S 的位深（16 / 24）
     * 24bit 源正常时两个都是 24；若 bits=24 而 out_bits=16，说明格式没切过去。 */
    cJSON_AddNumberToObject(au, "out_bits", audio_engine_out_bits());
    cJSON_AddBoolToObject(au, "raw", subsonic_last_raw());
    cJSON_AddNumberToObject(au, "ring_kb", audio_engine_ring_used_in() / 1024);
    /* 缓冲【已用/总量】（分母从引擎取，别在前端硬编码 512）*/
    size_t ring_cap = audio_engine_ring_in_cap();
    cJSON_AddNumberToObject(au, "ring_cap_kb", ring_cap / 1024);
    cJSON_AddNumberToObject(au, "ring_pct",
                            ring_cap ? (double)audio_engine_ring_used_in() * 100.0 / ring_cap : 0);
    /* 「听歌缓存」整体：所有为播放预留的缓冲里，眼下装了多少（前端显示百分率）*/
    cJSON_AddNumberToObject(au, "cache_used_kb", audio_engine_audio_cache_used() / 1024);
    cJSON_AddNumberToObject(au, "cache_cap_kb",  audio_engine_audio_cache_cap() / 1024);
    cJSON_AddNumberToObject(au, "cache_pct",
                            audio_engine_audio_cache_cap()
                              ? (double)audio_engine_audio_cache_used() * 100.0
                                / audio_engine_audio_cache_cap() : 0);
    cJSON_AddNumberToObject(au, "qual", subsonic_quality_current());   /* 当前音质挡位下标 */
    cJSON_AddNumberToObject(au, "underruns", audio_engine_underruns());

    cJSON *q = cJSON_AddObjectToObject(o, "queue");
    cJSON_AddNumberToObject(q, "index", player_queue_index());
    cJSON_AddNumberToObject(q, "count", player_queue_count());
    /* ⚠️ "source" 这个字段名是**老前端在用**的，现在它指的是「模式」（最近播放…/每日30首…）。
     *    真正的「来源」（后端）在下面 back/back_name。保留旧字段名以免老前端显示空白。 */
    cJSON_AddStringToObject(q, "source", player_mode_name(player_mode()));
    cJSON_AddNumberToObject(q, "mode", player_mode());
    cJSON_AddStringToObject(q, "mode_name", player_mode_name(player_mode()));
    cJSON_AddNumberToObject(q, "back", (int)player_back());
    cJSON_AddStringToObject(q, "back_name", player_back_name(player_back()));
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
    /* 「来源」（后端）和「模式」**分开报**，和屏幕上的菜单一一对应：
     *   来源 = 从哪个服务来（Navidrome / QQ 音乐）
     *   模式 = 这个服务下的哪类曲目（项数与名字**随后端变**：QQ 6 个 / Navidrome 3 个）
     * 老前端只认 items/current/current_name（那三个现在指的是"模式"），字段名保持兼容。 */
    cJSON_AddNumberToObject(o, "back", (int)player_back());
    cJSON_AddStringToObject(o, "back_name", player_back_name(player_back()));

    cJSON *backs = cJSON_AddArrayToObject(o, "backs");
    for (int i = 0; i < (int)PLAYER_BACK_COUNT; i++) {
        cJSON *it = cJSON_CreateObject();
        cJSON_AddNumberToObject(it, "i", i);
        cJSON_AddStringToObject(it, "name", player_back_name((player_back_t)i));
        cJSON_AddItemToArray(backs, it);
    }

    cJSON_AddNumberToObject(o, "current", player_mode());
    cJSON_AddStringToObject(o, "current_name", player_mode_name(player_mode()));
    cJSON *items = cJSON_AddArrayToObject(o, "items");
    int n = player_mode_count();
    for (int i = 0; i < n; i++) {
        cJSON *it = cJSON_CreateObject();
        cJSON_AddNumberToObject(it, "i", i);
        cJSON_AddStringToObject(it, "name", player_mode_name(i));
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
    /* ★ 后端分派：Navidrome 的封面走 /rest/getCoverArt；QQ 的是 y.qq.com 直链。
     *   前端把这首歌的 `back` 带上来（列表项里就有）。
     *   不带的话默认 0=Navidrome，保持老行为。 */
    int back = 0;
    query_int(req, "back", &back);

    /* ★ 转发的是【原始 JPEG 字节】，不是 RGB565 —— 浏览器不认 RGB565。
     *   顺带也做了格式校验（Navidrome 对 WebP 封面转不了码，会原样透传）。 */
    uint8_t *jpg = NULL;
    size_t len = 0;
    esp_err_t err;
    if (back == SUB_BACK_QQ) {
        char url[192];
        err = qqmusic_cover_url(id, size, url, sizeof(url));
        if (err == ESP_OK) err = subsonic_cover_raw_url(url, size, &jpg, &len);
    } else {
        err = subsonic_get_cover_raw(id, size, &jpg, &len);
    }
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
    /* ★ 后端分派（前端带上歌曲的 back）：QQ 的歌词要的是归一化 JSON，
     *   Navidrome 那边 subsonic_get_lyrics() 直接就给同一格式的 JSON。 */
    int back = 0;
    query_int(req, "back", &back);

    if (back == SUB_BACK_QQ) {
        char *json = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
        if (json == NULL) return fail(req, "500", "没有内存取歌词");
        esp_err_t qe = qqmusic_lyrics_json(id, json, 8192);
        if (qe != ESP_OK) {
            heap_caps_free(json);
            /* 没歌词不是错误：回一个空 lines 的 200，网页照常渲染"暂无歌词" */
            no_keepalive(req);
            httpd_resp_set_type(req, "application/json; charset=utf-8");
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            return httpd_resp_sendstr(req, "{\"synced\":false,\"total\":0,\"lines\":[]}");
        }
        no_keepalive(req);
        httpd_resp_set_type(req, "application/json; charset=utf-8");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        esp_err_t e = httpd_resp_sendstr(req, json);
        heap_caps_free(json);
        return e;
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
            /* ⚠️ 队列项可能是从【本地镜像】恢复来的，而镜像里的 suffix 可能是空的
             *    （实测踩到：网页点「我的收藏」的歌**全部变成 MP3**，而屏幕上点同一首
             *     却是无损 —— 因为屏幕走队列没这道坎，而后缀空 = 判不出格式 = 必然转码）。
             *    所以后缀空就宁可多一次回源往返，拿全的元数据。 */
            if (s.suffix[0]) player_play_index(found);
            else             player_play_song_id(id);
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
                CP(suffix, "suffix");          /* ★ 少了它就会被判成"解不了"→ 转码 */
                #undef CP
                cJSON *jd = cJSON_GetObjectItem(b, "dur_s");
                if (jd && cJSON_IsNumber(jd)) m.duration_s = (uint32_t)jd->valuedouble;
                cJSON *jz = cJSON_GetObjectItem(b, "size");
                if (jz && cJSON_IsNumber(jz)) m.size = (uint32_t)jz->valuedouble;
                cJSON *js = cJSON_GetObjectItem(b, "starred");
                if (js) m.starred = cJSON_IsTrue(js);
                /* ⚠️ 防御：万一日后又有字段漏传（比如浏览器缓存着旧版网页），
                 *   宁可贵一次回源往返（200~800ms），也别让后缀空着 ——
                 *   后缀空 = 判不出格式 = 必然转码，用户看到的就是"无损莫名变 MP3"。 */
                if (m.suffix[0]) player_play_song(&m);
                else             player_play_song_id(id);
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

/* 音质挡位：GET 拿当前值和全部挡位名，POST {"i":N} 设置。
 * 挡位表在 subsonic 层，和屏幕菜单用的是同一套 —— 不会出现两边不一致。
 * 立刻生效并存 NVS(ns "nav")，所以前端不用再点"应用"。 */
static esp_err_t h_quality(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "i", subsonic_quality_current());
        cJSON *a = cJSON_AddArrayToObject(o, "items");
        for (int i = 0; i < subsonic_quality_count(); i++) {
            cJSON_AddItemToArray(a, cJSON_CreateString(subsonic_quality_name(i)));
        }
        return send_json(req, o);
    }
    cJSON *b = read_body(req);
    if (b == NULL) return fail(req, "400 Bad Request", "请求体不是合法 JSON");
    int i = json_int(b, "i", -1);
    cJSON_Delete(b);
    if (subsonic_quality_set(i) != ESP_OK) return fail(req, "400 Bad Request", "挡位越界");
    return ok_json(req);
}

static esp_err_t h_source(httpd_req_t *req)
{
    cJSON *b = read_body(req);
    if (b == NULL) return fail(req, "400 Bad Request", "请求体不是合法 JSON");
    int i = json_int(b, "i", -1);
    bool autoplay = json_bool(b, "autoplay", true);
    cJSON_Delete(b);
    /* 「模式」的下标范围**随后端变**（QQ 6 个 / Navidrome 3 个），
     * 所以在这里按当前后端校验；player 任务里还会再校验一次（它才是唯一改状态的地方）。 */
    int n = player_mode_count();
    if (i < 0 || i >= n) return fail(req, "400 Bad Request", "模式下标越界");
    player_set_mode(i, autoplay);
    return ok_json(req);
}

/* 换「来源」= 换后端（Navidrome / QQ 音乐）。
 * ⚠️ 切换**不一定成功**（Navidrome 没配、QQ 服务连不上都会失败，player 会回退状态），
 *    所以这里只回"已下发"，真正的结果要看 /api/status 里的 back/back_name。 */
/* 清空封面缓存（POST /api/cache）。
 * ★ 只清**封面缓存** —— 它是纯可再生数据（下次看到那首歌会自动重新下载）。
 *   最近播放 / 收藏镜像**不清**：那里有用户的东西，清掉会莫名其妙地丢历史。
 * 返回清了多少、释放了多少，前端能给出真实反馈（而不是"点了没反应"）。 */
static esp_err_t h_cache(httpd_req_t *req)
{
    uint64_t before = store_free_kb();
    int n = store_evict("COVER", 0);            /* keep=0 = 全删 */
    uint64_t after = store_free_kb();
    ESP_LOGW(TAG, "清空封面缓存：删 %d 个，%u KB → %u KB 可用",
             n, (unsigned)before, (unsigned)after);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "removed", n);
    cJSON_AddNumberToObject(o, "freed_kb", (double)(after - before));
    cJSON_AddNumberToObject(o, "free_kb", (double)after);
    return send_json(req, o);
}

/* QQ 音乐契约层服务的地址（改完立刻生效并存 NVS，不用重刷固件）。
 * 为什么需要它：那个服务可能跑在别的机器/端口上，装机时不该要求重编固件。
 * 存进 NVS 后优先于 Kconfig 默认值（见 qqmusic_init）。 */
static esp_err_t h_qq(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "host", qqmusic_host());
        cJSON_AddBoolToObject(o, "configured", qqmusic_configured());
        /* 「收藏」跳过前 N 首（QQ 我喜欢里的置顶歌）—— 和设备上的设置同一个值 */
        cJSON_AddNumberToObject(o, "fav_skip", player_fav_skip());
        return send_json(req, o);
    }
    cJSON *b = read_body(req);
    if (b == NULL) return fail(req, "400 Bad Request", "请求体不是合法 JSON");
    const char *host = json_str(b, "host");
    /* ⚠️ 必须**先拷出来再 cJSON_Delete** —— json_str 返回的是树里的指针 */
    char buf[QQ_HOST_MAX] = {0};
    if (host != NULL) strlcpy(buf, host, sizeof(buf));
    /* fav_skip 可选：不传就只改地址（老调用方不会因为少个字段而失败） */
    const cJSON *fs = cJSON_GetObjectItem(b, "fav_skip");
    bool has_fs = cJSON_IsNumber(fs);
    int skip = has_fs ? (int)fs->valuedouble : 0;
    cJSON_Delete(b);
    if (has_fs) player_set_fav_skip(skip);
    if (buf[0] == 0) return has_fs ? ok_json(req) : fail(req, "400 Bad Request", "host 不能为空");
    if (qqmusic_set_config(buf, true) != ESP_OK) return fail(req, "500", "保存失败");
    return ok_json(req);
}

/* 「播放设备」：0=本机（ESP 解码出声）/ 1=服务器（那台接 USB DAC 的机器出声）
 * 网页/手机也能切 —— 跟屏幕菜单里「设备」是同一个入口（都走 player_set_out）。 */
static esp_err_t h_out(httpd_req_t *req)
{
    cJSON *b = read_body(req);
    if (b == NULL) return fail(req, "400 Bad Request", "请求体不是合法 JSON");
    int dev = json_int(b, "dev", -1);
    cJSON_Delete(b);
    if (dev < 0 || dev >= (int)PLAYER_OUT_COUNT) {
        return fail(req, "400 Bad Request", "dev 只能是 0（本机）或 1（服务器）");
    }
    player_set_out((player_out_t)dev);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddNumberToObject(o, "out", (int)player_out());
    cJSON_AddStringToObject(o, "out_name", player_out_name(player_out()));
    cJSON_AddNumberToObject(o, "requested", dev);
    /* 切换在 player 任务里异步做（要停本机、让服务器接管，1~3 秒）*/
    cJSON_AddStringToObject(o, "note", "切换异步执行，约 1~3 秒后 GET /api/out 才会变成新值");
    return send_json(req, o);
}

static esp_err_t h_out_get(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "out", (int)player_out());
    cJSON_AddStringToObject(o, "out_name", player_out_name(player_out()));
    return send_json(req, o);
}

static esp_err_t h_back(httpd_req_t *req)
{
    cJSON *b = read_body(req);
    if (b == NULL) return fail(req, "400 Bad Request", "请求体不是合法 JSON");
    int i = json_int(b, "i", -1);
    bool autoplay = json_bool(b, "autoplay", true);
    cJSON_Delete(b);
    if (i < 0 || i >= (int)PLAYER_BACK_COUNT) return fail(req, "400 Bad Request", "来源下标越界");
    player_set_back((player_back_t)i, autoplay);
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
    /* ⚠️ 这两个以前是写死的 "mp3"/192，不反映真实设置 —— 改成实际值，
     *    否则改了 transcode 在网页上完全看不出来。 */
    cJSON_AddStringToObject(o, "transcode", subsonic_transcode());
    cJSON_AddNumberToObject(o, "max_kbps", subsonic_max_kbps());
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

    char hostbuf[200] = {0}, userbuf[48] = {0}, passbuf[64] = {0}, tcbuf[8] = {0};
    if (host) strlcpy(hostbuf, host, sizeof(hostbuf));
    if (user) strlcpy(userbuf, user, sizeof(userbuf));
    if (pass) strlcpy(passbuf, pass, sizeof(passbuf));
    if (tc)   strlcpy(tcbuf, tc, sizeof(tcbuf));
    cJSON_Delete(b);

    /* ★ 地址规范化（2026-10-16 用户实测踩到）：
     *   · 去掉首尾空白 —— 手机/输入法很容易带一个空格，而 URL 里多一个空格就是另一个主机
     *   · 补 http:// —— 只写 "192.168.0.116:4533" 是最自然的输入，但 esp_http_client
     *     解析不出 host 时会直接返回 NULL，用户只看到一句 "ESP_FAIL"，
     *     完全猜不到是自己少写了协议头（实测就是这个死法）
     *   · 去掉结尾的 "/" —— 否则拼出来是 "...//rest/ping.view" */
    size_t hl = strlen(hostbuf);
    while (hl > 0 && (hostbuf[hl - 1] == ' ' || hostbuf[hl - 1] == '\t')) hostbuf[--hl] = 0;
    size_t hs = 0;
    while (hostbuf[hs] == ' ' || hostbuf[hs] == '\t') hs++;
    if (hs > 0) {
        memmove(hostbuf, hostbuf + hs, hl - hs + 1);
        hl -= hs;                       /* ⚠️ 必须同步更新，否则下面的去尾斜杠看到的是旧长度 */
    }
    while (hl > 7 && hostbuf[hl - 1] == '/') hostbuf[--hl] = 0;
    if (strncasecmp(hostbuf, "http://", 7) != 0 && strncasecmp(hostbuf, "https://", 8) != 0) {
        char tmp[216];
        int n = snprintf(tmp, sizeof(tmp), "http://%s", hostbuf);
        if (n > 0 && (size_t)n < sizeof(tmp)) strlcpy(hostbuf, tmp, sizeof(hostbuf));
    }
    if (hostbuf[0] == 0) return fail(req, "400 Bad Request", "服务器地址不能为空");

    /* ★ 面包屑：万一这一步把板子搞重启了，RTC 黑匣子（GET /api/crash）里最后一行
     *   就是它 —— 能立刻分清"死在连服务器上"还是"死在别处"。
     *   实测点"测试连接"会重启一次，就是这么查出来的。
     *   ⚠️ 账号名不记（这段日志会上网页），只记"填没填"；而"填没填"只有这里看得准，
     *      因为 subsonic_test_config 内部会把空账号回退成已存的那个。 */
    ESP_LOGW(TAG, "[黑匣子] /api/config/%s 开始：host=%s 账号%s 密码%s "
                  "httpd 栈余量=%u B（内部 SRAM 空 %u KB）",
             save ? "save" : "test", hostbuf,
             userbuf[0] ? "本次填了" : "未填（用已存的）",
             passbuf[0] ? "本次填了" : "未填（用已存的）",
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));

    char info[160] = {0};
    esp_err_t err = subsonic_test_config(hostbuf, userbuf, passbuf, info, sizeof(info));
    ESP_LOGW(TAG, "[黑匣子] /api/config/%s 结束：err=%s(0x%x) info=%.120s 栈余量=%u B",
             save ? "save" : "test", esp_err_to_name(err), (unsigned)err, info,
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
    if (err != ESP_OK) {
        /* 两种失败要分开说，否则用户完全不知道该改哪儿：
         *   · 服务器答话了但拒绝（info 有内容）→ 多半是账号/密码，原样带出来
         *   · 压根没连上（info 是空的）→ 必须把"试的是哪个地址"写出来，
         *     因为最常见的错因是端口没写（Navidrome 默认 4533，不写就是 80） */
        char msg[256];
        if (info[0]) {
            snprintf(msg, sizeof(msg), "服务器拒绝了：%s", info);
        } else {
            snprintf(msg, sizeof(msg),
                     "连不上 %s（%s）—— 检查端口（Navidrome 默认 4533）和是否同一网段",
                     hostbuf, esp_err_to_name(err));
        }
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

    /* ★ 排查"这歌怎么变 MP3 了"用：最近一次取流为什么直出/为什么转码。
     * 值形如 "直出" / "后缀=''" / "码率234>300KB/s" / "seek@139s" / "设置=raw"。
     * 有了它不用插串口就能查 —— 之前就是靠猜，来回烧了好几次固件。 */
    cJSON_AddStringToObject(o, "stream_why", subsonic_last_why());
    cJSON_AddNumberToObject(o, "i2c_retries", board_i2c_retries());
    cJSON_AddNumberToObject(o, "sample_rate", audio_engine_sample_rate());
    cJSON_AddNumberToObject(o, "ring_in_kb", audio_engine_ring_used_in() / 1024);
    cJSON_AddNumberToObject(o, "ring_pcm_kb", audio_engine_ring_used_pcm() / 1024);
    cJSON_AddNumberToObject(o, "settings_pending", settings_pending_count());
    cJSON_AddNumberToObject(o, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddStringToObject(o, "format", audio_engine_format_str());
    cJSON_AddNumberToObject(o, "bit_depth", audio_engine_bit_depth());
    cJSON_AddNumberToObject(o, "out_bits", audio_engine_out_bits());

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

    /* ★ 当前「播放设备」：0=本机（ESP 解码出声）/ 1=服务器（那台接 USB DAC 的机器出声）*/
    cJSON_AddNumberToObject(o, "out", (int)player_out());
    cJSON_AddStringToObject(o, "out_name", player_out_name(player_out()));

    /* ★ 崩溃取证三件套：本次开机是**为什么**开始的、开机过几次、
     *   以及 httpd 任务当前还剩多少栈（这个处理器就跑在 httpd 任务上，
     *   所以这个数字就是"处理一次请求之后"的余量，最接近溢出的一次）。
     *   ⚠️ 老版本把 reset_reason 记在 RTC 里，但被"魔数没先立起来"的 bug 清零了，
     *      于是永远读到 -1；现在直接读 esp_reset_reason()，拿的一定是真的。 */
    esp_reset_reason_t rr = esp_reset_reason();
    cJSON_AddNumberToObject(o, "reset_reason", (int)rr);
    cJSON_AddStringToObject(o, "reset_reason_name", reset_reason_name(rr));
    cJSON_AddNumberToObject(o, "httpd_stack_min", (double)uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t));
    cJSON_AddNumberToObject(o, "heap_min_kb", (double)(esp_get_minimum_free_heap_size() / 1024));
    return send_json(req, o);
}

/* GET /api/crash —— 黑匣子。
 * 为什么单独开一个接口而不是只打日志：日志环形只有 500 行，开完机再放 9 分钟歌
 * 就把它冲光了（实测 since=0 只能拿到第 305 行，崩溃现场早没了）。RTC 里的这份
 * 副本不受影响，**任何时候**都能读，包括"重启完过了一小时才想起来看"。 */
static esp_err_t h_crash(httpd_req_t *req);

/* ============================================================ 下载（转发给服务器）
 *
 * 需求：**设备网页控制台也能把 QQ 的歌下载进服务器的 Navidrome 曲库**。
 *
 * 为什么由固件转发、而不是网页直接调服务器：
 *   · 网页是从设备（192.168.0.105）打开的，直接跨域调 192.168.0.116:3210 会被浏览器拦；
 *   · 服务器地址本来就配在设备里（qqmusic_host()），转发一层等于免费复用它。
 * 这里只做"转发"，真正的下载/补封面/UTF-8 歌词/装进曲库全在服务器上做。
 */
static esp_err_t dl_forward(const char *path, char *out, size_t cap, size_t *len)
{
    /* ⚠️ 这个函数是**同步阻塞**的：它在 httpd 任务里发 HTTP，等服务器回包。
     *    设备只有一个 httpd 任务 —— 卡在这里的时候，网页所有别的请求
     *    （状态轮询、切歌、点列表）全部排队 = "点什么都没反应"。
     *    所以：① 超时压到 6 秒；② 失败后退避 20 秒，期间直接返回，不再去连。
     *    （真踩过：服务器不在时每 3.5 秒一次 25 秒超时，整个控制台像死了一样。） */
    static int64_t s_down_until = 0;
    int64_t now = esp_timer_get_time();
    if (s_down_until != 0 && now < s_down_until) {
        return ESP_ERR_INVALID_STATE;
    }

    char host[160];
    strlcpy(host, qqmusic_host(), sizeof(host));
    size_t hl = strlen(host);
    while (hl > 0 && host[hl - 1] == '/') host[--hl] = 0;
    char url[320];
    snprintf(url, sizeof(url), "%s%s", host, path);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 6000,         /* 服务器本机响应，6 秒足够；别拖垮 httpd */
        .buffer_size = 512,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (cli == NULL) return ESP_FAIL;
    esp_err_t err = esp_http_client_open(cli, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "下载转发连不上: %s（%s）", url, esp_err_to_name(err));
        s_down_until = now + 20LL * 1000 * 1000;
        esp_http_client_cleanup(cli);
        return err;
    }
    (void)esp_http_client_fetch_headers(cli);
    int status = esp_http_client_get_status_code(cli);
    size_t used = 0;
    while (used + 1 < cap) {
        int n = esp_http_client_read(cli, out + used, cap - 1 - used);
        if (n <= 0) break;
        used += n;
    }
    out[used] = 0;
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    if (len) *len = used;
    if (status != 200) {
        ESP_LOGW(TAG, "下载转发失败: %s → HTTP %d", url, status);
        s_down_until = now + 20LL * 1000 * 1000;
        return ESP_FAIL;
    }
    s_down_until = 0;
    return ESP_OK;
}

/* GET /api/dl?id=<QQ 歌曲 mid>：让服务器把这首下载进 Navidrome 曲库 */
static esp_err_t h_dl(httpd_req_t *req)
{
    char q[64] = {0};
    if (!query_str(req, "id", q, sizeof(q)) || q[0] == 0) {
        return fail(req, "400 Bad Request", "缺少 id（QQ 歌曲 mid）");
    }
    char path[128];
    snprintf(path, sizeof(path), "/v1/download/add?id=%s", q);
    /* httpd 是单任务串行处理，静态缓冲安全（也省 1.5KB 栈） */
    static char buf[1536];
    size_t n = 0;
    if (dl_forward(path, buf, sizeof(buf), &n) != ESP_OK) {
        return fail(req, "502 Bad Gateway",
                    "下载请求发不到服务器（检查服务器/QQ 服务是否正常）");
    }
    no_keepalive(req);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, buf, n);
}

/* GET /api/dl/stat：服务器那边的一句话下载概况（给网页显示进度） */
static esp_err_t h_dl_stat(httpd_req_t *req)
{
    static char buf[512];
    size_t n = 0;
    if (dl_forward("/v1/download/summary", buf, sizeof(buf), &n) != ESP_OK) {
        return fail(req, "502 Bad Gateway", "读不到服务器的下载状态");
    }
    no_keepalive(req);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, buf, n);
}

/* ============================================================ 有线联动（串口指令）
 *
 * 场景：**没有可用 WiFi 的时候**靠那根 USB 数据线（烧录口）救场：
 *   板子开热点 → 手机连上访问本页 → 把 WiFi 账号密码填进来 →
 *   通过串口让服务器也连上同一个 WiFi → 双方拿到 IP →
 *   再把"服务器地址"（QQ API :3210 / 音乐流 :4533）校准成服务器的新 IP。
 *
 * 协议见 components/usblink/include/usblink.h（一行一条，ESP< / ESP> 前缀）。
 */
static esp_err_t h_link(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ready", usblink_ready());
    cJSON_AddNumberToObject(o, "rx", usblink_rx_lines());
    cJSON_AddNumberToObject(o, "tx", usblink_tx_lines());
    cJSON_AddStringToObject(o, "last_cmd", usblink_last_cmd());
    cJSON_AddStringToObject(o, "last_reply", usblink_last_reply());
    return send_json(req, o);
}

/* GET /api/link/cmd?c=<指令>：把一条指令发给服务器（如 WIFI ssid pass / HOTSPOT / STATUS） */
static esp_err_t h_link_cmd(httpd_req_t *req)
{
    char c[160] = {0};
    if (!query_str(req, "c", c, sizeof(c)) || c[0] == 0) {
        return fail(req, "400 Bad Request", "缺少 c（要发给服务器的指令）");
    }
    esp_err_t err = usblink_send(c);
    if (err != ESP_OK) {
        return fail(req, "503 Service Unavailable",
                    "有线通道没就绪（板子插在服务器上了吗？）");
    }
    return ok_json(req);
}

/* GET /api/link/adopt?ip=x.x.x.x —— 把服务器地址校准成这个 IP。
 * 用途：配网成功后一条请求把 QQ API 和音乐流的地址都对齐，不用用户手抄 IP。 */
static esp_err_t h_link_adopt(httpd_req_t *req)
{
    char ip[64] = {0};
    if (!query_str(req, "ip", ip, sizeof(ip)) || ip[0] == 0) {
        return fail(req, "400 Bad Request", "缺少 ip");
    }
    char qhost[96], nhost[96];
    snprintf(qhost, sizeof(qhost), "http://%s:3210", ip);
    snprintf(nhost, sizeof(nhost), "http://%s:4533", ip);
    esp_err_t e1 = qqmusic_set_config(qhost, true);
    esp_err_t e2 = subsonic_set_config(nhost, NULL, NULL, NULL, 0);
    ESP_LOGW(TAG, "有线配网：服务器地址校准为 %s（QQ）/%s（音乐流）", qhost, nhost);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", e1 == ESP_OK && e2 == ESP_OK);
    cJSON_AddStringToObject(o, "qq", qhost);
    cJSON_AddStringToObject(o, "nav", nhost);
    return send_json(req, o);
}

/* GET /api/queue/more —— 让播放器再取一批歌追加到队列尾（列表拉到底时用）。
 * 只在【本机播放】有意义；服务器模式由服务器自己续。
 * ⚠️ 这里**不直接发 HTTP**（httpd 任务里做网络请求会把网页卡住），只置标记，
 *   由 player 任务去取；页面等 2~3 秒重新拉队列即可。 */
static esp_err_t h_queue_more(httpd_req_t *req)
{
    player_queue_more_async();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "note", "已请求加载更多，约 1~3 秒后重新拉队列");
    cJSON_AddNumberToObject(o, "count", player_queue_count());
    return send_json(req, o);
}

/* GET /api/link/provision?ssid=&pass= —— 启动"一键配网"（流程跑在设备上，页面关了也继续） */
static esp_err_t h_link_provision(httpd_req_t *req)
{
    char ssid[64] = {0}, pass[96] = {0};
    if (!query_str(req, "ssid", ssid, sizeof(ssid)) || ssid[0] == 0) {
        return fail(req, "400 Bad Request", "缺少 ssid");
    }
    (void)query_str(req, "pass", pass, sizeof(pass));
    esp_err_t err = usblink_provision_start(ssid, pass);
    if (err == ESP_ERR_INVALID_STATE) return fail(req, "409 Conflict", "已经有一个配网在跑了");
    if (err != ESP_OK) return fail(req, "500 Internal Server Error", "启动配网任务失败");
    return ok_json(req);
}

/* GET /api/link/provision/status —— 看配网进度（AP 断过、页面刷新过，这里都还有） */
static esp_err_t h_link_provision_status(httpd_req_t *req)
{
    int pct = 0;
    char msg[160] = {0};
    bool running = usblink_provision_status(&pct, msg, sizeof(msg));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "running", running);
    cJSON_AddNumberToObject(o, "pct", pct);
    cJSON_AddStringToObject(o, "msg", msg);
    cJSON_AddStringToObject(o, "ip", net_mgr_ip_str());
    return send_json(req, o);
}

/* ============================================================ 注册 */

/* ============================================================ 固件升级（OTA）
 *
 * 为什么现在做：分区表里那对 3MB 的 ota_0/ota_1 从项目一开始就留着
 * （注释写着"以后改 UI 走 WiFi 升级"），但一直没实现 ——
 * 结果每次改固件都得插 USB 烧录，实测一轮改了十几次就得插十几次。
 *
 * 流程：POST /api/update?confirm=1，**请求体就是 esp-music.bin 的原始字节** →
 *       写进"下一个" OTA 分区 → esp_ota_end() 校验 → 切换启动分区 → 1 秒后重启。
 *
 * 为什么刷不成砖：
 *   · `esp_ota_set_boot_partition()` **只在 esp_ota_end() 校验通过之后**才调用。
 *     中途断电 / 上传截断 / 传错文件，都只是"这次升级失败"，重启还是跑旧固件。
 *   · 升级全程只写"下一个"分区，**当前正在跑的那个分区一个字节都不碰**。
 *
 * ⚠️ 分块缓冲必须放 PSRAM：起播后内部 SRAM 只剩几十 KB，而 httpd 任务栈只有 4KB，
 *    在那儿放 4KB 的局部数组会直接爆栈。
 * ⚠️ esp_ota_write 要求每块长度是 4 的倍数（最后一块除外），所以按 4096 收。
 * ⚠️ 依赖 `partitions.csv` 里 ota_0/ota_1 这对槽 —— 它们**不能删**，
 *    删了就没法再远程升级，只能插 USB 救。
 */
static volatile int s_ota_pct = -1;      /* -1 = 没在升级；0..100 = 进度 */

static esp_err_t h_update_status(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "busy", s_ota_pct >= 0);
    cJSON_AddNumberToObject(o, "pct", s_ota_pct);
    /* 顺带报两个 app 槽，排查"这次升到哪个槽了"很方便 */
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *nxt = esp_ota_get_next_update_partition(NULL);
    cJSON_AddStringToObject(o, "running", run ? run->label : "?");
    cJSON_AddStringToObject(o, "next",    nxt ? nxt->label : "?");
    return send_json(req, o);
}

static void ota_reboot_cb(void *arg) { esp_restart(); }

static esp_err_t h_update(httpd_req_t *req)
{
    /* 防误触：别人随手 POST 一下不该触发升级 */
    char cf[8] = {0};
    if (!query_str(req, "confirm", cf, sizeof(cf)) || strcmp(cf, "1") != 0) {
        return fail(req, "400 Bad Request", "升级要带 ?confirm=1（防误触）");
    }
    if (req->content_len <= 0) {
        return fail(req, "400 Bad Request", "请求体是空的（要把 .bin 的原始字节作为 body）");
    }
    if (s_ota_pct >= 0) return fail(req, "409 Conflict", "已经有一次升级在进行");

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (part == NULL) return fail(req, "500", "没有可用的 OTA 分区（partitions.csv 里 ota_0/ota_1 还在吗？）");
    if ((size_t)req->content_len > part->size) {
        return fail(req, "413", "固件比 OTA 分区还大，装不下");
    }

    ESP_LOGW(TAG, "OTA 开始：%u 字节 → 分区 %s（%u KB）",
             (unsigned)req->content_len, part->label, (unsigned)(part->size / 1024));
    s_ota_pct = 0;

    const size_t CHUNK = 4096;
    char *buf = heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM);
    if (buf == NULL) { s_ota_pct = -1; return fail(req, "500", "没有内存收固件"); }

    esp_ota_handle_t ota = 0;
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    if (err != ESP_OK) {
        heap_caps_free(buf);
        s_ota_pct = -1;
        ESP_LOGE(TAG, "ota_begin 失败: %s", esp_err_to_name(err));
        return fail(req, "500", "ota_begin 失败");
    }

    int total = req->content_len;
    int got = 0;
    while (got < total) {
        int want = total - got;
        if (want > (int)CHUNK) want = (int)CHUNK;
        int n = httpd_req_recv(req, buf, want);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;      /* 网络抖动：重试这一块 */
        if (n <= 0) {
            ESP_LOGE(TAG, "收包中断（%d/%d）", got, total);
            err = ESP_FAIL;
            break;
        }
        err = esp_ota_write(ota, buf, n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ota_write 失败: %s", esp_err_to_name(err));
            break;
        }
        got += n;
        s_ota_pct = got * 100 / total;
    }
    heap_caps_free(buf);

    if (err != ESP_OK || got != total) {
        esp_ota_abort(ota);
        s_ota_pct = -1;
        return fail(req, "500", "写入失败/中断，已放弃 —— 旧固件不受影响，重启还是它");
    }

    /* ★ 校验：镜像损坏、或者传错文件（比如把 assets.bin 传上来）会在这里被挡下 */
    err = esp_ota_end(ota);
    if (err != ESP_OK) {
        s_ota_pct = -1;
        ESP_LOGE(TAG, "ota_end 校验失败: %s", esp_err_to_name(err));
        return fail(req, "400", "镜像校验失败 —— 是不是传错文件了？（要传 build/esp-music.bin）");
    }

    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        s_ota_pct = -1;
        ESP_LOGE(TAG, "set_boot_partition 失败: %s", esp_err_to_name(err));
        return fail(req, "500", "切换启动分区失败，这次升级没生效");
    }

    ESP_LOGW(TAG, "OTA 完成：下次从 %s 启动，1 秒后重启", part->label);
    s_ota_pct = 100;

    /* 先把响应发出去，客户端才知道成功了；重启放到定时器里做 ——
     * 否则"响应还没发完就重启"，浏览器只看到连接断开，会以为升级失败。 */
    esp_timer_handle_t t = NULL;
    const esp_timer_create_args_t targs = { .callback = ota_reboot_cb, .name = "ota_reboot" };
    if (esp_timer_create(&targs, &t) == ESP_OK) esp_timer_start_once(t, 1000 * 1000);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "note", "1 秒后重启到新固件");
    return send_json(req, o);
}

/* ============================================================ 网络日志
 *
 * 为什么需要它：这台设备平时**独立供电、不接电脑** —— 没插串口就等于瞎了。
 * 而开发时几乎所有的 bug 都是靠串口日志抓到的（接口静默注册失败、
 * 封面尺寸坑、"QQ 到底拉到歌没有"）。把日志搬到网页上，
 * 不插线也能看，手机连设备热点也能看。
 *
 * 做法：钩住 `esp_log_set_vprintf()` —— 除了照常往串口打，再抄一份进 PSRAM 环形缓冲。
 * 缓冲按**行**存，每行带一个只增不减的序号；网页用 `?since=<序号>` 增量拉，
 * 所以每秒轮询也只传新增的那几行。
 *
 * ⚠️ 三条硬约束（vprintf 钩子会在**任意任务、甚至中断上下文**被调用）：
 *   1. 只做 memcpy 和索引运算 —— **不加锁、不 malloc、不做 I/O**，
 *      否则某个任务在打日志时持锁，别的任务再来打日志就死锁。
 *   2. 缓冲区启动时一次性分配在 PSRAM。
 *   3. 覆盖写：满了冲掉最旧的。日志要的是"最近发生了什么"，旧的不重要。
 */
#define LOG_LINE_MAX   192                      /* 单行截断长度 */
/* ⚠️ 2026-10-14：160 行**不够用**，而且是排查时最要命的那种不够 ——
 *    开机前 2.5 秒就能打出 160 行（ST7735 初始化自己就占 20 多行），
 *    于是 web_log_init() 里回放的"上次崩溃尾部日志 + 复位原因"**在网页服务器
 *    还没起来的时候就已经被冲掉了**。实测丢了两次现场：按 K3 崩溃，抓下来的日志
 *    永远从开机第 157 行开始，崩溃原因一个字都看不到。
 *    500 行 × 192B ≈ 94 KB PSRAM（还有 4.8MB 富余）—— 换"现场留得住"很值。 */
#define LOG_LINES      500

typedef struct {
    volatile uint32_t seq;                      /* 只增不减的行号（0 = 这个槽还没用过） */
    volatile uint16_t len;
    char              text[LOG_LINE_MAX];
} log_rec_t;

static log_rec_t *s_log;                        /* PSRAM 环形数组 */
static volatile uint32_t s_log_next;            /* 下一个要写的行号（从 1 开始） */
static vprintf_like_t    s_old_vprintf;         /* 原来的输出（串口），必须继续调 */

/* ---------- 崩溃现场：放在 RTC 慢速内存里 ----------
 *
 * 为什么是 RTC RAM 而不是写 flash/cache 分区：
 *   · **软复位、看门狗、异常崩溃都不丢** —— 只有彻底断电才丢。
 *     而我们要覆盖的场景（"新固件能启动但一启动就崩，反复重启"）**正是软复位循环**，
 *     RTC RAM 完全够用。
 *   · 没有 flash 磨损顾虑，所以可以**每行都写**，崩溃瞬间的日志还在；
 *     写 flash 的话要在"多久落一次盘"里两难（太快伤 flash、太慢丢现场）。
 *
 * ⚠️ RTC 慢速内存总共只有 8KB（启动日志里 RTCRAM 是 7KB），所以这里只留 2KB ——
 *    够放"最后 10~20 行"，而那通常正好包含崩因。
 * ⚠️ 必须用魔数判断"这块内存是本次开机刚上电（内容随机）还是上次留下的"，
 *    否则上电会把随机字节当日志打出来。 */
#define RTC_LOG_SIZE  2048
/* ★ 上一次运行的尾巴**另存一份**（就是崩溃现场）。
 *   为什么不能只留一份：开机时要把上次的尾巴"回显"进本次日志，而回显本身也会写
 *   进这块 RTC 缓冲 —— 2KB 只够十几行，自己抄自己一遍就满了，真正的崩因被挤出去。
 *   （2026-10-16 实测：读到的现场只剩"上次日志回显"的套娃，全是空转。）
 *   RTC 慢速内存总共 8KB（启动日志里报 RTCRAM 7KB），2+2KB 是安全的。 */
#define RTC_TAIL_SIZE 2048
/* ⚠️ 魔数带版本号：改了这块内存的结构/语义就 +1，否则升级后的第一次开机会把
 *    旧固件留下的字节当成本固件的日志打出来（乱码事小，冤枉一次事大）。
 *    47 = "ESLG" 是旧版（只有一份缓冲、开机次数会被清零的那版）。 */
#define RTC_LOG_MAGIC 0x45534C48u               /* "ESLH" */
static RTC_NOINIT_ATTR char     s_rtc_log[RTC_LOG_SIZE];
static RTC_NOINIT_ATTR char     s_rtc_prev[RTC_TAIL_SIZE];
static RTC_NOINIT_ATTR uint32_t s_rtc_prev_len;
static RTC_NOINIT_ATTR uint32_t s_rtc_prev_reason;  /* 上一次运行是"为什么"结束的 */
static RTC_NOINIT_ATTR uint32_t s_rtc_magic;
static RTC_NOINIT_ATTR uint32_t s_rtc_len;
static RTC_NOINIT_ATTR uint32_t s_rtc_boots;    /* 累计开机次数：反复重启一眼可见 */
/* 回放上次日志期间不写 RTC（否则 2KB 被自己的回显占满） */
static bool s_rtc_mute;

/* 复位原因翻成人话。有它才能区分"崩了(panic)"“看门狗”“供电跌落”这三种完全不同的病 */
static const char *reset_reason_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "上电";
    case ESP_RST_EXT:       return "外部复位脚";
    case ESP_RST_SW:        return "软件复位（OTA/主动重启）";
    case ESP_RST_PANIC:     return "panic 异常（栈溢出/野指针/断言）";
    case ESP_RST_INT_WDT:   return "中断看门狗（长时间关中断）";
    case ESP_RST_TASK_WDT:  return "任务看门狗（某任务独占 CPU）";
    case ESP_RST_WDT:       return "其他看门狗";
    case ESP_RST_DEEPSLEEP: return "休眠唤醒";
    case ESP_RST_BROWNOUT:  return "电源跌落 brownout（供电不够）";
    case ESP_RST_SDIO:      return "SDIO";
    case ESP_RST_USB:       return "USB";
    default:                return "未知/无法判定";
    }
}

/* 往 RTC 环形里追加一行（软复位后还能读到） */
static void rtc_log_append(const char *s, size_t n)
{
    if (n == 0 || s_rtc_mute) return;
    if (s_rtc_magic != RTC_LOG_MAGIC) {         /* 上电首次：初始化 */
        s_rtc_magic = RTC_LOG_MAGIC;
        s_rtc_len = 0;
        s_rtc_boots = 0;
        s_rtc_prev_len = 0;
    }
    if (n > RTC_LOG_SIZE / 2) {                 /* 单行太长就截断（保尾部） */
        s += n - RTC_LOG_SIZE / 2;
        n = RTC_LOG_SIZE / 2;
    }
    if (s_rtc_len + n > RTC_LOG_SIZE) {         /* 满了：整体前移，冲掉最旧的 */
        size_t drop = s_rtc_len + n - RTC_LOG_SIZE;
        memmove(s_rtc_log, s_rtc_log + drop, s_rtc_len - drop);
        s_rtc_len -= drop;
    }
    memcpy(s_rtc_log + s_rtc_len, s, n);
    s_rtc_len += n;
}

/* 把一块（不一定以 NUL 结尾的）RTC 日志按行打出来 */
static void rtc_tail_print(const char *buf, size_t len, const char *prefix)
{
    size_t i = 0;
    while (i < len) {
        size_t j = i;
        while (j < len && buf[j] != '\n') j++;
        char line[200];
        size_t n = j - i;
        if (n > sizeof(line) - 1) n = sizeof(line) - 1;
        memcpy(line, buf + i, n);
        line[n] = 0;
        if (n > 0) ESP_LOGW(TAG, "  %s%s", prefix, line);
        i = (j < len) ? j + 1 : j;
    }
}

/* ★ 一次开机只调一次，而且必须在**本固件第一条日志之前**调。
 *
 * 为什么不能放在 rtc_log_dump_prev() 里（原来的写法就是这么错的）：
 *   本固件的第一条日志（"网络日志已启用"）一写就把 RTC 缓冲从"上次的现场"变成
 *   "本次开机的第 1 行"，随后 dump 再判定"有没有上次的尾巴"时，看到的是刚写进去
 *   自己那行 —— 于是把**自己的开机日志**当成上次的崩溃现场报出来。
 *   实测证据：OTA 后第一次 /api/crash 读到 prev_len=94，内容就是那行"网络日志已启用"。
 *   所以判定必须在这里做：此刻 RTC 里还是纯粹的"上一次运行"。
 *
 * ⚠️ 另外两个坑（都踩过）：
 *   1) **先把魔数立起来**再记开机次数。以前顺序反了：魔数无效时紧接着的第一次
 *      append 会把刚记的开机次数当"上电首次"清零 → 永远读到"累计开机 0 次"。
 *   2) 必须把上次的尾巴**拷到另一块**（s_rtc_prev），不能只靠 s_rtc_log —— 回显
 *      会把它覆盖掉，2KB 只够十几行，一抄就没了。 */
static void rtc_blackbox_boot(void)
{
    esp_reset_reason_t rr = esp_reset_reason();
    bool had = (s_rtc_magic == RTC_LOG_MAGIC && s_rtc_len > 0 && s_rtc_len <= RTC_LOG_SIZE);

    if (s_rtc_magic != RTC_LOG_MAGIC) {         /* 上电冷启动：这块内存内容不可信 */
        s_rtc_boots = 0;
        s_rtc_len = 0;
    }

    if (had) {
        size_t n = s_rtc_len;
        if (n > sizeof(s_rtc_prev)) n = sizeof(s_rtc_prev);
        memcpy(s_rtc_prev, s_rtc_log, n);
        s_rtc_prev_len = n;
        /* rr = "本次开机为什么开始" = "上一次运行为什么结束" —— 正好是现场的死因 */
        s_rtc_prev_reason = (uint32_t)rr;
    } else {
        s_rtc_prev_len = 0;
        s_rtc_prev_reason = (uint32_t)ESP_RST_UNKNOWN;
    }

    s_rtc_magic = RTC_LOG_MAGIC;
    s_rtc_len = 0;                              /* 工作缓冲从这个开机点重新开始 */
    s_rtc_boots++;
}

/* 把上一次运行的尾部日志打出来（在钩子装好之后调，这样它自己也会进新日志）。
 * ⚠️ 判定"有没有上次的尾巴"**不在这里做**，见 rtc_blackbox_boot()：
 *    这里只负责回显，而且回显期间必须 mute RTC，否则 2KB 缓冲会被自己的回显填满。 */
static void rtc_log_dump_prev(void)
{
    esp_reset_reason_t rr = esp_reset_reason();

    if (s_rtc_prev_len > 0) {
        s_rtc_mute = true;
        ESP_LOGW(TAG, "===== 上次运行的尾部日志（RTC 黑匣子，共 %u 字节；"
                      "上一次复位原因=%u %s；累计开机 %u 次）=====",
                 (unsigned)s_rtc_prev_len, (unsigned)s_rtc_prev_reason,
                 reset_reason_name((esp_reset_reason_t)s_rtc_prev_reason),
                 (unsigned)s_rtc_boots);
        rtc_tail_print(s_rtc_prev, s_rtc_prev_len, "[上次] ");
        ESP_LOGW(TAG, "===== 上次日志结束 =====");
        s_rtc_mute = false;
    } else {
        ESP_LOGI(TAG, "RTC 里没有上次的日志（上电冷启动 / 刚升级完固件）");
    }
    ESP_LOGW(TAG, "本次开机：复位原因=%d（%s）；完整黑匣子 GET /api/crash",
             (int)rr, reset_reason_name(rr));
}

static void log_line(const char *s, size_t n)
{
    if (n == 0) return;
    rtc_log_append(s, n);                       /* 先存崩溃现场（哪怕下面没缓冲） */
    if (s_log == NULL) return;
    if (n > LOG_LINE_MAX - 1) n = LOG_LINE_MAX - 1;
    uint32_t seq = s_log_next;
    log_rec_t *r = &s_log[seq % LOG_LINES];
    /* 先写内容，最后再写 seq —— 读的人靠 seq 判断这条是不是新的/写完的 */
    memcpy(r->text, s, n);
    r->text[n] = 0;
    r->len = (uint16_t)n;
    r->seq = seq + 1;                           /* 行号从 1 开始，0 表示空槽 */
    s_log_next = seq + 1;
}

/* vprintf 钩子：串口照打，另抄一份进环形缓冲 */
static int log_vprintf(const char *fmt, va_list ap)
{
    va_list cp;
    va_copy(cp, ap);
    char tmp[LOG_LINE_MAX];
    int n = vsnprintf(tmp, sizeof(tmp), fmt, cp);
    va_end(cp);
    if (n > 0) log_line(tmp, (size_t)(n < (int)sizeof(tmp) ? n : (int)sizeof(tmp) - 1));
    /* ★ 串口输出必须继续走原来的路子，不能因为加了网页日志就丢掉串口 */
    return s_old_vprintf ? s_old_vprintf(fmt, ap) : n;
}

void web_log_init(void)
{
    if (s_log != NULL) return;
    /* ★ 必须排在分配/打日志之前：一有日志落进 RTC，"上次的现场"就没了（见函数注释） */
    rtc_blackbox_boot();
    s_log = heap_caps_calloc(LOG_LINES, sizeof(log_rec_t), MALLOC_CAP_SPIRAM);
    if (s_log == NULL) {
        ESP_LOGW(TAG, "日志环形缓冲分配失败（%u KB）—— 网页日志不可用",
                 (unsigned)(LOG_LINES * sizeof(log_rec_t) / 1024));
        return;
    }
    s_log_next = 1;
    s_old_vprintf = esp_log_set_vprintf(log_vprintf);
    ESP_LOGW(TAG, "网络日志已启用：%d 行 × %d 字节（PSRAM），GET /api/log?since=N",
             LOG_LINES, LOG_LINE_MAX);

    /* ★ 钩子装好【之后】再回放上次崩溃的尾部日志 —— 这样这段回放本身
     *   也会进本次的网络日志，网页上连着看就是"上次怎么崩的 + 这次开机"。 */
    rtc_log_dump_prev();
}

/* GET /api/log?since=<上次拿到的最大行号> → 纯文本，每行 "<行号> <内容>"
 * 为什么要带行号：客户端拿最大行号当地标，下次只拉新的 —— 每秒轮询也不费流量。 */
static esp_err_t h_log(httpd_req_t *req)
{
    char q[16] = {0};
    uint32_t since = 0;
    if (query_str(req, "since", q, sizeof(q))) since = (uint32_t)strtoul(q, NULL, 10);

    if (s_log == NULL) return fail(req, "503 Service Unavailable", "网络日志没启用（缓冲分配失败）");

    /* 先算最老还有效的行号；since 比它还旧就从最老的开始（中间的被覆盖了） */
    uint32_t next = s_log_next;
    uint32_t oldest = (next > LOG_LINES) ? (next - LOG_LINES) : 1;
    if (since + 1 > oldest) oldest = since + 1;

    /* 拼进 PSRAM（最多 30KB）—— 不占内部 SRAM，也不做 chunked 的麻烦事 */
    size_t cap = (size_t)LOG_LINES * (LOG_LINE_MAX + 16) + 64;
    char *out = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (out == NULL) return fail(req, "500", "没有内存拼日志");
    size_t o = 0;
    for (uint32_t s = oldest; s < next; s++) {
        const log_rec_t *r = &s_log[(s - 1) % LOG_LINES];
        if (r->seq != s || r->len == 0) continue;      /* 被覆盖了或还没写完 */
        /* ⚠️ 日志行**自带换行**，所以这里不加 \n —— 加了每行之间会多一个空行。
         *    但没有换行的（极少）补一个，否则两行会粘在一起。 */
        bool nl = (r->text[r->len - 1] == '\n');
        o += snprintf(out + o, cap - o, "%" PRIu32 " %s%s", s, r->text, nl ? "" : "\n");
        if (cap - o < LOG_LINE_MAX + 24) break;
    }
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    esp_err_t e = httpd_resp_send(req, out, o);
    heap_caps_free(out);
    return e;
}

/* 黑匣子的读出口。放在这一节后面是因为它直接用上面那几个 RTC_NOINIT 变量。 */
static esp_err_t h_crash(httpd_req_t *req)
{
    esp_reset_reason_t rr = esp_reset_reason();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "reason", (int)rr);
    cJSON_AddStringToObject(o, "reason_name", reset_reason_name(rr));
    cJSON_AddNumberToObject(o, "boots", (double)s_rtc_boots);
    cJSON_AddNumberToObject(o, "prev_len", (double)s_rtc_prev_len);
    cJSON_AddNumberToObject(o, "prev_reason", (double)s_rtc_prev_reason);
    cJSON_AddStringToObject(o, "prev_reason_name",
                            reset_reason_name((esp_reset_reason_t)s_rtc_prev_reason));
    cJSON_AddNumberToObject(o, "httpd_stack_min", (double)uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t));
    cJSON_AddNumberToObject(o, "heap_min_kb", (double)(esp_get_minimum_free_heap_size() / 1024));

    /* ⚠️ RTC 里的日志不是以 NUL 结尾的，得按长度拷一份出来再交给 cJSON */
    char *pt = NULL, *ct = NULL;
    if (s_rtc_prev_len > 0) {
        pt = heap_caps_malloc(s_rtc_prev_len + 1, MALLOC_CAP_SPIRAM);
        if (pt) { memcpy(pt, s_rtc_prev, s_rtc_prev_len); pt[s_rtc_prev_len] = 0; }
    }
    if (s_rtc_len > 0) {
        ct = heap_caps_malloc(s_rtc_len + 1, MALLOC_CAP_SPIRAM);
        if (ct) { memcpy(ct, s_rtc_log, s_rtc_len); ct[s_rtc_len] = 0; }
    }
    cJSON_AddStringToObject(o, "prev_tail", pt ? pt : "");
    cJSON_AddStringToObject(o, "tail", ct ? ct : "");
    if (pt) heap_caps_free(pt);
    if (ct) heap_caps_free(ct);
    return send_json(req, o);
}

esp_err_t web_console_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    /* ⚠️ 每个 socket 都要占 lwIP 缓冲，而这个芯片内部 SRAM 只剩二十几 KB。
     * 4 个够用（status 轮询 + 列表 + 封面），比默认 7 省一截。 */
    /* ⚠️ 内部 SRAM 在起播后只剩个位数 KB（解码器是预编译库、内存挪不到 PSRAM），
     * 所以这里每一项都往小里配：
     *   - socket 数：每个连接要一份 header 缓冲（HTTPD_MAX_REQ_HDR_LEN）+ 请求结构
     *   - 栈：JSON 拼装 + cJSON 全在 PSRAM，4KB 够
     * 配合 sdkconfig.defaults 里把 HTTPD_MAX_REQ_HDR_LEN / MAX_URI_LEN 调小。
     *
     * ⚠️⚠️ 2026-10-16 修正："4KB 够"是**错的**，代价是一次真机崩溃 + 一次用户投诉。
     * 起因：网页上点「测试连接」（或保存 Navidrome 地址）→ 板子重启一次。
     * RTC 黑匣子（GET /api/crash）抓到的现场：
     *     web: [黑匣子] /api/config/test 开始：host=http://192.168.0.116:4533 httpd 栈余量=632 B
     *     nav: [ping] 1/5 客户端已建（buffer 4096，url 126 字节）
     *     （日志到此为止）→ 复位原因 = 4 panic
     * 也就是说：**进 ping 时只剩 632 字节**，`esp_http_client_open()` 一压栈就完了。
     * 根因不是 ping 特殊，而是"在 httpd 任务里同步发 HTTP"这条路本来就要 2~3KB 栈：
     * /api/config(test)、/api/cover、/api/lyrics、/api/search 全走它。
     * 为什么不像别的任务那样把栈搬到 PSRAM（CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY）：
     * **httpd 任务自己会写 flash（POST /api/update 那 2.5MB 固件）**，
     * cache 一关它访问不了 PSRAM 栈 → 必崩。这条规矩项目里已经踩过（见 audio_engine.c
     * 里"decode/fetch 可以搬、out 不能搬"的注释）。所以只能扩内部 SRAM 的栈。
     * 代价：内部 SRAM 常态 30KB → 26KB。实测（httpd_stack_min）留了 2.6KB 余量，
     * 而 fix 前 OTA/播放/网页一切照旧。 */
    cfg.max_open_sockets   = 6;
    cfg.lru_purge_enable   = true;      /* 满了就踢最久没动的连接，别拒绝新请求 */
    cfg.stack_size         = 8192;
    /* ⚠️ 这个数字**必须大于 uris[] 的项数**，而且要有余量。
     *
     * 血泪教训（2026-10-11 真机验证抓到）：这里原来是 32，而 uris[] 已经有 **33** 项 ——
     * 超出的部分会**静默注册失败**，而且失败的是**数组末尾那几个**，不是新加的那个。
     * 当时的症状是 `/api/hid`、`/api/hid/send`（蓝牙遥控）一直注册不上，
     * 只在日志里留一行 `no slots left for registering handler`（W 级），
     * 平时看启动日志很容易划过去。我加 `/api/back` 把它从"少 1 个"变成"少 2 个"才发现。
     *
     * 内存代价：每项只是一个指针（sizeof(httpd_uri_t*)），48 项 = 192 字节，可以忽略。
     * 所以这里给足余量，宁可多留也别再踩一次。 */
    cfg.max_uri_handlers   = 64;
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
        { .uri = "/api/cache",           .method = HTTP_POST, .handler = h_cache },
        { .uri = "/api/log",             .method = HTTP_GET,  .handler = h_log },
        { .uri = "/api/qq",              .method = HTTP_GET,  .handler = h_qq },
        { .uri = "/api/qq",              .method = HTTP_POST, .handler = h_qq },
        { .uri = "/api/update",          .method = HTTP_POST, .handler = h_update },
        { .uri = "/api/update",          .method = HTTP_GET,  .handler = h_update_status },
        { .uri = "/",                    .method = HTTP_GET,  .handler = h_index },
        { .uri = "/api/status",          .method = HTTP_GET,  .handler = h_status },
        { .uri = "/api/lyrics",          .method = HTTP_GET,  .handler = h_lyrics },
        { .uri = "/api/queue",           .method = HTTP_GET,  .handler = h_queue },
        { .uri = "/api/queue/more",      .method = HTTP_GET,  .handler = h_queue_more },
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
        { .uri = "/api/crash",           .method = HTTP_GET,  .handler = h_crash },
        { .uri = "/api/play",            .method = HTTP_POST, .handler = h_play },
        { .uri = "/api/toggle",          .method = HTTP_POST, .handler = h_toggle },
        { .uri = "/api/next",            .method = HTTP_POST, .handler = h_next },
        { .uri = "/api/prev",            .method = HTTP_POST, .handler = h_prev },
        { .uri = "/api/seek",            .method = HTTP_POST, .handler = h_seek },
        { .uri = "/api/volume",          .method = HTTP_POST, .handler = h_volume },
        { .uri = "/api/mute",            .method = HTTP_POST, .handler = h_mute },
        { .uri = "/api/bright",          .method = HTTP_POST, .handler = h_bright },
        { .uri = "/api/source",          .method = HTTP_POST, .handler = h_source },
        { .uri = "/api/back",            .method = HTTP_POST, .handler = h_back },
        { .uri = "/api/out",             .method = HTTP_GET,  .handler = h_out_get },
        { .uri = "/api/out",             .method = HTTP_POST, .handler = h_out },
        /* 下载（固件转发给服务器的下载接口，见 dl_forward 的说明） */
        { .uri = "/api/dl",              .method = HTTP_GET,  .handler = h_dl },
        { .uri = "/api/dl/stat",         .method = HTTP_GET,  .handler = h_dl_stat },
        /* 有线联动（串口）：无网时通过数据线指挥服务器开热点/配网 */
        { .uri = "/api/link",            .method = HTTP_GET,  .handler = h_link },
        { .uri = "/api/link/cmd",        .method = HTTP_GET,  .handler = h_link_cmd },
        { .uri = "/api/link/adopt",      .method = HTTP_GET,  .handler = h_link_adopt },
        { .uri = "/api/link/provision",  .method = HTTP_GET,  .handler = h_link_provision },
        { .uri = "/api/link/provision/status", .method = HTTP_GET, .handler = h_link_provision_status },
        /* 音质挡位：GET 读、POST 写，同一个处理器按 method 分支 */
        { .uri = "/api/quality",         .method = HTTP_GET,  .handler = h_quality },
        { .uri = "/api/quality",         .method = HTTP_POST, .handler = h_quality },
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
