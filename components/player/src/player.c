/*
 * player —— 播放命令层实现
 *
 * 任务模型：一条命令队列 + 一个 player 任务（core 0, prio 5）。
 *   - core 0 是故意的：命令里会做 HTTP（拉歌单 / 打开流），那可能阻塞几百毫秒；
 *     UI 在 core 1，不能被网络阻塞（方案里的"避免 UI 卡顿四条硬措施"之③）。
 *   - player 任务不碰 LVGL，只改 app_core 里的状态快照，UI 自己定时来读。
 *
 * 位置/暂停等高频状态不在这里，UI 直接读 audio_engine 的原子量。
 */
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "player.h"
#include "app_core.h"
#include "audio_engine.h"
#include "board.h"
#include "settings.h"
#include "store.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

static const char *TAG = "player";

#define PLAYER_TASK_STACK 6144
#define PLAYER_TASK_PRIO  5
#define PLAYER_TASK_CORE  0

/* ---- M6 落盘（cache 分区）----
 * 文件名必须严格 8.3（FATFS 关着长文件名），理由见 store.h。
 * 键名故意用单字母：这些文件每次换歌/收藏都可能重写一遍，省一半字节。 */
#define F_RECENT  "RECENT.RC"
#define F_STAR    "STAR.SC"
#define F_PEND    "PEND.OP"

/* 序列化缓冲：64 首 × 最坏 ~560 字节 ≈ 36KB，留点余量。
 * 读和写共用一份（都在 player 任务里，天然串行）。 */
#define JSON_BUF  (40 * 1024)

#define RECENT_SAVE_MS   10000   /* 最近播放：变了 10 秒后才落盘（掉电最多丢这一小段） */
#define STAR_SAVE_MS     10000
#define STAR_REFRESH_MS  120000  /* 收藏列表多久算"过期，该跟服务器对一次" */
#define PEND_RETRY_MS    60000   /* 待重试收藏操作的退避间隔 */
#define RESUME_SAVE_MS   20000   /* 续播位置写 NVS 的最小间隔（别把 NVS 写爆） */

typedef enum {
    CMD_NEXT = 0, CMD_PREV, CMD_TOGGLE, CMD_PLAY_INDEX, CMD_SET_SOURCE,
    CMD_STAR, CMD_VOLUME, CMD_RESTART, CMD_MUTE, CMD_BRIGHT,
    CMD_SEEK, CMD_PLAY_ID, CMD_STAR_ID,
    CMD_RESUME, CMD_STAR_REFRESH,
    CMD_SEARCH,               /* M8：屏上搜索 */
} cmd_kind_t;

typedef struct {
    cmd_kind_t kind;
    int        iarg;      /* PLAY_INDEX: 下标；SET_SOURCE: 来源；VOLUME: 音量；SEEK: 毫秒 */
    bool       barg;      /* SET_SOURCE: autoplay；STAR: on */
    int        iarg2;     /* SET_SOURCE: dir */
    char       sid[48];   /* PLAY_ID / STAR_ID: 歌曲 id */
    char       query[64]; /* SEARCH: 搜索词（拼音模式下来的是汉字，UTF-8 最多 ~20 字） */
    sub_song_t meta;      /* PLAY_ID: 前端带来的元数据（id 非空就用它，免一次网络往返） */
} player_cmd_t;

static QueueHandle_t     s_q;
static SemaphoreHandle_t s_lock;          /* 保护 s_queue[] / s_index / s_count / s_recent */

static sub_song_t       *s_queue;         /* PSRAM，PLAYER_QUEUE_MAX 首 */
static int               s_count;
static int               s_index = -1;
static uint32_t          s_queue_gen;
static player_src_t      s_src = PLAYER_SRC_RANDOM;

/* 最近播放：环形缓冲，[0] 是最新的 */
static sub_song_t       *s_recent;
static int               s_recent_n;
static volatile bool     s_recent_dirty;      /* 有变化还没落盘 */
static int64_t           s_recent_dirty_ms;   /* 第一次发现有变化的时间（0 = 还没发现） */
static bool              s_recent_loaded;     /* 本次开机从 cache 恢复过 */

/* 收藏的本地镜像（M6）：Navidrome 是权威，这里是"快照 + 离线也能看"。
 * 真正的一致性靠两件事：① 每次改动都推服务器；② 推不出去就排队重试。 */
static sub_song_t       *s_starred;
static int               s_starred_n;
static volatile uint32_t s_starred_gen;       /* 列表内容变化计数（网页据此重拉） */
static int64_t           s_starred_ok_ms = -1;/* 上次成功跟服务器对上（<0 = 本次开机还没成功过） */
static bool              s_starred_dirty;
static int64_t           s_starred_dirty_ms;

/* 离线时攒下的收藏操作（同上，去重后的"最终意图"） */
typedef struct { char id[48]; bool on; } star_op_t;
static star_op_t        *s_pend;
static int               s_pend_n;
static int64_t           s_pend_try_ms;

/* 落盘用的临时缓冲：持锁只做 memcpy，写 flash 在放锁之后 ——
 * 不然 UI 读"最近播放"会被几十毫秒的 flash 写挡住。
 * 四个缓冲一起分配、一起失败，所以用 s_persist_ok 一个标志统一判断，
 * 免得每个入口都写四遍 !NULL（漏一处就是开机就崩的空指针）。 */
static sub_song_t       *s_snap;
static char             *s_json;
static bool              s_persist_ok;

static int64_t           s_resume_ms;     /* 上次写续播位置的时间 */

/* ---- M8：搜索结果快照 ----
 * 独立于播放队列：搜索不动队列，选中某首才去播。
 * ⚠️ 网络调用期间**不能持 s_lock**（几百毫秒会挡住 UI 读队列/最近播放），
 *    所以先搜进 s_found_tmp，搜完再持锁整体换过去。 */
static sub_song_t       *s_found;         /* PSRAM，屏幕要显示的结果 */
static sub_song_t       *s_found_tmp;     /* PSRAM，网络直接写这里（不持锁） */
static int               s_found_n;
static volatile uint32_t s_found_gen;
static volatile bool     s_found_busy;
static volatile esp_err_t s_found_err = ESP_OK;
static char              s_found_q[64];

static ae_source_t      *s_cur_src;       /* 当前 HTTP 源（换歌时释放上一首） */
static bool              s_active;        /* 用户意图 = 想让它播 */
static int               s_err_streak;
static int               s_volume = 40;

/* 音量有两份：s_volume 是 player 任务真正写进 ES8388 的值，s_vol_target 是"请求值"。
 * 界面必须看请求值 —— 按键 → 队列 → player 任务 → I2C 是异步的，界面如果读 s_volume，
 * 会慢一拍（按一下加 5，屏幕上还是旧数字）。 */
static volatile int      s_vol_target = 40;
static volatile bool     s_mute_target;   /* 同上：界面读请求值，不等 I2C 写完 */
static volatile int      s_bright_target = 100;

/* 临时静音解开时，要恢复成"用户是否静音"而不是无条件解开 */
static void restore_mute(void) { board_es8388_mute(board_es8388_is_muted()); }

