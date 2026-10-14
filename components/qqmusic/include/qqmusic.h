/*
 * qqmusic —— QQ 音乐（自建契约层服务）客户端
 *
 * 它不是"另一个 Navidrome"，而是**另一个后端**：
 *
 *   Navidrome   走 Subsonic 协议（subsonic.c），歌是"服务器上的文件"
 *   QQ 音乐     走我们自己在 F:\wsl\qqmusic 部署的契约层服务（本组件），
 *               歌是"按 songmid 现取的流"
 *
 * 两者在设备侧共用同一个歌曲结构（sub_song_t）、共用同一个播放引擎，
 * 区别只有两处：
 *   1) 列表从哪来（subsonic_* vs qqmusic_*）
 *   2) 播放地址怎么拼（Subsonic 的 /rest/stream vs QQ 的 /v1/url）
 *
 * ⚠️ 三条必须记住的（都写进了对应函数的注释）：
 *   · 播放地址**必须在播之前现取** —— QQ 的 vkey 只有 2 小时有效期，
 *     提前存进队列会"存的时候好好的，播的时候 403"。
 *   · QQ 的"收藏"是**只读镜像**（读 QQ 账号的「我喜欢」），
 *     设备上点红心**不写回 QQ** —— 写操作风控风险高，刻意不做。
 *   · 封面不走本服务：固件自己拼 y.qq.com 的直链（见 qqmusic_cover_url）。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "subsonic.h"      /* sub_song_t —— 两种后端共用的歌曲结构 */

