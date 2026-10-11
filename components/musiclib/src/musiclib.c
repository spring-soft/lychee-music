/*
 * musiclib 实现 —— 设计取舍见 musiclib.h 顶部（那里写了 5 条"改之前先读"）。
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <inttypes.h>

#include "musiclib.h"
#include "sd_card.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "musiclib";

/* 扫描时目录栈的深度上限（每个元素约 200B，全在 PSRAM） */
#define MUSICLIB_SCAN_STACK  24

/* ============================================================ 数据结构 */

/* 落盘与内存同一份记录。刻意做得紧凑：2000 首常驻 PSRAM，
 * 每条省 200 字节就是 400KB 的差别。 */
typedef struct {
    char     path[SUB_SONG_PATH_MAX];   /* 完整 VFS 路径（/sdcard/…），与 sub_song_t.path 同格式 */
    char     title[96];
    char     artist[64];
    char     suffix[8];                 /* 小写、不带点 */
    uint32_t size;
    uint32_t duration_s;                /* 0 = 未知（不解析标签；首次播放后可回填） */
    uint8_t  flags;                     /* 预留：bit0 = 已收藏 */
} musiclib_rec_t;

typedef struct {
    char     magic[4];                  /* "EMLI" */
    uint32_t version;
    uint32_t count;
    uint32_t rec_size;
    uint32_t crc;                       /* 仅记录区的 CRC32 */
} musiclib_hdr_t;

typedef struct { char path[SUB_SONG_PATH_MAX]; uint8_t depth; } dir_item_t;

static musiclib_rec_t  *s_rec;          /* PSRAM 常驻索引（NULL = 还没装载） */
static int              s_n;
static bool             s_scanning;
static volatile int     s_scan_scanned;
static volatile int     s_scan_found;
static volatile int     s_scan_result;  /* esp_err_t */
static int              s_last_err;     /* esp_err_t */

/* ============================================================ 纯函数 */

/* FNV-1a 32 位。用来把路径变成稳定的曲目 id ——
 * 稳定很重要：离线收藏存的是 id，重扫后路径没变就必须还是同一个 id。 */
static uint32_t fnv1a(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) { h ^= (uint8_t)*s; h *= 16777619u; }
    return h;
}

static void trim_both(char *s)
{
    if (s == NULL) return;
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t')) s[--n] = 0;
}

/* 取小写扩展名（不带点）。没有扩展名就空串。 */
static void ext_lower(const char *name, char *out, size_t outlen)
{
    out[0] = 0;
    const char *dot = strrchr(name, '.');
    if (dot == NULL || dot == name || dot[1] == 0) return;
    size_t n = strlen(dot + 1);
    if (n >= outlen) n = outlen - 1;
    for (size_t i = 0; i < n; i++) out[i] = (char)tolower((unsigned char)dot[1 + i]);
    out[n] = 0;
}

bool musiclib_is_music_file(const char *name)
{
    char e[8];
    ext_lower(name, e, sizeof(e));
    return (strcmp(e, "flac") == 0 || strcmp(e, "mp3")  == 0 ||
            strcmp(e, "aac")  == 0 || strcmp(e, "m4a")  == 0 ||
            strcmp(e, "ogg")  == 0 || strcmp(e, "wav")  == 0);
    /* ⚠️ 能不能真的解码是另一回事（见 sdkconfig.forced 里"能力边界"那段：
     *    WMA/APE/DSD 这些解码器组件里根本没有）。放进曲库但放不出声，
     *    用户会以为是坏了 —— 所以这里只列解码器真的支持的格式。 */
}