const char *player_src_name(player_src_t s)
{
    switch (s) {
        case PLAYER_SRC_RECENT:  return "最近播放";
        case PLAYER_SRC_STARRED: return "我的收藏";
        case PLAYER_SRC_RANDOM:  return "随机 30 首";
        default:                 return "?";
    }
}

/* ============================================================ 落盘（cache 分区）
 *
 * 存的是"必要子集"不是整个 sub_song_t：size / suffix 只是嗅探与 seek 的提示，
 * 丢了也能靠头几个字节重新嗅出来，所以只在有值时写。
 * 键名是单字母（t=title, a=artist, b=album, c=cover, d=duration, z=size,
 * x=suffix, s=starred）—— 文件每次换歌都要重写整份，能省一半体积。
 */

static void song_to_json(cJSON *arr, const sub_song_t *s)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", s->id);
    cJSON_AddStringToObject(o, "t",  s->title);
    cJSON_AddStringToObject(o, "a",  s->artist);
    cJSON_AddStringToObject(o, "b",  s->album);
    cJSON_AddStringToObject(o, "c",  s->cover);
    cJSON_AddNumberToObject(o, "d",  s->duration_s);
    if (s->size)        cJSON_AddNumberToObject(o, "z", (double)s->size);
    if (s->suffix[0])   cJSON_AddStringToObject(o, "x", s->suffix);
    if (s->starred)     cJSON_AddBoolToObject(o, "s", true);
    cJSON_AddItemToArray(arr, o);
}

static bool song_from_json(const cJSON *o, sub_song_t *s)
{
    memset(s, 0, sizeof(*s));
    const cJSON *it = cJSON_GetObjectItem(o, "id");
    if (!cJSON_IsString(it) || it->valuestring[0] == 0) return false;
    strlcpy(s->id, it->valuestring, sizeof(s->id));

    #define GETSTR(k, f) do { const cJSON *_i = cJSON_GetObjectItem(o, k); \
        if (cJSON_IsString(_i)) strlcpy(s->f, _i->valuestring, sizeof(s->f)); } while (0)
    GETSTR("t", title);  GETSTR("a", artist); GETSTR("b", album);
    GETSTR("c", cover);  GETSTR("x", suffix);
    #undef GETSTR

    const cJSON *j = cJSON_GetObjectItem(o, "d");
    if (cJSON_IsNumber(j)) s->duration_s = (uint32_t)j->valuedouble;
    j = cJSON_GetObjectItem(o, "z");
    if (cJSON_IsNumber(j)) s->size = (uint32_t)j->valuedouble;
    s->starred = cJSON_IsTrue(cJSON_GetObjectItem(o, "s"));
    return true;
}

/* 原子写一份歌曲数组。序列化 + 写盘，**不在锁里做**（会挡住 UI 读列表）。
 * 结果比 JSON_BUF 还大时逐条少存（宁可少几条，也不能写出一个自己读不回来的文件）。 */
static bool list_save(const char *file, const sub_song_t *v, int n)
{
    if (!store_ready() || v == NULL) return false;

    char *js = NULL;
    int m = n;
    while (m >= 0) {
        cJSON *arr = cJSON_CreateArray();
        for (int i = 0; i < m; i++) song_to_json(arr, &v[i]);
        js = cJSON_PrintUnformatted(arr);
        cJSON_Delete(arr);
        if (js == NULL) return false;
        if (strlen(js) < JSON_BUF) break;
        free(js);
        js = NULL;
        if (m == 0) return false;
        m = (m > 8) ? m - 8 : 0;
    }
    if (m < n) ESP_LOGW(TAG, "%s 太大，只存了 %d/%d 条", file, m, n);

    esp_err_t err = store_write(file, js, strlen(js));
    free(js);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s 落盘失败: %s", file, esp_err_to_name(err));
        return false;
    }
    return true;
}

static int list_load(const char *file, sub_song_t *out, int max)
{
    if (!store_ready() || s_json == NULL) return 0;
    size_t len = 0;
    if (store_read(file, s_json, JSON_BUF - 1, &len) != ESP_OK) return 0;  /* 没有很正常 */
    s_json[len] = 0;
    cJSON *arr = cJSON_ParseWithLength(s_json, len);
    if (arr == NULL) {
        ESP_LOGE(TAG, "%s 解析失败（%u 字节），当空处理", file, (unsigned)len);
        return 0;
    }
    int n = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (n >= max) break;
        if (song_from_json(it, &out[n])) n++;
    }
    cJSON_Delete(arr);
    return n;
}

/* ---- 最近播放 ---- */

static void recent_save(void)
{
    if (!s_persist_ok) { s_recent_dirty = false; return; }
    int n;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    n = s_recent_n;
    if (n > 0) memcpy(s_snap, s_recent, (size_t)n * sizeof(sub_song_t));
    s_recent_dirty = false;
    xSemaphoreGive(s_lock);

    if (list_save(F_RECENT, s_snap, n)) {
        ESP_LOGI(TAG, "最近播放已落盘（%d 条）", n);
    }
}

/* 变了以后静置一会儿再写：连续换歌（长按 K1）时不会每首写一次 flash */
static void recent_save_tick(void)
{
    if (!s_recent_dirty) {
        s_recent_dirty_ms = 0;
        return;
    }
    int64_t now = esp_timer_get_time() / 1000;
    if (s_recent_dirty_ms == 0) {
        s_recent_dirty_ms = now;
        return;
    }
    if (now - s_recent_dirty_ms < RECENT_SAVE_MS) return;
    recent_save();
}

/* ---- 收藏（本地镜像） ---- */

static void starred_save(void)
{
    if (!store_ready() || !s_persist_ok) return;
    int n;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    n = s_starred_n;
    if (n > 0) memcpy(s_snap, s_starred, (size_t)n * sizeof(sub_song_t));
    s_starred_dirty = false;
    xSemaphoreGive(s_lock);

    if (list_save(F_STAR, s_snap, n)) ESP_LOGI(TAG, "收藏已落盘（%d 首）", n);
}

static void starred_save_tick(void)
{
    if (!s_starred_dirty) {
        s_starred_dirty_ms = 0;
        return;
    }
    int64_t now = esp_timer_get_time() / 1000;
    if (s_starred_dirty_ms == 0) {
        s_starred_dirty_ms = now;
        return;
    }
    if (now - s_starred_dirty_ms < STAR_SAVE_MS) return;
    starred_save();
}

/* ---- 待重试的收藏操作 ---- */

static void pend_save(void)
{
    if (!store_ready() || !s_persist_ok) return;
    int n;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    n = s_pend_n;
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", s_pend[i].id);
        cJSON_AddBoolToObject(o, "o", s_pend[i].on);
        cJSON_AddItemToArray(arr, o);
    }
    xSemaphoreGive(s_lock);

    char *js = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (js == NULL) return;
    store_write(F_PEND, js, strlen(js));
    free(js);
}

