/*
 * 播放状态快照 —— 各模块之间的唯一共享点
 *
 * 分工（对应方案里的架构约束）：
 *   - 谁换歌谁调 player_status_set_song()（目前是 main 的播放流程，M4 之后是命令层）
 *   - UI / 网页 / 按键都只【读快照】，不互相调用
 *   - 位置与播放状态由消费方（UI 的 250ms 定时器）从音频引擎的原子量同步进来：
 *     引擎的位置本来就是无锁原子读，这里只在换歌这类结构变化时加锁
 *
 * 这样做是为了避免"三路并发控制（按键/网页/蓝牙）+ 各自抓内部状态"长成一团。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char     title[160];
    char     artist[96];
    char     album[96];
    char     cover_id[48];
    uint32_t duration_ms;
    uint32_t position_ms;
    bool     playing;        /* 正在出声（非暂停/非结束） */
    bool     paused;
    bool     finished;
    bool     starred;
    uint32_t gen;            /* 换歌计数：异步结果（封面等）靠它对账，避免串台 */
    uint32_t sample_rate;
    int      volume;         /* 0..100 */

    /* 队列（M4 起）：当前歌在队列里的位置。index = -1 表示"在放，但不在当前队列里"
     * （例如在列表页换了来源、但没打断正在播的那首） */
    int      queue_index;
    int      queue_count;
    uint32_t queue_gen;      /* 队列内容变化计数：列表页靠它决定要不要重建行 */
    char     source[20];     /* 来源名（"最近播放" / "我的收藏" / "随机 30 首"） */
    char     song_id[48];    /* 当前歌 id，收藏/网页控制要用 */
} player_status_t;

void player_status_init(void);

/* 取快照（内部加锁，调用方拿到的是拷贝） */
void player_status_get(player_status_t *out);

/* 换歌：写入歌曲信息并 gen++（封面等异步结果会用 gen 对账） */
void player_status_set_song(const char *song_id, const char *title, const char *artist,
                            const char *album, const char *cover_id, uint32_t duration_ms,
                            bool starred);

/* 只更新位置/播放状态（高频，轻量） */
void player_status_set_progress(uint32_t position_ms, bool playing, bool paused, bool finished);

/* 队列信息。content_changed=true 时 queue_gen++（列表页据此重建行） */
void player_status_set_queue(int index, int count, const char *source, bool content_changed);

void player_status_set_starred(bool starred);
void player_status_set_volume(int vol);

#ifdef __cplusplus
}
#endif