void musiclib_parse_name(const char *name, char *title, size_t tlen,
                         char *artist, size_t alen)
{
    if (tlen) title[0] = 0;
    if (alen)  artist[0] = 0;
    if (name == NULL || name[0] == 0) return;

    /* 只取 basename（调用方一般给的就是 basename，这里兜一层） */
    const char *base = name;
    for (const char *p = name; *p; p++) if (*p == '/') base = p + 1;

    char work[176];
    strlcpy(work, base, sizeof(work));

    /* 去扩展名。只在"点号不算太靠前、且后面不超过 5 个字符"时才当扩展名，
     * 免得把 "1989.演唱会" 这种标题从中间切掉。 */
    char *dot = strrchr(work, '.');
    if (dot && dot != work && strlen(dot) <= 5) *dot = 0;

    const char *s = work;

    /* 去前导音轨号：数字（≤3 位）后面必须跟一个明确的分隔符才认，
     * 否则 "2002年的第一场雪" 会被切成 "年的第一场雪"。 */
    {
        const char *p = s;
        int d = 0;
        while (d < 3 && p[0] >= '0' && p[0] <= '9') { p++; d++; }
        if (d > 0) {
            const char *q = p;
            while (*q == ' ') q++;
            if (*q == '-' || *q == '.' || *q == '_' || *q == ')') {
                q++;
                while (*q == ' ' || *q == '-' || *q == '.') q++;
                s = q;
            }
        }
    }

    /* 以【第一个】" - " 切分：左 = 艺术家，右 = 标题。
     * 用第一个而不是最后一个，"Artist - Title - Remix" 才能把 Remix 留在标题里。 */
    const char *sep = strstr(s, " - ");
    if (sep != NULL && sep != s) {
        size_t an = (size_t)(sep - s);
        if (an >= alen) an = (alen > 0) ? alen - 1 : 0;
        memcpy(artist, s, an);
        artist[an] = 0;
        strlcpy(title, sep + 3, tlen);
    } else {
        strlcpy(title, s, tlen);
    }
    trim_both(title);
    trim_both(artist);
}

/* ============================================================ 记录 → sub_song_t */

static void rec_to_song(const musiclib_rec_t *r, sub_song_t *out)
{
    memset(out, 0, sizeof(*out));
    /* id 由【路径】哈希得到：重扫后路径没变 → id 不变 → 离线收藏不会错位。 */
    snprintf(out->id, sizeof(out->id), "L%08" PRIX32, fnv1a(r->path));
    strlcpy(out->title,  r->title,  sizeof(out->title));
    strlcpy(out->artist, r->artist, sizeof(out->artist));
    strlcpy(out->suffix, r->suffix, sizeof(out->suffix));
    strlcpy(out->path,   r->path,   sizeof(out->path));
    out->size       = r->size;
    out->duration_s = r->duration_s;
    out->starred    = (r->flags & 1) != 0;
    /* album / cover 留空：不解析标签，离线模式用占位封面。 */
}

/* ============================================================ 自检（纯函数） */

void musiclib_selftest(void)
{
    /* 每一例都是"以前可能出错、或者以后容易被改坏"的形状 */
    static const struct { const char *name, *title, *artist; } cases[] = {
        { "01 - 周杰伦 - 稻香.flac",      "稻香",              "周杰伦" },
        { "Artist - Title.mp3",           "Title",             "Artist" },
        { "Artist - Title - Remix.flac",  "Title - Remix",     "Artist" },   /* 第一个 " - " 切分 */
        { "纯中文标题.flac",               "纯中文标题",          "" },        /* 没有分隔符 */
        { "01.标题.mp3",                   "标题",              "" },        /* 点号分隔的音轨号 */
        { "2002年的第一场雪.flac",         "2002年的第一场雪",    "" },        /* 前导数字【不】能当音轨号切掉 */
        { "07 - a - b.ogg",               "b",                 "a" },
        { "no_ext",                       "no_ext",            "" },        /* 无扩展名 */
        { "with.dots.in.name.flac",       "with.dots.in.name", "" },        /* 只去最后一个扩展名 */
        { "  spaced - name  .mp3",        "name",              "spaced" },  /* 两端空白要去掉 */
    };
    static const struct { const char *n; bool want; } fext[] = {
        { "a.flac", true }, { "a.MP3", true }, { "a.m4a", true }, { "a.ogg", true },
        { "a.wav",  true }, { "a.aac", true },
        { "a.txt", false }, { "a.flac.bak", false }, { "noext", false },
    };

    const int N = (int)(sizeof(cases) / sizeof(cases[0]));
    int pass = 0, fail = 0;

    ESP_LOGW(TAG, "===== musiclib 自检：文件名解析（%d 例）=====", N);
    for (int i = 0; i < N; i++) {
        char t[96], a[64];
        musiclib_parse_name(cases[i].name, t, sizeof(t), a, sizeof(a));
        if (strcmp(t, cases[i].title) == 0 && strcmp(a, cases[i].artist) == 0) {
            pass++;
            ESP_LOGW(TAG, "  [OK  ] \"%s\" -> 标题「%s」艺术家「%s」", cases[i].name, t, a);
        } else {
            fail++;
            ESP_LOGE(TAG, "  [FAIL] \"%s\" -> 得到 标题「%s」艺术家「%s」，"
                          "期望 标题「%s」艺术家「%s」",
                     cases[i].name, t, a, cases[i].title, cases[i].artist);
        }
    }

    const int M = (int)(sizeof(fext) / sizeof(fext[0]));
    ESP_LOGW(TAG, "===== musiclib 自检：扩展名过滤（%d 例）=====", M);
    for (int i = 0; i < M; i++) {
        bool got = musiclib_is_music_file(fext[i].n);
        if (got == fext[i].want) {
            pass++;
            ESP_LOGW(TAG, "  [OK  ] \"%s\" -> %s", fext[i].n, got ? "收进曲库" : "忽略");
        } else {
            fail++;
            ESP_LOGE(TAG, "  [FAIL] \"%s\" -> 得到 %s，期望 %s",
                     fext[i].n, got ? "收进曲库" : "忽略", fext[i].want ? "收进曲库" : "忽略");
        }
    }

    ESP_LOGW(TAG, "===== musiclib 自检结束：通过 %d，失败 %d =====", pass, fail);
}