static void pend_load(void)
{
    if (!store_ready() || s_json == NULL) return;
    size_t len = 0;
    if (store_read(F_PEND, s_json, JSON_BUF - 1, &len) != ESP_OK) return;
    s_json[len] = 0;
    cJSON *arr = cJSON_ParseWithLength(s_json, len);
    if (arr == NULL) return;
    int n = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (n >= PLAYER_STAR_PEND_MAX) break;
        const cJSON *id = cJSON_GetObjectItem(it, "id");
        if (!cJSON_IsString(id) || id->valuestring[0] == 0) continue;
        strlcpy(s_pend[n].id, id->valuestring, sizeof(s_pend[0].id));
        s_pend[n].on = cJSON_IsTrue(cJSON_GetObjectItem(it, "o"));
        n++;
    }
    cJSON_Delete(arr);
    s_pend_n = n;
}

/* 排一条待重试。同一首只留最后的意图（先收藏又取消 = 什么都不用发） */
static void pend_add(const char *id, bool on)
{
    if (!s_persist_ok) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int at = -1;
    for (int i = 0; i < s_pend_n; i++) {
        if (!strcmp(s_pend[i].id, id)) { at = i; break; }
    }
    if (at >= 0) {
        if (s_pend[at].on == on) { xSemaphoreGive(s_lock); return; }   /* 已经排了同样的 */
        s_pend[at].on = on;
    } else if (s_pend_n < PLAYER_STAR_PEND_MAX) {
        strlcpy(s_pend[s_pend_n].id, id, sizeof(s_pend[0].id));
        s_pend[s_pend_n].on = on;
        s_pend_n++;
    } else {
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "待重试收藏队列满（%d），丢弃这条", PLAYER_STAR_PEND_MAX);
        return;
    }
    xSemaphoreGive(s_lock);
}

/* 把攒下的操作补发出去。成功一条删一条；第一条失败就停（留到下次）。
 * ⚠️ 会做网络请求，只能在 player 任务里调。 */
static void pend_flush(void)
{
    if (!s_persist_ok || s_pend_n == 0) return;
    for (;;) {
        char id[48] = {0};
        bool on;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_pend_n == 0) { xSemaphoreGive(s_lock); break; }
        strlcpy(id, s_pend[0].id, sizeof(id));
        on = s_pend[0].on;
        xSemaphoreGive(s_lock);

        esp_err_t err = subsonic_star(id, on);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "收藏操作还是发不出去（剩 %d 条待重试）", player_starred_pending());
            break;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int k = 0; k < s_pend_n - 1; k++) s_pend[k] = s_pend[k + 1];
        s_pend_n--;
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "补发收藏操作：%s %s", on ? "收藏" : "取消收藏", id);
    }
    pend_save();
}

static void pend_retry_tick(void)
{
    if (s_pend_n == 0) return;
    int64_t now = esp_timer_get_time() / 1000;
    if (now - s_pend_try_ms < PEND_RETRY_MS) return;
    s_pend_try_ms = now;
    pend_flush();
}

/* 在本地镜像里落实一次收藏/取消（界面立刻就对，不等服务器）。
 * song 至少要带 id；收藏一首元数据不全的歌时先记 id，等刷新时服务器补全。 */
static void starred_local_apply(const sub_song_t *song, bool on)
{
    if (!s_persist_ok) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);

    int at = -1;
    for (int i = 0; i < s_starred_n; i++) {
        if (!strcmp(s_starred[i].id, song->id)) { at = i; break; }
    }
    if (at >= 0 && !on) {
        for (int k = at; k < s_starred_n - 1; k++) s_starred[k] = s_starred[k + 1];
        s_starred_n--;
    } else if (at >= 0) {
        s_starred[at].starred = true;
        if (at > 0) {                    /* 新收藏的排最前，和 Navidrome 的排序观感一致 */
            sub_song_t t = s_starred[at];
            for (int k = at; k > 0; k--) s_starred[k] = s_starred[k - 1];
            s_starred[0] = t;
        }
    } else if (on && song->title[0] && s_starred_n < PLAYER_STAR_MAX) {
        for (int k = s_starred_n; k > 0; k--) s_starred[k] = s_starred[k - 1];
        s_starred[0] = *song;
        s_starred[0].starred = true;
        s_starred_n++;
    } else if (on) {
        ESP_LOGW(TAG, "收藏的歌 %s 缺元数据，等下次刷新时从服务器补", song->id);
    }

    s_starred_gen++;
    s_starred_dirty = true;
    xSemaphoreGive(s_lock);
}

/* 跟服务器对一次（权威）。成功 = 整个镜像替换掉。只在 player 任务里调。 */
static bool starred_refresh(void)
{
    if (!s_persist_ok) return false;
    int got = 0;
    esp_err_t err = subsonic_get_starred(s_snap, PLAYER_STAR_MAX, &got);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "拉收藏列表失败: %s（继续用本地镜像 %d 首）",
                 esp_err_to_name(err), s_starred_n);
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (got > 0) memcpy(s_starred, s_snap, (size_t)got * sizeof(sub_song_t));
    s_starred_n = got;
    s_starred_gen++;
    s_starred_dirty = true;
    xSemaphoreGive(s_lock);

    s_starred_ok_ms = esp_timer_get_time() / 1000;
    ESP_LOGW(TAG, "收藏列表已同步：%d 首", got);
    return true;
}

/* 需要的话对一次。⚠️ 先补发本地改动再拉，否则拉回来的列表会把本地刚收藏的"吞掉" */
static void starred_ensure(bool force)
{
    int64_t now = esp_timer_get_time() / 1000;
    bool stale = (s_starred_ok_ms < 0) || (now - s_starred_ok_ms > STAR_REFRESH_MS);
    if (!force && !stale && player_starred_pending() == 0) return;
    pend_flush();
    starred_refresh();
}

/* 把镜像拷一份（持锁只做 memcpy） */
static int starred_copy(sub_song_t *out, int max, bool *synced)
{
    if (s_lock == NULL || !s_persist_ok) {
        if (synced) *synced = false;
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_starred_n < max ? s_starred_n : max;
    if (n > 0 && out) memcpy(out, s_starred, (size_t)n * sizeof(sub_song_t));
    int pend = s_pend_n;
    xSemaphoreGive(s_lock);
    if (synced) *synced = (pend == 0 && s_starred_ok_ms >= 0);
    return n;
}

/* ---- 续播位置 ----
 * 只用一个 NVS 键 "resume"，值是 "<歌曲id>|<毫秒>"。为什么打包成一个字符串：
 * 拆成两个键的话，"换成新歌"和"位置归零"是两次写入，中间掉电就会变成
 * "新歌 + 上一首的位置"。一个键一次写入，不存在这种中间态。 */
static void resume_save_now(void)
{
    player_status_t st;
    player_status_get(&st);
    if (st.song_id[0] == 0) return;
    char buf[64];
    snprintf(buf, sizeof(buf), "%.47s|%" PRIu32, st.song_id, audio_engine_position_ms());
    settings_set_str("resume", buf);
}

static void resume_reset(const char *song_id)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%.47s|0", song_id ? song_id : "");
    settings_set_str("resume", buf);
    s_resume_ms = esp_timer_get_time() / 1000;
}

