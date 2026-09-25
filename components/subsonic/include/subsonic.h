/*
 * Navidrome / Subsonic 客户端
 *
 * 认证：u + t/s 形式，t = md5(password + salt)，salt 每次请求重新生成（≥6 字符）。
 *   （Navidrome 0.63 不支持 OpenSubsonic 的 apiKey 认证，只能用 u+p 或 u+t+s）
 * 必须先声明 c = 客户端名，否则 Navidrome 的 checkRequiredParameters 会拒；
 * 且不能用 "DSub"/"SubMusic"（会被当成 legacy/minimal 客户端，返回被裁剪的响应）。
 *
 * ⚠️ 播放必须显式带 format=：只给 maxBitRate 时 Navidrome 会默认给 Opus。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SUB_SONGS_MAX 64

typedef struct {
    char     id[48];
    char     title[160];
    char     artist[96];
    char     album[96];
    char     cover[48];     /* coverArt id，请求封面用它而不是歌曲 id */
    uint32_t duration_s;
    uint32_t size;
    char     suffix[8];
    bool     starred;
} sub_song_t;

/* 从 NVS(ns "nav") 读配置，Kconfig 兜底 */
esp_err_t subsonic_init(void);
const char *subsonic_host(void);
bool        subsonic_configured(void);

/* 连通性 + 服务器信息（/rest/ping）。server_info 失败时带回原因（如鉴权失败的消息） */
esp_err_t subsonic_ping(char *server_info, size_t info_len);
bool      subsonic_has_password(void);

/* M5 网页配网用：用【给定】凭据试连一次，不保存、不影响全局状态。
 * pass 传 NULL/空串 = 沿用已存的（网页上"不改密码"的情况） */
esp_err_t subsonic_test_config(const char *host, const char *user, const char *pass,
                               char *server_info, size_t info_len);

/* 保存并立刻生效（NVS ns "nav"）。传 NULL 的字段保持原值不动 */
esp_err_t subsonic_set_config(const char *host, const char *user, const char *pass,
                              const char *transcode, int max_kbps);

/* 曲库搜索（网页搜索框），最多 SUB_SONGS_MAX 条 */
esp_err_t subsonic_search(const char *query, sub_song_t *out, int max, int *got);

/* 随机取歌（一键随机 30 首的后端） */
esp_err_t subsonic_get_random(sub_song_t *out, int max, int *got);

/* 我的收藏（getStarred2）。注意 out 里每首的 starred 都是 true */
esp_err_t subsonic_get_starred(sub_song_t *out, int max, int *got);

/* 收藏 / 取消收藏（写回 Navidrome，网页端立刻能看到） */
esp_err_t subsonic_star(const char *song_id, bool on);

/* 拼播放 URL。max_kbps<=0 时用配置里的默认值 */
esp_err_t subsonic_stream_url(const sub_song_t *song, int max_kbps, char *out, size_t out_len);

/* 从第 offset_s 秒开始拉的流（seek 用）。
 * ⚠️ 转码流没有 Accept-Ranges（实测），所以 seek 只能靠 timeOffset 让服务端重转；
 *    只有 format != raw 时才生效，raw 会被 Navidrome 静默忽略（表现为"seek 后从头播"）。 */
esp_err_t subsonic_stream_url_at(const sub_song_t *song, int max_kbps, uint32_t offset_s,
                                 char *out, size_t out_len);

/* 取歌词原始 JSON（OpenSubsonic getLyricsBySongId，带毫秒时间轴） */
esp_err_t subsonic_get_lyrics_json(const char *song_id, char **json_out);

/* ★ 归一化歌词，给网页控制台用。输出 {"synced":bool,"total":N,"lines":[{"t":ms,"text":"..."}]}，
 * 调用方负责 heap_caps_free。
 * ⚠️ **没有歌词时也返回 ESP_OK + 空 lines**（纯音乐占位行会被识别并去掉）——
 *    前端要显示"暂无歌词"，不要当错误。 */
esp_err_t subsonic_get_lyrics(const char *song_id, char **json_out);

/* 取原始封面字节（已校验是 JPEG）。**网页控制台的封面代理用它** ——
 * 浏览器不认 RGB565，必须把 JPEG 原样转发。调用方负责 heap_caps_free(*out) */
esp_err_t subsonic_get_cover_raw(const char *cover_id, int size_px,
                                 uint8_t **out, size_t *len_out);

/* 单曲详情（按 id 播放队列外的歌时用） */
esp_err_t subsonic_get_song(const char *song_id, sub_song_t *out);

/* 取封面并解码成 RGB565（大端字节序，与本机屏幕要求的字节序一致，零转换）。
 * cover_id 用歌曲的 coverArt 字段（不是歌曲 id）。
 * out 需自行提供（PSRAM），建议 size=96 时给 96*96*2 = 18KB。 */
esp_err_t subsonic_get_cover_rgb565(const char *cover_id, int size_px,
                                    uint8_t *out, size_t out_cap,
                                    uint16_t *w_out, uint16_t *h_out);

#ifdef __cplusplus
}
#endif