/* ============================================================ 索引读写 */

static void index_path(char *out, size_t len)
{
    snprintf(out, len, SD_ROOT "/" MUSICLIB_INDEX_REL);
}

static esp_err_t index_save(void)
{
    char full[SUB_SONG_PATH_MAX];
    index_path(full, sizeof(full));

    FILE *f = fopen(full, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "索引写不了（卡被写保护？）: %s", full);
        return ESP_FAIL;
    }

    musiclib_hdr_t h;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, "EMLI", 4);
    h.version  = MUSICLIB_INDEX_VER;
    h.count    = (uint32_t)s_n;
    h.rec_size = (uint32_t)sizeof(musiclib_rec_t);
    h.crc      = (s_n > 0)
                 ? esp_rom_crc32_le(0, (const uint8_t *)s_rec,
                                    (uint32_t)((size_t)s_n * sizeof(musiclib_rec_t)))
                 : 0;

    bool ok = (fwrite(&h, sizeof(h), 1, f) == 1);
    if (ok && s_n > 0) {
        ok = (fwrite(s_rec, sizeof(musiclib_rec_t), (size_t)s_n, f) == (size_t)s_n);
    }
    fclose(f);
    return ok ? ESP_OK : ESP_FAIL;
}

void musiclib_clear(void)
{
    if (s_rec) { heap_caps_free(s_rec); s_rec = NULL; }
    s_n = 0;
}

esp_err_t musiclib_load(void)
{
    if (!sd_card_mounted()) {
        s_last_err = ESP_ERR_INVALID_STATE;
        return s_last_err;
    }
    musiclib_clear();

    char full[SUB_SONG_PATH_MAX];
    index_path(full, sizeof(full));

    FILE *f = fopen(full, "rb");
    if (f == NULL) {
        /* 第一次用、或换了一张卡 —— 不是错误，只是"还没扫过"。 */
        ESP_LOGI(TAG, "卡上没有索引（%s），需要扫描一次", full);
        musiclib_clear();
        s_last_err = ESP_ERR_NOT_FOUND;
        return s_last_err;
    }

    musiclib_hdr_t h;
    if (fread(&h, sizeof(h), 1, f) != 1) {
        fclose(f);
        ESP_LOGW(TAG, "索引头读不全（掉电写坏？），当作没有索引");
        s_last_err = ESP_ERR_INVALID_SIZE;
        return s_last_err;
    }
    if (memcmp(h.magic, "EMLI", 4) != 0 || h.version != MUSICLIB_INDEX_VER ||
        h.rec_size != (uint32_t)sizeof(musiclib_rec_t) || h.count > MUSICLIB_MAX_SONGS) {
        fclose(f);
        ESP_LOGW(TAG, "索引格式不兼容（版本/记录大小变了），需要重扫");
        s_last_err = ESP_ERR_INVALID_VERSION;
        return s_last_err;
    }
    if (h.count == 0) {
        fclose(f);
        ESP_LOGI(TAG, "索引是空的（扫描时一首都没识别出来）");
        s_last_err = ESP_OK;
        return ESP_OK;
    }

    size_t bytes = (size_t)h.count * sizeof(musiclib_rec_t);
    musiclib_rec_t *buf = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        fclose(f);
        ESP_LOGE(TAG, "索引缓冲分配失败（%u 字节 PSRAM）", (unsigned)bytes);
        s_last_err = ESP_ERR_NO_MEM;
        return s_last_err;
    }
    if (fread(buf, sizeof(musiclib_rec_t), h.count, f) != h.count) {
        fclose(f);
        heap_caps_free(buf);
        ESP_LOGW(TAG, "索引记录读不全，当作没有索引");
        s_last_err = ESP_ERR_INVALID_SIZE;
        return s_last_err;
    }
    fclose(f);

    uint32_t crc = esp_rom_crc32_le(0, (const uint8_t *)buf, (uint32_t)bytes);
    if (crc != h.crc) {
        heap_caps_free(buf);
        ESP_LOGW(TAG, "索引 CRC 校验失败（掉电写坏？），需要重扫");
        s_last_err = ESP_ERR_INVALID_CRC;
        return s_last_err;
    }

    s_rec = buf;
    s_n   = (int)h.count;
    s_last_err = ESP_OK;
    ESP_LOGW(TAG, "离线曲库已装载：%d 首（%u KB PSRAM）", s_n, (unsigned)(bytes / 1024));
    return ESP_OK;
}