/* 每 20 秒记一次位置。暂停时不写（位置本来就不动），太靠头尾也不写（没意义）。 */
static void resume_tick(void)
{
    if (!s_active || audio_engine_is_paused()) return;
    if (audio_engine_state() != AE_STATE_PLAYING) return;
    int64_t now = esp_timer_get_time() / 1000;
    if (now - s_resume_ms < RESUME_SAVE_MS) return;

    player_status_t st;
    player_status_get(&st);
    uint32_t pos = audio_engine_position_ms();
    if (st.song_id[0] == 0 || pos < 3000) return;
    if (st.duration_ms > 0 && pos + 5000 > st.duration_ms) return;

    s_resume_ms = now;
    resume_save_now();
    ESP_LOGI(TAG, "续播位置已记：%s @ %" PRIu32 "ms", st.song_id, pos);
}

/* 在【恢复出来的队列】里找上次在放的那首。
 *
 * 为什么是"找"而不是"看第 0 首"：来源也是恢复的（比如"我的收藏"），
 * 但用户上次可能听的是列表里的第 7 首 —— 只看第 0 首的话，重启后会
 * 高高兴兴从第 1 首从头放，等于"恢复了曲目"这件事根本没发生。
 *
 * 返回队列下标（-1 = 上次那首不在这个队列里，调用方从第一首放）；
 * *from_out 是要接的位置（0 = 从头）。位置太靠头/靠尾都只定位不接位置。 */
static int resume_find(int count, uint32_t *from_out)
{
    *from_out = 0;
    char buf[64] = {0};
    if (!settings_get_str("resume", buf, sizeof(buf), "")) return -1;
    char *bar = strchr(buf, '|');
    if (bar == NULL || buf[0] == 0) return -1;
    *bar = 0;
    long pos = strtol(bar + 1, NULL, 10);

    for (int i = 0; i < count; i++) {
        sub_song_t s;
        if (!player_queue_get(i, &s)) break;
        if (strcmp(s.id, buf) != 0) continue;
        if (pos >= 5000 && (s.duration_s == 0 || (uint32_t)pos + 5000 < s.duration_s * 1000)) {
            *from_out = (uint32_t)pos;
        }
        return i;
    }
    return -1;
}

/* ============================================================ 最近播放 */

static void recent_push(const sub_song_t *s)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);

    /* ★ 先去重：同一首歌反复播放只保留【最新那一次】。
     *   不去重的话"最近播放"里会出现一串同一首歌（用户实测报的）。
     *   做法：找到旧的那条就地删掉（后面的整体前移），再当新记录插到队首 —— 
     *   位置就自然变成"最近听的排最前"。 */
    int at = -1;
    for (int i = 0; i < s_recent_n; i++) {
        if (!strcmp(s_recent[i].id, s->id)) { at = i; break; }
    }
    if (at >= 0) {
        for (int i = at; i < s_recent_n - 1; i++) s_recent[i] = s_recent[i + 1];
        s_recent_n--;
    }
    if (s_recent_n < PLAYER_RECENT_MAX) s_recent_n++;
    /* 整体后移一格，新的放 [0] */
    for (int i = s_recent_n - 1; i > 0; i--) s_recent[i] = s_recent[i - 1];
    s_recent[0] = *s;
    s_recent_dirty = true;          /* M6：落盘由 recent_save_tick 去抖处理 */
    xSemaphoreGive(s_lock);
}

/* ============================================================ 队列装载 */

static void queue_store(const sub_song_t *songs, int n, player_src_t src, bool content_changed)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (n > PLAYER_QUEUE_MAX) n = PLAYER_QUEUE_MAX;
    if (n > 0) memcpy(s_queue, songs, (size_t)n * sizeof(sub_song_t));
    s_count = n;
    s_src   = src;
    if (content_changed) s_queue_gen++;
    xSemaphoreGive(s_lock);
    /* 队列换了：把"当前歌在队列里的位置"重新算一遍 */
    if (content_changed) {
        player_status_set_queue(s_index, n, player_src_name(src), true);
        /* 来源是"设置"，要持久化 —— 不然重启后又回到随机 30 首 */
        settings_set_int_lazy("src", (int)src);
    }
}

/* 按来源拉取歌单（会阻塞几百毫秒，只在 player 任务里调） */
static bool load_source(player_src_t src, bool autoplay, int *first_idx)
{
    static sub_song_t *tmp;       /* 复用，避免每次 malloc */
    if (tmp == NULL) {
        tmp = heap_caps_malloc(PLAYER_QUEUE_MAX * sizeof(sub_song_t), MALLOC_CAP_SPIRAM);
        if (tmp == NULL) {
            ESP_LOGE(TAG, "歌单临时缓冲分配失败（%u KB）",
                     (unsigned)(PLAYER_QUEUE_MAX * sizeof(sub_song_t) / 1024));
            return false;
        }
    }

    int got = 0;
    esp_err_t err;
    if (src == PLAYER_SRC_RECENT) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        int n = s_recent_n;
        if (n > 0) memcpy(tmp, s_recent, (size_t)n * sizeof(sub_song_t));
        xSemaphoreGive(s_lock);
        got = n;
        err = n > 0 ? ESP_OK : ESP_FAIL;
        ESP_LOGI(TAG, "来源「最近播放」: %d 首", n);
    } else if (src == PLAYER_SRC_STARRED) {
        /* 切到"我的收藏"= 用户想看最新的，强制跟服务器对一次；
         * 对不上（没网）就用本地镜像 —— M6 之前这里会直接失败，收藏来源变空。 */
        starred_ensure(true);
        bool synced = false;
        got = starred_copy(tmp, PLAYER_QUEUE_MAX, &synced);
        err = got > 0 ? ESP_OK : ESP_FAIL;
        ESP_LOGI(TAG, "来源「我的收藏」: %d 首%s", got, synced ? "" : "（本地镜像，未与服务器对上）");
    } else {
        err = subsonic_get_random(tmp, 30, &got);
    }

    if (err != ESP_OK || got <= 0) {
        ESP_LOGE(TAG, "来源「%s」拉取失败（%s，%d 首）—— 保留原队列",
                 player_src_name(src), esp_err_to_name(err), got);
        return false;
    }
    queue_store(tmp, got, src, true);
    *first_idx = 0;
    if (autoplay) {
        ESP_LOGW(TAG, "来源切到「%s」（%d 首）", player_src_name(src), got);
    } else {
        ESP_LOGW(TAG, "来源切到「%s」（%d 首，列表页浏览，不打断当前播放）",
                 player_src_name(src), got);
    }
    return true;
}

/* ============================================================ 起播某一首 */

