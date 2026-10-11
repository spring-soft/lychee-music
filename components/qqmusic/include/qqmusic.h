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

/* 按模式取列表（GET /v1/list?mode=...） */
esp_err_t qqmusic_list(qq_mode_t mode, sub_song_t *out, int max, int *got);

/* 搜索（GET /v1/search?q=...） */
esp_err_t qqmusic_search(const char *query, sub_song_t *out, int max, int *got);

/* ★ 取播放直链（GET /v1/url?id=...）。**每次播之前现取**，不要缓存。
 * out 里有 vkey，有效期约 2 小时。长度约 250~260 字符 → 调用方给 512 以上。 */
esp_err_t qqmusic_stream_url(const sub_song_t *song, char *out, size_t out_len);

/* 歌词（GET /v1/lyric?id=...），返回 LRC 纯文本 */
esp_err_t qqmusic_lyrics(const char *song_id, char *out, size_t out_len);

/* 封面直链：固件自己拼，不经服务（实测直连可通、不带请求头也行）。
 * ⚠️ size 只能取 90/120/150/300/500/800 —— 其它尺寸 QQ 一律 404。 */
esp_err_t qqmusic_cover_url(const char *album_mid, int size, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
