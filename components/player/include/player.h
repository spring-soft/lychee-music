/*
 * player —— 播放命令层（队列 + 自动连播）
 *
 * 为什么要有这一层：按键、网页（M5）、蓝牙 HID（M7）三路都能控制播放。
 * 如果各自直接调 audio_engine_*，就会出现"两个任务同时换歌、队列各说各话"。
 * 所以约定：
 *   - 所有控制入口都是 player_xxx()，内部只往一条 FreeRTOS 队列里塞命令（不阻塞）；
 *   - 只有 player 任务碰 audio_engine / 队列 / 曲目切换，天然串行；
 *   - UI / 网页只读 player_queue_*() 这些查询接口（内部加锁）。
 *
 * 队列的用途：
 *   - 顺序连播（一首播完自动下一首）
 *   - 列表页浏览（K1/K2 翻行，K3 播放选中）
 *   - 来源切换（最近播放 / 我的收藏 / 随机 30 首）
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "subsonic.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PLAYER_QUEUE_MAX   64   /* 队列最多几首（sub_song_t 约 370B → 24KB PSRAM） */
#define PLAYER_RECENT_MAX  50   /* 最近播放环形缓冲（M6 起落盘到 cache 分区） */
/* 收藏的本地镜像上限。刻意跟 subsonic 的 SUB_SONGS_MAX 对齐 —— getStarred2
 * 本来就只取 64 条，本地留更多也没意义。（M6 前 /api/starred 每次现拉，
 * 现在改成本地镜像 + 后台刷新，于是"没网也能看收藏"。） */
#define PLAYER_STAR_MAX    64
#define PLAYER_STAR_PEND_MAX 16 /* 离线时攒下的待重试收藏操作上限 */

typedef enum {
    PLAYER_SRC_RECENT = 0,   /* 最近播放（本机播放历史，M6 落盘） */
    PLAYER_SRC_STARRED,      /* 我的收藏（Navidrome getStarred2） */
    PLAYER_SRC_RANDOM,       /* 随机 30 首（Navidrome getRandomSongs） */
    PLAYER_SRC_COUNT,
} player_src_t;

const char *player_src_name(player_src_t s);

esp_err_t player_init(void);

/* ---------------- 命令（任任务可调，非阻塞） ---------------- */

/* 换来源并重建队列。autoplay=true 时立刻从第 1 首开始播；
 * autoplay=false 只换列表（正在播的那首不打断，列表页浏览用） */
void player_set_source(player_src_t src, bool autoplay);
void player_set_source_next(int dir, bool autoplay);

void player_play_index(int idx);      /* 播队列第 idx 首（0 基） */
void player_next(void);
void player_prev(void);
void player_toggle(void);             /* 播放/暂停 */
void player_restart(void);            /* 从头重播当前这首 */
void player_star(bool on);            /* 收藏/取消收藏当前歌（写回 Navidrome） */
void player_volume(int vol);          /* 0..100，落到 ES8388（含模拟音量混合），自动存 NVS */
void player_set_mute(bool on);        /* 真·静音：ES8388 DAC 硬静音，不是把音量设成 0 */
void player_set_brightness(int pct);  /* 屏幕背光 5..100（网页和屏幕都改这一个，自动存 NVS） */
void player_seek(uint32_t pos_ms);    /* seek：让服务端从该位置重转（约 1 秒缓冲） */
void player_play_song_id(const char *song_id);      /* 播放队列外的歌（只知道 id，会去服务器取元数据） */
/* 同上，但元数据已经有了 —— **前端点列表时用这个，省掉一次 getSong 网络往返** */
void player_play_song(const sub_song_t *song);
void player_star_song(const char *song_id, bool on);/* 收藏任意一首（离线会排队重试） */

/* 开机：沿用上次的来源 + **续播上次的位置**。等价于 player_set_source(当前来源, true)，
 * 只是多了一步"如果队列第一首就是上次那首，就从上次的位置接着放"。 */
void player_resume(void);

/* ---------------- M8：屏幕上的搜索 ----------------
 *
 * 屏幕上的搜索是"UI 发命令 → player 任务做网络 → UI 轮询结果"。
 * 为什么不让 UI 自己搜：subsonic_search() 是阻塞网络调用（几百毫秒），
 * 而 UI 任务必须随时能画（方案里的四条措施③：网络全在 core0，不跟 UI 抢核1）。
 *
 * 结果是一条独立于播放队列的快照（跟"最近播放/收藏"一样是只读查询）：
 * 搜索不会动当前队列，选中某首才用 player_play_song() 去播。 */
void     player_search(const char *query);   /* 非阻塞：塞进命令队列 */
int      player_search_count(void);          /* 结果条数 */
bool     player_search_get(int idx, sub_song_t *out);
uint32_t player_search_gen(void);            /* 结果变化计数：UI 据此重建列表 */
bool     player_search_busy(void);           /* 正在搜（UI 显示"搜索中…"） */
esp_err_t player_search_err(void);           /* 上次搜索的结果码（区分"没找到"和"网络错"） */
const char *player_search_query(void);       /* 上次搜的词（结果页顶栏显示） */

/* 请求后台把收藏列表跟 Navidrome 对一次（异步，在 player 任务里跑）。
 * force=false 时只有"过期了（>2 分钟）/ 从没成功过 / 有待重试操作"才真拉。 */
void player_starred_refresh(bool force);

/* ---------------- 只读查询（给 UI / 网页用） ---------------- */
int          player_queue_count(void);
int          player_queue_index(void);         /* -1 = 当前歌不在队列里 */
int          player_recent_count(void);        /* 最近播放里已有几条（0 = 还没播过歌） */
bool         player_recent_get(int idx, sub_song_t *out);   /* [0] 是最新的 */
int          player_brightness(void);
bool         player_queue_get(int idx, sub_song_t *out);
uint32_t     player_queue_gen(void);           /* 内容变化计数 */
player_src_t player_source(void);
int          player_volume_get(void);
bool         player_is_muted(void);

/* 收藏的本地镜像（一次拷完，网页列表用它；内部加锁）。
 * synced = 这份跟服务器一致（上次刷新成功且没有待重试的收藏操作）。
 * 返回真正拷进 out 的条数。 */
int          player_starred_list(sub_song_t *out, int max, bool *synced);
uint32_t     player_starred_gen(void);         /* 收藏列表变化计数（网页据此重拉） */
int          player_starred_pending(void);     /* 待重试的收藏操作数（>0 = 还没跟服务器对上） */
/* 上次成功刷新收藏的"距今毫秒"（-1 = 本次开机还没成功过） */
int64_t      player_starred_age_ms(void);

#ifdef __cplusplus
}
#endif