/* from_ms > 0 = seek：让服务端从这一毫秒开始转码 */
static void start_index_from(int idx, uint32_t from_ms)
{
    int count;
    sub_song_t song;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    count = s_count;
    if (idx < 0 || idx >= count) {
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "起播下标越界: %d（队列 %d 首）", idx, count);
        s_active = false;
        return;
    }
    s_index = idx;
    song = s_queue[idx];
    xSemaphoreGive(s_lock);

    /* 换歌：静音 → 停旧 → 起新 → 解除静音。不加静音会听到断点"咔"一声
     * （旧 PCM 还在 DMA 里，新的一来波形直接跳变）。 */
    board_es8388_mute(true);
    audio_engine_stop();                    /* 内部等 120ms 让三个任务退出 */
    vTaskDelay(pdMS_TO_TICKS(60));           /* 让 DMA 里剩的那点旧音频放完 */

    ae_source_t *old = s_cur_src;
    s_cur_src = NULL;

    char url[768];
    uint32_t off_s = from_ms / 1000;
    if (off_s > 0) {
        subsonic_stream_url_at(&song, 0, off_s, url, sizeof(url));
    } else {
        subsonic_stream_url(&song, 0, url, sizeof(url));
    }
    ae_source_t *src = ae_source_http_new(url);
    if (src == NULL) {
        ESP_LOGE(TAG, "创建 HTTP 源失败");
        restore_mute();
        s_active = false;
        if (old) ae_source_http_free(old);
        return;
    }

    /* 先登记到快照（UI/网页立刻看到新歌名），再开流 */
    player_status_set_song(song.id, song.title, song.artist, song.album, song.cover,
                           song.duration_s * 1000, song.starred);
    player_status_set_queue(idx, count, player_src_name(s_src), false);

    s_cur_src = src;
    s_err_streak = 0;
    audio_engine_play(src);                  /* 内部会 stop 掉旧的（已经提前 stop 过了） */
    /* ⚠️ 位置基准要在 play 之后设（play 会把基准清 0），否则 seek 后位置会从 0 重新数 */
    if (from_ms > 0) audio_engine_set_pos_base(from_ms);
    restore_mute();

    if (old) ae_source_http_free(old);        /* 旧源现在才能安全释放 */
    if (from_ms == 0) {
        recent_push(&song);                   /* seek 不算"又听了一遍新歌" */
        resume_reset(song.id);                /* 换歌了：续播位置归零，别把上一首的位置带过来 */
    }

    if (from_ms > 0) {
        ESP_LOGW(TAG, "seek 到 %" PRIu32 "s：[%d/%d] %s - %s",
                 off_s, idx + 1, count, song.title, song.artist);
    } else {
        ESP_LOGW(TAG, "▶ [%d/%d] %s - %s (%" PRIu32 "s, %s)%s",
                 idx + 1, count, song.title, song.artist, song.duration_s, song.suffix,
                 song.starred ? " ★" : "");
    }
}

static void start_index(int idx) { start_index_from(idx, 0); }

/* 按歌曲 id 播放：队列里有就跳过去，没有（搜索/收藏里点的）就取回元数据、
 * 追加到队尾再播 —— 这样"下一首"还能继续往下走，队列也自然变成"我播过的" */
static void play_song_id(const char *id)
{
    int count;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    count = s_count;
    for (int i = 0; i < count; i++) {
        if (!strcmp(s_queue[i].id, id)) {
            xSemaphoreGive(s_lock);
            s_active = true;
            start_index(i);
            return;
        }
    }
    xSemaphoreGive(s_lock);

    sub_song_t song;
    if (subsonic_get_song(id, &song) != ESP_OK) {
        ESP_LOGE(TAG, "取不到歌曲 %s（网络？）", id);
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int slot = s_count;
    if (slot < PLAYER_QUEUE_MAX) {
        s_queue[slot] = song;
        s_count++;
        s_queue_gen++;
    } else {
        /* 队列满了：退回"单曲队列"，至少能播出来 */
        s_queue[0] = song;
        s_count = 1;
        slot = 0;
        s_queue_gen++;
    }
    int n = s_count;
    xSemaphoreGive(s_lock);
    player_status_set_queue(slot, n, player_src_name(s_src), true);
    s_active = true;
    start_index(slot);
}

/* 直接给元数据播放（网页把列表里已有的字段带过来了）——省掉一次 getSong 往返 */
static void play_song_meta(const sub_song_t *song)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int count = s_count;
    for (int i = 0; i < count; i++) {
        if (!strcmp(s_queue[i].id, song->id)) {
            xSemaphoreGive(s_lock);
            s_active = true;
            start_index(i);
            return;
        }
    }
    int slot = s_count;
    if (slot < PLAYER_QUEUE_MAX) {
        s_queue[slot] = *song;
        s_count++;
    } else {
        s_queue[0] = *song;      /* 队列满了：退回单曲队列，至少能播 */
        s_count = 1;
        slot = 0;
    }
    s_queue_gen++;
    int n = s_count;
    xSemaphoreGive(s_lock);
    player_status_set_queue(slot, n, player_src_name(s_src), true);
    s_active = true;
    start_index(slot);
}

/* 收藏任意一首（M6：本地先改、再推服务器，推不出去就排队重试）
 *
 * 顺序是刻意的：先改本地镜像 → 界面立刻翻心形，不等网络（Navidrome 的
 * star 请求在这个局域网上要 100~300ms）；服务器那次失败也不再"整件事失败"，
 * 而是进重试队列 —— 以前的实现是"写不进去就什么都不做"，用户在没网时
 * 按收藏会看起来毫无反应。 */
static void star_song_id(const char *id, bool on)
{
    if (id == NULL || id[0] == 0) return;

    /* 1) 先凑出这首歌的元数据：本地镜像要存歌名/封面，否则"我的收藏"里是一条空行 */
    sub_song_t song;
    memset(&song, 0, sizeof(song));
    strlcpy(song.id, id, sizeof(song.id));

    player_status_t st;
    player_status_get(&st);
    bool is_current = (strcmp(st.song_id, id) == 0);
    if (is_current) {
        strlcpy(song.title,  st.title,    sizeof(song.title));
        strlcpy(song.artist, st.artist,   sizeof(song.artist));
        strlcpy(song.album,  st.album,    sizeof(song.album));
        strlcpy(song.cover,  st.cover_id, sizeof(song.cover));
        song.duration_s = st.duration_ms / 1000;
        player_status_set_starred(on);
    } else {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool found = false;
        for (int i = 0; i < s_count; i++) {                       /* 队列里的那首 */
            if (!strcmp(s_queue[i].id, id)) { song = s_queue[i]; found = true; break; }
        }
        if (!found) {                                            /* 收藏列表里直接取消收藏 */
            for (int i = 0; i < s_starred_n; i++) {
                if (!strcmp(s_starred[i].id, id)) { song = s_starred[i]; found = true; break; }
            }
        }
        for (int i = 0; i < s_count; i++) {                       /* 队列那份拷贝的心形也要翻 */
            if (!strcmp(s_queue[i].id, id)) s_queue[i].starred = on;
        }
        xSemaphoreGive(s_lock);
        if (!found) ESP_LOGW(TAG, "收藏的 %s 不在队列/收藏里，只有 id（元数据等刷新时从服务器补）", id);
    }

    /* 2) 本地镜像先落实（界面立刻对），3) 再推服务器 */
    starred_local_apply(&song, on);
    starred_save();

    esp_err_t err = subsonic_star(id, on);
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "%s收藏: %s", on ? "已" : "已取消", id);
    } else {
        ESP_LOGW(TAG, "%s收藏没发到服务器（%s），已排队等联网后重试: %s",
                 on ? "已" : "已取消", esp_err_to_name(err), id);
        pend_add(id, on);
        pend_save();
    }
}