/* ============================================================ 扫描 */

static void rec_add(const char *full_path, const char *name, uint32_t size)
{
    musiclib_rec_t *r = &s_rec[s_n];
    memset(r, 0, sizeof(*r));
    strlcpy(r->path, full_path, sizeof(r->path));
    musiclib_parse_name(name, r->title, sizeof(r->title), r->artist, sizeof(r->artist));
    ext_lower(name, r->suffix, sizeof(r->suffix));
    r->size = size;
    s_n++;
}

static void scan_task(void *arg)
{
    dir_item_t *stack = heap_caps_malloc((size_t)MUSICLIB_SCAN_STACK * sizeof(dir_item_t),
                                         MALLOC_CAP_SPIRAM);
    if (stack == NULL) {
        ESP_LOGE(TAG, "扫描栈分配失败");
        s_scan_result = ESP_ERR_NO_MEM;
        s_scanning = false;
        vTaskDelete(NULL);
        return;
    }

    /* 每一步都重建索引：扫描的语义就是"以卡上的实际情况为准" */
    s_n = 0;
    s_scan_scanned = 0;
    s_scan_found   = 0;

    int top = 0;
    strlcpy(stack[0].path, SD_ROOT, sizeof(stack[0].path));
    stack[0].depth = 0;
    top = 1;

    bool limit_warned = false;

    while (top > 0) {
        dir_item_t cur;
        memcpy(&cur, &stack[--top], sizeof(cur));

        DIR *d = opendir(cur.path);
        if (d == NULL) {
            ESP_LOGW(TAG, "打不开目录（跳过）: %s", cur.path);
            continue;
        }

        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            /* 跳过 "." ".." 以及 macOS 的 "._xxx" 这类垃圾项 */
            if (e->d_name[0] == '.') continue;

            char full[SUB_SONG_PATH_MAX];
            int k = snprintf(full, sizeof(full), "%s/%s", cur.path, e->d_name);
            if (k <= 0 || k >= (int)sizeof(full)) {
                ESP_LOGW(TAG, "路径太长，跳过: %s/%s", cur.path, e->d_name);
                continue;
            }

            struct stat st;
            if (stat(full, &st) != 0) continue;

            if (S_ISDIR(st.st_mode)) {
                if (cur.depth + 1 > MUSICLIB_MAX_DEPTH) {
                    ESP_LOGW(TAG, "目录太深（>%d），跳过: %s", MUSICLIB_MAX_DEPTH, full);
                } else if (top >= MUSICLIB_SCAN_STACK) {
                    ESP_LOGW(TAG, "扫描栈满（目录太宽），跳过: %s", full);
                } else {
                    strlcpy(stack[top].path, full, sizeof(stack[top].path));
                    stack[top].depth = (uint8_t)(cur.depth + 1);
                    top++;
                }
            } else if (S_ISREG(st.st_mode)) {
                s_scan_scanned++;
                if (musiclib_is_music_file(e->d_name)) {
                    if (s_n < MUSICLIB_MAX_SONGS) {
                        rec_add(full, e->d_name, (uint32_t)st.st_size);
                        s_scan_found = s_n;
                    } else if (!limit_warned) {
                        limit_warned = true;
                        ESP_LOGW(TAG, "曲目数已达上限 %d，后面的不再收录", MUSICLIB_MAX_SONGS);
                    }
                }
            }
        }
        closedir(d);
        /* 每处理完一个目录让一次步：扫描任务优先级低，别把 CPU 占死 */
        vTaskDelay(1);
    }

    heap_caps_free(stack);

    ESP_LOGW(TAG, "扫描完成：遍历 %d 个文件，入库 %d 首", s_scan_scanned, s_n);

    esp_err_t err = index_save();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "索引落盘失败: %s（本次扫描结果只在内存里，重启要重扫）",
                 esp_err_to_name(err));
    }
    s_last_err    = err;
    s_scan_result = err;
    s_scanning    = false;
    vTaskDelete(NULL);
}

