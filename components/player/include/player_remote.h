/*
 * player_remote —— 把「播放」交给服务器（那台接了 USB DAC 的 OpenStick）
 *
 * 为什么要有这一层：
 *   用户在菜单里把「设备」切成"服务器"之后，歌由服务器那边放（它接 DAC/耳机），
 *   ESP 只当遥控器 + 显示屏。但 UI / 按键 / 网页全都在读 player 的状态和队列 ——
 *   所以最省事也最不容易出错的做法是：**把服务器的状态镜像回本地**，
 *   让 UI 一行都不用改（它照旧读 player_status_* / player_queue_*）。
 *
 * 服务器地址：直接用 QQ 契约层的 host（qqmusic_host()）—— 本来就是同一台机器、
 * 同一个服务（播放接口就挂在它的 /v1/player 下面）。这样用户在网页里改地址，这边自动跟着变。
 *
 * ⚠️ 这里的 HTTP 都是**阻塞**的（1~4 秒），只能在 player 任务里调，绝不能在 UI 任务里调。
 *    UI 只通过 player_status_get()/player_queue_* 拿快照（加锁拷贝，不阻塞）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "subsonic.h"

#ifdef __cplusplus
extern "C" {
#endif

#define REMOTE_QUEUE_MAX 64      /* 跟 player 的 PLAYER_QUEUE_MAX 对齐 */

/* 服务器播放器的一次状态快照 */
typedef struct {
    bool     valid;              /* 上次查询成功（false = 连不上/没在跑） */
    bool     mpv_ok;             /* 服务器那边的播放器进程在不在 */
    char     state[12];          /* playing / paused / stopped */
    uint32_t pos_ms;
    uint32_t dur_ms;
    int      volume;             /* 0..100 */
    int      back;               /* 0 = Navidrome / 1 = QQ 音乐 */
    uint32_t gen;                /* 服务器队列版本（变了才去拉队列明细） */
    char     id[48], title[160], artist[96], album[96], cover[48], suffix[8];
    int      q_index, q_count;
    char     q_mode[32];         /* 中文模式名（"收藏"…）；空 = 没有队列 */
    char     q_mode_key[24];
    char     err[96];
} remote_status_t;

esp_err_t   player_remote_init(void);
void        player_remote_reset(void);                    /* 断开时清状态 */
esp_err_t   player_remote_fetch(void);                    /* 拉一次状态（+需要时拉队列）*/
void        player_remote_status(remote_status_t *out);   /* 加锁拷贝快照 */
uint32_t    player_remote_queue_gen(void);
int         player_remote_queue_copy(sub_song_t *out, int max);
bool        player_remote_online(void);
const char *player_remote_last_err(void);

/* 下面这些是命令，阻塞 HTTP（1~4s），只能在 player 任务里调 */
esp_err_t player_remote_toggle(void);
esp_err_t player_remote_next(void);
esp_err_t player_remote_prev(void);
esp_err_t player_remote_seek(uint32_t pos_ms);
esp_err_t player_remote_volume(int level);
esp_err_t player_remote_stop(void);
esp_err_t player_remote_play_id(int back, const char *id);
/* 按模式建队列并起播：mode_index 是该后端下的第几个模式（名字会去 /v1/player/modes 查） */
esp_err_t player_remote_play_mode(int back, int mode_index, int index, uint32_t pos_ms);

#ifdef __cplusplus
}
#endif