static void next_or_wrap(int dir)
{
    int count, idx;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    count = s_count;
    idx   = s_index;
    xSemaphoreGive(s_lock);
    if (count <= 0) return;
    if (idx < 0) idx = dir > 0 ? -1 : 0;      /* 不在队列里：从头/尾算起 */
    int n = (idx + dir + count) % count;
    start_index(n);
}

/* ============================================================ 音乐来源 */

/* 装载来源队列；取不到就退回随机 30 首（不然设备成哑巴）。成功返回 true */
static bool load_source_chain(player_src_t src, int *first)
{
    if (load_source(src, false, first)) return true;
    if (src == PLAYER_SRC_RANDOM) return false;
    ESP_LOGW(TAG, "来源「%s」取不到歌，退回随机 30 首", player_src_name(src));
    return load_source(PLAYER_SRC_RANDOM, false, first);
}

static void handle_set_source(int src, bool autoplay)
{
    if (src < 0 || src >= PLAYER_SRC_COUNT) return;
    int first = 0;
    if (!load_source_chain((player_src_t)src, &first)) return;
    if (autoplay) {
        s_active = true;
        start_index(first);
    } else {
        /* 列表页浏览：不动播放，但如果当前歌恰好在新队列里，把位置标出来 */
        player_status_set_queue(-1, player_queue_count(), player_src_name((player_src_t)src), false);
    }
}

/* 开机：沿用上次来源，并且**接着上次那首、那个位置放**（M6：重启恢复上次曲目与位置）。
 * 上次那首不在恢复出来的队列里（换了来源、收藏被清空）才退回第一首。 */
static void handle_resume(void)
{
    int first = 0;
    if (!load_source_chain(s_src, &first)) return;

    uint32_t from = 0;
    int idx = resume_find(player_queue_count(), &from);
    if (idx < 0) idx = first;

    s_active = true;
    if (from > 0) {
        start_index_from(idx, from);
        player_status_t st;
        player_status_get(&st);
        ESP_LOGW(TAG, "续播：队列第 %d 首 %s 从 %" PRIu32 "ms 接着放（上次没听完）",
                 idx + 1, st.title, from);
    } else {
        if (idx > 0) ESP_LOGW(TAG, "续播：回到上次那首（队列第 %d 首）", idx + 1);
        start_index(idx);
    }
}

/* ============================================================ M8：搜索 */

/* 真正去搜。只在 player 任务里跑（会阻塞做网络）。 */
static void do_search(const char *q)
{
    if (q == NULL || q[0] == 0) return;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_found_q, q, sizeof(s_found_q));
    s_found_busy = true;
    xSemaphoreGive(s_lock);

    ESP_LOGW(TAG, "搜索 \"%s\"…", q);
    int got = 0;
    esp_err_t err = s_found_tmp ? subsonic_search(q, s_found_tmp, SUB_SONGS_MAX, &got)
                                : ESP_ERR_NO_MEM;

    /* 搜完了整体换过去（持锁只做 memcpy）*/
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (err == ESP_OK && got > 0) {
        memcpy(s_found, s_found_tmp, (size_t)got * sizeof(sub_song_t));
    }
    s_found_n = (err == ESP_OK) ? got : 0;
    s_found_err = err;
    s_found_busy = false;
    s_found_gen++;
    xSemaphoreGive(s_lock);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "搜索失败: %s", esp_err_to_name(err));
    } else {
        ESP_LOGW(TAG, "搜索 \"%s\"：%d 首%s", q, got, got ? "" : "（没找到）");
    }
}

/* ============================================================ 命令分发 */

static void handle_cmd(const player_cmd_t *c)
{
    switch (c->kind) {
        case CMD_NEXT:
            s_active = true;
            next_or_wrap(+1);
            break;
        case CMD_PREV:
            s_active = true;
            next_or_wrap(-1);
            break;
        case CMD_TOGGLE: {
            ae_state_t st = audio_engine_state();
            if (audio_engine_is_paused()) {
                audio_engine_pause(false);
                ESP_LOGI(TAG, "继续播放");
            } else if (st == AE_STATE_PLAYING || st == AE_STATE_BUFFERING) {
                audio_engine_pause(true);
                ESP_LOGI(TAG, "暂停");
            } else if (st == AE_STATE_FINISHED || st == AE_STATE_IDLE) {
                s_active = true;
                if (player_queue_count() > 0) start_index(player_queue_index() >= 0
                                                          ? player_queue_index() : 0);
            }
            break;
        }
        case CMD_RESTART:
            s_active = true;
            start_index(player_queue_index() >= 0 ? player_queue_index() : 0);
            break;
        case CMD_PLAY_INDEX:
            s_active = true;
            start_index(c->iarg);
            break;
        case CMD_SET_SOURCE:
            handle_set_source(c->iarg, c->barg);
            break;
        case CMD_RESUME:
            handle_resume();
            break;
        case CMD_STAR_REFRESH:
            starred_ensure(c->barg);
            break;
        case CMD_SEARCH:
            do_search(c->query);
            break;
        case CMD_STAR: {
            player_status_t st;
            player_status_get(&st);
            if (!st.song_id[0]) break;
            star_song_id(st.song_id, c->barg);
            break;
        }
        case CMD_STAR_ID:
            star_song_id(c->sid, c->barg);
            break;
        case CMD_PLAY_ID:
            s_active = true;
            if (c->meta.id[0]) play_song_meta(&c->meta);
            else               play_song_id(c->sid);
            break;
        case CMD_SEEK: {
            int idx = player_queue_index();
            if (idx < 0 || player_queue_count() <= 0) {
                ESP_LOGW(TAG, "不在队列里，无法 seek");
                break;
            }
            /* ⚠️ 转码流没有 Accept-Ranges（实测），seek 只能让服务端从 timeOffset
             *    重转一遍 —— 所以本质是"重新起播 + 位置基准"，约 1 秒缓冲等待。 */
            s_active = true;
            start_index_from(idx, (uint32_t)c->iarg);
            break;
        }

        case CMD_VOLUME:
            s_volume = c->iarg < 0 ? 0 : (c->iarg > 100 ? 100 : c->iarg);
            board_vol_set(s_volume);
            player_status_set_volume(s_mute_target ? 0 : s_volume);
            settings_set_int_lazy("vol", s_volume);   /* 去抖落盘：长按连调不会写爆 NVS */
            ESP_LOGI(TAG, "音量 %d%%", s_volume);
            break;

        case CMD_BRIGHT:
            s_bright_target = c->iarg < 5 ? 5 : (c->iarg > 100 ? 100 : c->iarg);
            board_backlight_set(s_bright_target);
            settings_set_int_lazy("bright", s_bright_target);
            ESP_LOGI(TAG, "亮度 %d%%", s_bright_target);
            break;

        case CMD_MUTE:
            /* ★ 必须用 ES8388 的 DAC 硬静音，不能只把音量设成 0：
             *   音量 0 = 模拟 -45dB + 数字 -15dB，用户反馈"不是真静音"（还有声音）。 */
            board_es8388_set_mute(c->barg);
            player_status_set_volume(c->barg ? 0 : s_volume);
            break;
    }
}