esp_err_t musiclib_scan_start(void)
{
    if (s_scanning) return ESP_ERR_INVALID_STATE;
    if (!sd_card_mounted()) {
        ESP_LOGW(TAG, "卡没挂上，扫不了");
        s_last_err = ESP_ERR_INVALID_STATE;
        return s_last_err;
    }
    if (s_rec == NULL) {
        /* 索引数组按最大容量一次性分配（PSRAM），扫一半失败也不会留下半个状态 */
        s_rec = heap_caps_malloc((size_t)MUSICLIB_MAX_SONGS * sizeof(musiclib_rec_t),
                                 MALLOC_CAP_SPIRAM);
        if (s_rec == NULL) {
            ESP_LOGE(TAG, "曲库数组分配失败（需 %u KB PSRAM）",
                     (unsigned)((size_t)MUSICLIB_MAX_SONGS * sizeof(musiclib_rec_t) / 1024));
            s_last_err = ESP_ERR_NO_MEM;
            return s_last_err;
        }
    }
    s_n = 0;
    s_scanning    = true;
    s_scan_result = ESP_OK;
    s_scan_scanned = 0;
    s_scan_found   = 0;

    /* 栈放 PSRAM：扫描任务要用几百字节到几 KB 的栈，内部 SRAM 现在只有十几 KB。
     * 但它会读卡（VFS/FATFS），写索引 —— 那些内部缓冲不在这块栈上。 */
    if (xTaskCreatePinnedToCoreWithCaps(scan_task, "ml_scan", 4096, NULL, 3, NULL, 0,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        s_scanning = false;
        s_last_err = ESP_ERR_NO_MEM;
        ESP_LOGE(TAG, "扫描任务创建失败");
        return s_last_err;
    }
    return ESP_OK;
}

bool      musiclib_scan_busy(void)    { return s_scanning; }
int       musiclib_scan_scanned(void) { return s_scan_scanned; }
int       musiclib_scan_found(void)   { return s_scan_found; }
esp_err_t musiclib_scan_result(void)  { return (esp_err_t)s_scan_result; }
esp_err_t musiclib_last_err(void)     { return (esp_err_t)s_last_err; }
int       musiclib_count(void)        { return s_n; }

bool musiclib_get(int idx, sub_song_t *out)
{
    if (out == NULL || idx < 0 || idx >= s_n) return false;
    rec_to_song(&s_rec[idx], out);
    return true;
}

bool musiclib_find_by_id(const char *id, sub_song_t *out)
{
    if (id == NULL || id[0] == 0 || out == NULL) return false;
    for (int i = 0; i < s_n; i++) {
        /* id 是路径哈希，直接比字符串就够了（不用再算一遍哈希再比，省事且更直观） */
        char tmp[48];
        snprintf(tmp, sizeof(tmp), "L%08" PRIX32, fnv1a(s_rec[i].path));
        if (strcmp(tmp, id) == 0) { rec_to_song(&s_rec[i], out); return true; }
    }
    return false;
}

bool musiclib_find_by_path(const char *path, sub_song_t *out)
{
    if (path == NULL || path[0] == 0 || out == NULL) return false;
    for (int i = 0; i < s_n; i++) {
        if (strcmp(s_rec[i].path, path) == 0) { rec_to_song(&s_rec[i], out); return true; }
    }
    return false;
}

int musiclib_pick_random(sub_song_t *out, int max)
{
    if (out == NULL || max <= 0 || s_n <= 0) return 0;

    int got = 0;
    int guard = max * 8 + 16;            /* 防止曲目很少时死循环 */
    while (got < max && got < s_n && guard-- > 0) {
        int idx = (int)(esp_random() % (uint32_t)s_n);
        bool dup = false;
        for (int i = 0; i < got; i++) {
            sub_song_t t;
            /* 已经挑过的就不重复挑（曲库小的时候很想看到不同的歌） */
            if (musiclib_get(idx, &t) && strcmp(t.id, out[i].id) == 0) { dup = true; break; }
        }
        if (dup) continue;
        if (musiclib_get(idx, &out[got])) got++;
    }
    return got;
}

/* ============================================================ 搜索 */

/* 子串包含：ASCII 部分忽略大小写，非 ASCII（UTF-8 多字节）按字节比较。
 * 不做 UTF-8 解码 —— 汉字在 UTF-8 里不会和 ASCII 字节撞，所以按字节比较是安全的。 */
static bool contains_ci(const char *hay, const char *needle)
{
    if (needle[0] == 0) return true;
    size_t hl = strlen(hay), nl = strlen(needle);
    if (nl > hl) return false;
    for (size_t i = 0; i + nl <= hl; i++) {
        size_t j = 0;
        while (j < nl) {
            unsigned char a = (unsigned char)hay[i + j];
            unsigned char b = (unsigned char)needle[j];
            if (a < 0x80 && b < 0x80) { if (tolower(a) != tolower(b)) break; }
            else if (a != b) break;
            j++;
        }
        if (j == nl) return true;
    }
    return false;
}

int musiclib_search(const char *query, sub_song_t *out, int max)
{
    if (query == NULL || query[0] == 0 || out == NULL || max <= 0) return 0;
    int got = 0;
    for (int i = 0; i < s_n && got < max; i++) {
        if (contains_ci(s_rec[i].title, query) || contains_ci(s_rec[i].artist, query)) {
            if (musiclib_get(i, &out[got])) got++;
        }
    }
    return got;
}

/* ============================================================ 收藏同步用的匹配 */

/* 归一化：ASCII 只留字母数字并转小写，其余 ASCII（空格、-、_、(、) 等）丢掉；
 * UTF-8 多字节原样保留（汉字不能丢）。
 * 目的：让"周杰伦 - 稻香"与"周杰伦-稻香"、以及英文大小写差异都能对上。 */
static void normalize_key(const char *in, char *out, size_t outlen)
{
    size_t o = 0;
    if (in == NULL) { out[0] = 0; return; }
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 1 < outlen; p++) {
        unsigned char c = *p;
        if (c < 0x80) {
            if (isalnum(c)) out[o++] = (char)tolower(c);
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = 0;
}

int musiclib_match(const char *title, const char *artist,
                   char paths[][SUB_SONG_PATH_MAX], int max)
{
    if (title == NULL || paths == NULL || max <= 0 || s_n <= 0) return 0;

    char nt[160], na[96];
    normalize_key(title, nt, sizeof(nt));
    normalize_key(artist, na, sizeof(na));
    if (nt[0] == 0) return 0;

    /* 第一遍：标题 + 艺术家都对上（最可信） */
    int got = 0;
    for (int i = 0; i < s_n && got < max; i++) {
        char rt[96], ra[64];
        normalize_key(s_rec[i].title, rt, sizeof(rt));
        if (strcmp(nt, rt) != 0) continue;
        normalize_key(s_rec[i].artist, ra, sizeof(ra));
        /* 两边都有艺术家时要求一致；一边没有就不强求 */
        if (na[0] && ra[0] && strcmp(na, ra) != 0) continue;
        strlcpy(paths[got], s_rec[i].path, SUB_SONG_PATH_MAX);
        got++;
    }
    if (got > 0) return got;

    /* 第二遍：只按标题（在线元数据里的艺术家常带 "feat. X"，卡上却是干净的名字）。
     * 这一遍只在第一遍颗粒无收时才走 —— 宁可少同步，也不要张冠李戴。 */
    for (int i = 0; i < s_n && got < max; i++) {
        char rt[96];
        normalize_key(s_rec[i].title, rt, sizeof(rt));
        if (strcmp(nt, rt) != 0) continue;
        strlcpy(paths[got], s_rec[i].path, SUB_SONG_PATH_MAX);
        got++;
    }
    return got;
}