#ifdef __cplusplus
extern "C" {
#endif

#define QQ_SONGS_MAX   64           /* 单次最多取多少首（与 SUB_SONGS_MAX 对齐） */
#define QQ_HOST_MAX    160
#define QQ_MODE_MAX    8

/* 「模式」—— 切到 QQ 音乐后端时，主菜单里"模式"可选的那几项。
 * 顺序就是菜单里显示的顺序，改这里就行（服务端 /v1/modes 是同一份清单）。 */
typedef enum {
    QQ_MODE_FAV = 0,      /* 收藏     —— QQ 账号的「我喜欢」（只读镜像） */
    QQ_MODE_DAILY,        /* 每日30首 —— 每天 30 首 */
    QQ_MODE_MILLION,      /* 百万收藏 */
    QQ_MODE_NEWREC,       /* 新歌推荐 */
    QQ_MODE_GUESS,        /* 刷歌     —— 每次刷新都不一样 */
    QQ_MODE_RADAR,        /* 雷达     —— 按你的收藏推荐 */
    QQ_MODE_COUNT,
} qq_mode_t;

const char *qq_mode_name(qq_mode_t m);      /* 屏上显示的中文名，越界返回 "" */
const char *qq_mode_slug(qq_mode_t m);      /* 服务端用的 mode 参数 */

/* ---------------- 配置 ---------------- */

/* 读 NVS 里的服务地址（没有就用 Kconfig 默认值）。只读配置，不联网。 */
esp_err_t   qqmusic_init(void);
bool        qqmusic_configured(void);
const char *qqmusic_host(void);
/* save=true 时写进 NVS（网页/设置页改地址用） */
esp_err_t   qqmusic_set_config(const char *host, bool save);

/* ---------------- 网络 ---------------- */

/* 健康检查（GET /v1/health）。
 * ok 时 msg 带回一句人话（如"凭据有效 / VIP"）；
 * 失败时 msg 带回原因 —— ★ 这类服务头号故障是"凭据过期"，
 * 表现却是"所有歌都放不了"，所以这个接口是排障第一入口。 */
esp_err_t qqmusic_health(char *msg, size_t len);

/* 结构化版的健康检查 —— 给设置里的「QQ 登录」页用（要逐项显示登录情况，
 * 一句人话不够）。返回 ESP_OK = 服务**连上了**（不代表凭据可用，看下面的字段）。
 *
 * ⚠️ 三个布尔值都必须分开看，它们是**三种完全不同的故障**：
 *    present=false → 从来没登录过（要扫码）
 *    expired=true  → 登录过但过期了（要重扫）★ 最常见，表现却是"所有歌都放不了"
 *    valid=false   → 凭据本身不完整/坏了
 *   而 ok=false 只说明服务自评不 ok，此时 reason 里才是原因。
 * 2026-10-14 实测：过期时 present/valid 仍是 true，只有 expired 会变 true，
 * 而 ok 甚至还是 true（VIP 接口照样能用）—— 所以**不能拿 ok 当登录状态**。 */
typedef struct {
    bool      reachable;    /* HTTP 通了、JSON 解出来了 */
    bool      ok;           /* 服务自评 ok */
    bool      present;      /* 有凭据（登录过） */
    bool      valid;        /* 凭据完好 */
    bool      expired;      /* 已过期 */
    bool      vip;          /* 识别到 VIP（决定能不能拿无损） */
    long long musicid;      /* 账号；0 = 没有 */
    char      reason[96];   /* 服务端给的原因 / 本地错误说明 */
} qqmusic_health_t;

esp_err_t qqmusic_health_ex(qqmusic_health_t *out);

/* ---------------- 扫码登录（设置里的「QQ 登录」页用） ----------------
 * HTTP 与 base64 都关在这一层里，固件上层只拿"PNG 原始字节"和"状态字符串"。
 *
 * ⚠️ 登录是**合约层服务端的全局状态**（同一时刻只有一张有效的码），
 *    所以这两个接口都没有 identifier 参数 —— 传进去只会多一层对不上的风险。 */

/* POST /v1/login/qr，把 image_base64 解成 PNG 原始字节。
 * 成功时 *png 是 PSRAM/内部堆上的缓冲，调用方用 heap_caps_free() 释放。 */
esp_err_t qqmusic_login_qr_png(uint8_t **png, size_t *len);

/* GET /v1/login/status。event 取值来自上游库的 QRCodeLoginEvents：
 *   WAITING 还没扫     SCAN 等待扫描中     CONF 已扫码、等手机上确认
 *   DONE    登录完成（凭据已落盘）        TIMEOUT 码过期
 *   REFUSE  手机上拒绝                    ERROR 服务端监听出错（err 里有原因）
 *   STALE 给的是旧码（本固件不会出现） */
esp_err_t qqmusic_login_status(char *event, size_t event_len,
                               bool *saved, char *err, size_t err_len);

/* 按模式取列表（GET /v1/list?mode=...） */
esp_err_t qqmusic_list(qq_mode_t mode, sub_song_t *out, int max, int *got);

/* 搜索（GET /v1/search?q=...） */
esp_err_t qqmusic_search(const char *query, sub_song_t *out, int max, int *got);

/* ★ 取播放直链（GET /v1/url?id=...）。**每次播之前现取**，不要缓存。
 * out 里有 vkey，有效期约 2 小时。长度约 250~260 字符 → 调用方给 512 以上。 */
esp_err_t qqmusic_stream_url(const sub_song_t *song, char *out, size_t out_len);

/* 歌词（GET /v1/lyric?id=...），返回 LRC 纯文本 */
esp_err_t qqmusic_lyrics(const char *song_id, char *out, size_t out_len);

/* 歌词的**归一化 JSON**（GET /v1/lyric?id=...&format=json）：
 * `{"synced":bool,"total":N,"lines":[{"t":毫秒,"text":"..."}]}` —— 网页控制台要这个。
 * LRC 解析放在服务端做（一行多个时间标签、小数位数、无时间轴都处理好了），
 * 设备只管转发。调用方给 4KB 以上（实测 70 行的歌词 JSON 约 3KB）。 */
esp_err_t qqmusic_lyrics_json(const char *song_id, char *out, size_t out_len);

/* 把歌加入 QQ 账号的「我喜欢」（红心）。
 *
 * ⚠️ **只能加，不能删**（2026-10-11 查清，服务端 README 里有完整证据）：
 *   加（AddSonglist）实测有效；删（DelSonglist）QQ 对 dirId=201 **静默忽略** ——
 *   返回 retCode=0 但什么都不做。穷举过 4 类 10 种组合，全部无效。
 *
 *   ⚠️ 别被上游库的自测骗了：它的 test_like_song_roundtrip 只断言返回值，
 *      而 del_songs 是 `return retCode == 0`（删失败也是 0）→ **假阳性**。
 *
 *   所以 on=false 时本函数**直接返回 ESP_ERR_NOT_SUPPORTED，连请求都不发**，
 *   也不改任何本地状态。调用方据此提示用户"请到 QQ 音乐 App 里手动取消"。
 *
 * 成功时 *added = 服务端**回读收藏总数**后确认真的新增了
 *   （false 表示这首歌本来就在收藏里；服务端不盲信上游返回值）。 */
esp_err_t qqmusic_favorite(const sub_song_t *song, bool on, bool *added);

/* 封面直链：固件自己拼，不经服务（实测直连可通、不带请求头也行）。
 * ⚠️ size 只能取 90/120/150/300/500/800 —— 其它尺寸 QQ 一律 404。 */
esp_err_t qqmusic_cover_url(const char *album_mid, int size, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