static void player_task(void *arg)
{
    ESP_LOGI(TAG, "player 任务启动（core %d, prio %d）", PLAYER_TASK_CORE, PLAYER_TASK_PRIO);
    for (;;) {
        player_cmd_t c;
        if (xQueueReceive(s_q, &c, pdMS_TO_TICKS(200)) == pdTRUE) {
            handle_cmd(&c);
            continue;                       /* 有命令就连续处理，别漏 */
        }
        /* 周期活儿都放这里：命令队列空的时候每 200ms 走一遍。
         * 它们各自判断"到点没" —— 队列忙的时候不做也安全（去抖只是延后）。 */
        settings_tick();                    /* 把所有去抖中的设置项落盘（音量/来源…） */
        recent_save_tick();                 /* M6：最近播放落盘（变了 10 秒后写一次） */
        starred_save_tick();                /* M6：收藏镜像落盘 */
        pend_retry_tick();                  /* M6：离线攒下的收藏操作，每分钟补发一次 */
        resume_tick();                      /* M6：每 20 秒记一次续播位置 */
        if (!s_active) continue;

        /* 无命令时：看引擎状态决定要不要自动接下一首 */
        ae_state_t st = audio_engine_state();
        if (st == AE_STATE_FINISHED) {
            ESP_LOGI(TAG, "本首播完 → 下一首");
            next_or_wrap(+1);
        } else if (st == AE_STATE_ERROR) {
            if (++s_err_streak <= 2) {
                ESP_LOGW(TAG, "播放出错（第 %d 次）→ 重试当前这首", s_err_streak);
                start_index(s_index);
            } else if (s_err_streak <= 6) {
                ESP_LOGW(TAG, "连续出错 → 跳过这一首");
                next_or_wrap(+1);
            } else {
                ESP_LOGE(TAG, "连续 %d 次出错，停止自动连播（检查网络/Navidrome）", s_err_streak);
                s_active = false;
                audio_engine_stop();
            }
        }
    }
}

/* ============================================================ 公开接口 */

esp_err_t player_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_FAIL;
    s_q = xQueueCreate(8, sizeof(player_cmd_t));
    if (s_q == NULL) return ESP_FAIL;

    /* 大队列全进 PSRAM（内部 SRAM 只留给协议栈/任务栈/DMA） */
    s_queue = heap_caps_calloc(PLAYER_QUEUE_MAX, sizeof(sub_song_t), MALLOC_CAP_SPIRAM);
    s_recent = heap_caps_calloc(PLAYER_RECENT_MAX, sizeof(sub_song_t), MALLOC_CAP_SPIRAM);
    if (s_queue == NULL || s_recent == NULL) {
        ESP_LOGE(TAG, "队列分配失败（队列 %u KB + 最近 %u KB）",
                 (unsigned)(PLAYER_QUEUE_MAX * sizeof(sub_song_t) / 1024),
                 (unsigned)(PLAYER_RECENT_MAX * sizeof(sub_song_t) / 1024));
        return ESP_ERR_NO_MEM;
    }

    /* M6 的落盘用缓冲。分配失败不致命（缓存功能降级，播放照常），所以只告警。 */
    s_starred = heap_caps_calloc(PLAYER_STAR_MAX, sizeof(sub_song_t), MALLOC_CAP_SPIRAM);
    s_pend    = heap_caps_calloc(PLAYER_STAR_PEND_MAX, sizeof(star_op_t), MALLOC_CAP_SPIRAM);
    s_snap    = heap_caps_malloc(PLAYER_STAR_MAX * sizeof(sub_song_t), MALLOC_CAP_SPIRAM);
    s_json    = heap_caps_malloc(JSON_BUF, MALLOC_CAP_SPIRAM);
    /* M8 搜索：两块结果缓冲（一块给网络直接写、一块给 UI 读，避免持锁做网络）*/
    s_found     = heap_caps_calloc(SUB_SONGS_MAX, sizeof(sub_song_t), MALLOC_CAP_SPIRAM);
    s_found_tmp = heap_caps_calloc(SUB_SONGS_MAX, sizeof(sub_song_t), MALLOC_CAP_SPIRAM);
    if (s_found == NULL || s_found_tmp == NULL) {
        ESP_LOGE(TAG, "搜索结果缓冲分配失败（搜不了，其它功能正常）");
    }
    if (s_starred == NULL || s_pend == NULL || s_snap == NULL || s_json == NULL) {
        ESP_LOGE(TAG, "落盘缓冲分配失败 —— 最近播放/收藏不会持久化（PSRAM 不够？）");
        /* 半套一起清掉，后面所有落盘入口只判断 s_persist_ok 一个标志 */
        heap_caps_free(s_starred); s_starred = NULL;
        heap_caps_free(s_pend);    s_pend = NULL;
        heap_caps_free(s_snap);    s_snap = NULL;
        heap_caps_free(s_json);    s_json = NULL;
    } else {
        s_persist_ok = true;
    }

    /* 音量与来源：NVS 里存的上次设置优先，没有才用默认值 */
    s_volume = s_vol_target = settings_get_int("vol", board_vol_get());
    if (s_volume < 0 || s_volume > 100) s_volume = s_vol_target = 40;
    board_vol_set(s_volume);

    /* 亮度也恢复（背光要等 ui_start 里 board_backlight_init 之后才能真正设，
     * 这里只把值读出来，ui_pages_apply_brightness() 负责施加） */
    s_bright_target = settings_get_int("bright", 100);
    if (s_bright_target < 5 || s_bright_target > 100) s_bright_target = 100;

    int src = settings_get_int("src", (int)PLAYER_SRC_RANDOM);
    if (src >= 0 && src < PLAYER_SRC_COUNT) s_src = (player_src_t)src;

    /* ---- M6：从 cache 分区恢复。store 没挂上就跳过，一切照旧（只是重启后为空）---- */
    if (store_ready() && s_persist_ok) {
        int n = list_load(F_RECENT, s_recent, PLAYER_RECENT_MAX);
        if (n > 0) { s_recent_n = n; s_recent_loaded = true; }
        n = list_load(F_STAR, s_starred, PLAYER_STAR_MAX);
        if (n > 0) { s_starred_n = n; s_starred_gen++; }
        pend_load();
        ESP_LOGW(TAG, "cache 恢复到内存：最近播放 %d 条%s / 收藏 %d 首 / 待重试收藏 %d 条",
                 s_recent_n, s_recent_loaded ? "" : "（全新）", s_starred_n, s_pend_n);
    } else {
        ESP_LOGW(TAG, "cache 分区没挂上：最近播放/收藏只存在内存里（重启会丢）");
    }

    player_status_set_volume(s_volume);
    player_status_set_queue(-1, 0, player_src_name(s_src), true);

    xTaskCreatePinnedToCore(player_task, "player", PLAYER_TASK_STACK, NULL,
                            PLAYER_TASK_PRIO, NULL, PLAYER_TASK_CORE);
    ESP_LOGI(TAG, "就绪：队列 %d 首 / 最近 %d 条（都在 PSRAM），音量 %d%%，上次来源「%s」",
             PLAYER_QUEUE_MAX, PLAYER_RECENT_MAX, s_volume, player_src_name(s_src));
    return ESP_OK;
}

