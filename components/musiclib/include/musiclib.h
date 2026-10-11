/*
 * musiclib —— TF 卡离线曲库（扫描 / 文件名解析 / 索引 / 搜索 / 匹配）
 *
 * 职责：把 /sdcard 下的音乐文件扫成一份索引，供离线模式当"曲库"用。
 *
 * 几个刻意的决定（都有理由，改之前先读）：
 *
 * 1) 元数据来自【文件名】，不解析 ID3/FLAC 标签。
 *    规则：`艺术家 - 标题.ext`（以第一个 " - " 切分），没有 " - " 就整体当标题。
 *    用户已确认这条路线。标签解析留作后续增强（结构上不用改，只要在解析处换实现）。
 *
 * 2) 索引常驻 PSRAM，**不按 sub_song_t 存**（那要 660B/首，2000 首 1.3MB），
 *    而是用更小的 musiclib_rec_t（约 372B/首），要往外给的时候才转成 sub_song_t。
 *
 * 3) 索引写【卡上】的一个文件（/sdcard/ESP-MUSIC.IDX），不写 flash：
 *    索引是可重建的（重扫一次就有），放卡上省 flash 磨损，也天然跟着卡走。
 *
 * 4) 索引【不做原子改名】那种写法，改用头部 magic+version+rec_size+CRC32 校验：
 *    掉电写坏了一半，下次 load 时校验不过就当"没有索引"，让用户重扫。
 *    反正索引可重建 —— 这比在卡上多做一次 rename 更简单也更不容易出新 bug。
 *
 * 5) 搜索**不需要拼音索引**：屏幕上的拼音输入法输出的已经是汉字
 *    （ui_search.c 的候选字机制），所以这里只做 UTF-8 子串匹配
 *    （ASCII 部分大小写不敏感）。这一条省掉了整整一套拼音倒排。
 *
 * ⚠️ 本组件只写 ESP-MUSIC.IDX 一个文件，不删不改用户任何音乐文件，
 *    更不碰格式化（挂载参数在 sd_card 里锁死为 false）。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "subsonic.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 曲库容量上限。2000 首 × musiclib_rec_t(≈372B) ≈ 744KB PSRAM（空闲约 5MB，够）。 */
#define MUSICLIB_MAX_SONGS  2000
/* 扫描时最大目录深度（防止卡上有奇怪的软链/循环结构把栈撑爆） */
#define MUSICLIB_MAX_DEPTH  8
/* 索引文件（相对卡根） */
#define MUSICLIB_INDEX_REL  "ESP-MUSIC.IDX"
/* 索引格式版本。改了 rec 布局就 +1 —— 旧索引会被判为不兼容，要求重扫。 */
#define MUSICLIB_INDEX_VER  1

/* ---------- 扫描（后台任务，UI 轮询进度） ---------- */

/* 起一次扫描。已经在下扫则返回 ESP_ERR_INVALID_STATE（不排队）。
 * 扫描会重建整份索引并落盘到卡上；扫完 musiclib_count() 才是新值。 */
esp_err_t musiclib_scan_start(void);
bool      musiclib_scan_busy(void);
int       musiclib_scan_scanned(void);   /* 已遍历的文件数（进度显示用） */
int       musiclib_scan_found(void);     /* 已入库的曲目数 */
esp_err_t musiclib_scan_result(void);    /* 上次扫描的结果码（ESP_OK = 成功） */

/* ---------- 索引装载 / 释放 ---------- */

/* 从卡上读索引。校验不过（没有文件/版本不符/CRC 错）返回非 OK，
 * 上层当作"曲库为空，需要扫描"，而不是错误弹窗。 */
esp_err_t musiclib_load(void);
void      musiclib_clear(void);          /* 释放 PSRAM 索引 */

/* ---------- 查询 ---------- */

int  musiclib_count(void);
/* 按序号取一首（转成 sub_song_t，填好 id / path / suffix / title / artist） */
bool musiclib_get(int idx, sub_song_t *out);
/* 按 id（"L" + 8 位 hex）找 */
bool musiclib_find_by_id(const char *id, sub_song_t *out);
/* 按【完整路径】找（格式与 sub_song_t.path 一致，如 "/sdcard/音乐/x.flac"）。
 * 重扫后回链离线收藏用它 —— 路径没变就还是同一首歌。 */
bool musiclib_find_by_path(const char *path, sub_song_t *out);

/* 随机取 max 首（离线模式的"随机 30 首"后端）。返回真正取到的条数。 */
int  musiclib_pick_random(sub_song_t *out, int max);

/* 子串搜索：title / artist 任一命中即可（UTF-8 字节比较，ASCII 忽略大小写）。
 * 返回写入 out 的条数（上限 max）。 */
int  musiclib_search(const char *query, sub_song_t *out, int max);

/* 用「标题 + 艺术家」在曲库里找匹配项（在线收藏 → 离线收藏同步用）。
 * 归一化规则见 .c（忽略大小写/空白/常见标点）。命中项的 rel_path 写进 paths，
 * 返回命中条数（上限 max）。 */
int  musiclib_match(const char *title, const char *artist,
                    char paths[][SUB_SONG_PATH_MAX], int max);

/* ---------- 纯函数（可无硬件单测） ---------- */

/* 文件名 → 标题 / 艺术家。name 可以是完整 basename（带扩展名）。
 * 规则：去扩展名 → 去前导音轨号 → 按第一个 " - " 切分 → 去两端空白。 */
void musiclib_parse_name(const char *name, char *title, size_t tlen,
                         char *artist, size_t alen);

/* 文件名/路径是否是可播放的音乐扩展名（flac/mp3/aac/m4a/ogg/wav，忽略大小写） */
bool musiclib_is_music_file(const char *name);

/* 文件名解析自检：拿一批合成的名字过一遍 musiclib_parse_name / is_music_file，
 * 逐条打印"通过/失败 + 期望值"。**纯函数、不碰硬件**，串口就能验证 —— 板子还没到
 * 也能把解析规则验掉；板子到了之后第一次烧录也建议顺手看一眼这几十行日志。
 * 由 main/app_main.c 的 MUSICLIB_SELFTEST 开关调用（平时必须是 0）。 */
void musiclib_selftest(void);

/* 诊断：上次 load/scan 的错误码 */
esp_err_t musiclib_last_err(void);

#ifdef __cplusplus
}
#endif