static void post(cmd_kind_t kind, int iarg, bool barg)
{
    if (s_q == NULL) return;
    player_cmd_t c = { .kind = kind, .iarg = iarg, .barg = barg, .iarg2 = 0 };
    /* 满了就丢最旧的命令：宁可少一次按键，也不能阻塞按键任务 */
    if (xQueueSend(s_q, &c, 0) != pdTRUE) ESP_LOGW(TAG, "命令队列满，丢弃 %d", (int)kind);
}

void player_set_source(player_src_t src, bool autoplay) { post(CMD_SET_SOURCE, (int)src, autoplay); }
void player_set_source_next(int dir, bool autoplay)
{
    int n = (int)player_source() + (dir >= 0 ? 1 : PLAYER_SRC_COUNT - 1);
    post(CMD_SET_SOURCE, n % PLAYER_SRC_COUNT, autoplay);
}
void player_play_index(int idx) { post(CMD_PLAY_INDEX, idx, false); }
void player_next(void)          { post(CMD_NEXT, 0, false); }
void player_prev(void)          { post(CMD_PREV, 0, false); }
void player_toggle(void)        { post(CMD_TOGGLE, 0, false); }
void player_restart(void)       { post(CMD_RESTART, 0, false); }
void player_star(bool on)       { post(CMD_STAR, 0, on); }

void player_star_song(const char *song_id, bool on)
{
    if (song_id == NULL || song_id[0] == 0) return;
    player_cmd_t c = { .kind = CMD_STAR_ID, .barg = on };
    strlcpy(c.sid, song_id, sizeof(c.sid));
    if (s_q == NULL || xQueueSend(s_q, &c, 0) != pdTRUE) ESP_LOGW(TAG, "命令队列满，丢弃收藏");
}

void player_play_song_id(const char *song_id)
{
    if (song_id == NULL || song_id[0] == 0) return;
    player_cmd_t c = { .kind = CMD_PLAY_ID };
    strlcpy(c.sid, song_id, sizeof(c.sid));
    if (s_q == NULL || xQueueSend(s_q, &c, 0) != pdTRUE) ESP_LOGW(TAG, "命令队列满，丢弃播放");
}

void player_play_song(const sub_song_t *song)
{
    if (song == NULL || song->id[0] == 0) return;
    /* ⚠️ player_cmd_t 现在有 ~380 字节（含 sub_song_t），队列深度 8 → 3KB 命令队列。
     * 按值传进队列没有额外的拷贝成本（xQueueSend 本来就要拷结构体），
     * 但**不能用 xQueueSendFromISR 之外的方式发大结构**——这里都是任务上下文，没问题。 */
    player_cmd_t c = { .kind = CMD_PLAY_ID };
    c.meta = *song;
    strlcpy(c.sid, song->id, sizeof(c.sid));
    if (s_q == NULL || xQueueSend(s_q, &c, 0) != pdTRUE) ESP_LOGW(TAG, "命令队列满，丢弃播放");
}

void player_seek(uint32_t pos_ms) { post(CMD_SEEK, (int)pos_ms, false); }

void player_search(const char *query)
{
    if (query == NULL || query[0] == 0) return;
    /* 和 player_star_song 一样手动组包：post() 只带 iarg/barg，装不下字符串 */
    player_cmd_t c = { .kind = CMD_SEARCH };
    strlcpy(c.query, query, sizeof(c.query));
    if (s_q == NULL || xQueueSend(s_q, &c, 0) != pdTRUE) ESP_LOGW(TAG, "命令队列满，丢弃搜索");
}

int player_search_count(void)
{
    if (s_lock == NULL) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_found_n;
    xSemaphoreGive(s_lock);
    return n;
}

bool player_search_get(int idx, sub_song_t *out)
{
    if (s_lock == NULL || out == NULL) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = (idx >= 0 && idx < s_found_n);
    if (ok) *out = s_found[idx];
    xSemaphoreGive(s_lock);
    return ok;
}

uint32_t player_search_gen(void)   { return s_found_gen; }
bool     player_search_busy(void)  { return s_found_busy; }
esp_err_t player_search_err(void)  { return s_found_err; }
const char *player_search_query(void) { return s_found_q; }

void player_volume(int vol)
{
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    s_vol_target = vol;                /* 先更新请求值：界面读的是它，不会慢一拍 */
    post(CMD_VOLUME, vol, false);
}

void player_set_mute(bool on) { s_mute_target = on; post(CMD_MUTE, 0, on); }

void player_set_brightness(int pct)
{
    if (pct < 5) pct = 5;
    if (pct > 100) pct = 100;
    s_bright_target = pct;
    post(CMD_BRIGHT, pct, false);
}

int player_queue_count(void)
{
    if (s_lock == NULL) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_count;
    xSemaphoreGive(s_lock);
    return n;
}

int player_queue_index(void)
{
    if (s_lock == NULL) return -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int i = s_index;
    xSemaphoreGive(s_lock);
    return i;
}

int player_recent_count(void)
{
    if (s_lock == NULL) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_recent_n;
    xSemaphoreGive(s_lock);
    return n;
}

bool player_recent_get(int idx, sub_song_t *out)
{
    if (s_lock == NULL) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = (idx >= 0 && idx < s_recent_n);
    if (ok) *out = s_recent[idx];
    xSemaphoreGive(s_lock);
    return ok;
}

bool player_queue_get(int idx, sub_song_t *out)
{
    if (s_lock == NULL) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = (idx >= 0 && idx < s_count);
    if (ok) *out = s_queue[idx];
    xSemaphoreGive(s_lock);
    return ok;
}

uint32_t player_queue_gen(void)
{
    if (s_lock == NULL) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t g = s_queue_gen;
    xSemaphoreGive(s_lock);
    return g;
}

player_src_t player_source(void) { return s_src; }
/* 返回"请求值"而不是"已写入 ES8388 的值" —— 界面要立刻反映按键，不能慢一拍 */
int  player_volume_get(void) { return s_vol_target; }
int  player_brightness(void) { return s_bright_target; }
bool player_is_muted(void)   { return s_mute_target; }

/* ============================================================ M6：续播 / 收藏镜像 */

void player_resume(void) { post(CMD_RESUME, 0, false); }

void player_starred_refresh(bool force) { post(CMD_STAR_REFRESH, 0, force); }

int player_starred_list(sub_song_t *out, int max, bool *synced)
{
    return starred_copy(out, max, synced);
}

uint32_t player_starred_gen(void) { return s_starred_gen; }

int player_starred_pending(void)
{
    if (s_lock == NULL) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_pend_n;
    xSemaphoreGive(s_lock);
    return n;
}

int64_t player_starred_age_ms(void)
{
    if (s_starred_ok_ms < 0) return -1;      /* 本次开机还没成功对上过 */
    return esp_timer_get_time() / 1000 - s_starred_ok_ms;
}
