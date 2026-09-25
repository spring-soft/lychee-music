/*
 * ui_pinyin —— 屏上拼音输入法的查表部分（表由 tools/gen_pinyin.py 生成）
 *
 * 为什么设备上要做这个：屏幕只有一个 160×128 的字母格，**打不出汉字**；
 * 而 Navidrome 服务端不支持拼音搜索（实测过）。所以把字母在本地变成真汉字，
 * 再把汉字交给服务器搜 —— 搜索始终由服务器做，**新加的歌立刻能搜到**，
 * 不存在"本地索引过时"的问题。
 *
 * 表长什么样（见 tools/gen_pinyin.py 的头注释）：
 *   [0]  u32 magic "EPY1"
 *   [4]  u16 音节数 N   [6] u16 保留
 *   [8]  u32 候选总数 M [12] u32 保留
 *   [16] N × { char name[8]; u16 off; u16 cnt; }   ← name 升序，二分查找
 *   [...]  M × u16 Unicode 码点（按音节分组，组内已按常用度降序）
 *
 * ⚠️ 一律用**逐字节读**（下面的 rd16）而不是把缓冲区强转成 u16 指针或结构体指针：
 *    mmap 出来的地址虽然实测是对齐的，但靠"我算过它对齐"来保证 Xtensa 上的
 *    对齐访问太脆（表长度一变就可能错位），而这里一次查表也就几次加法，
 *    性能完全无所谓。
 */
#include <string.h>
#include "ui_internal.h"
#include "assets.h"
#include "esp_log.h"

static const char *TAG = "ui.py";

#define PY_MAGIC     0x31595045u   /* "EPY1" 小端 */
#define PY_NAME_LEN  8

static const uint8_t *s_tab;
static size_t         s_len;
static int            s_n_syl;
static const uint8_t *s_syl;       /* 音节表起始 */
static const uint8_t *s_cand;      /* 汉字段起始（u16 数组）*/
static int            s_n_cand;

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* 第 i 个音节表项 */
static const uint8_t *syl_at(int i) { return s_syl + (size_t)i * 12; }

/* 找到音节（返回表项下标，找不到 -1）。表按 name 升序 → 二分 */
static int syl_find(const char *s)
{
    if (s_tab == NULL || s == NULL || s[0] == 0) return -1;
    size_t n = strlen(s);
    if (n > PY_NAME_LEN) return -1;

    int lo = 0, hi = s_n_syl - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const uint8_t *e = syl_at(mid);
        /* 表项里的名字是 NUL 填充的，直接 strncmp 到 NUL 即可 */
        int c = strncmp(s, (const char *)e, PY_NAME_LEN);
        if (c == 0) return mid;
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return -1;
}

esp_err_t pinyin_init(void)
{
    asset_t a;
    if (!assets_get(ASSET_PINYIN, &a) || a.size < 16) {
        ESP_LOGE(TAG, "拼音表没进 assets 分区（跑 tools/gen_pinyin.py 再 gen_assets.py）");
        return ESP_ERR_NOT_FOUND;
    }
    if (rd32(a.data) != PY_MAGIC) {
        ESP_LOGE(TAG, "拼音表 magic 不对（生成脚本和固件版本不匹配？）");
        return ESP_ERR_INVALID_STATE;
    }
    s_n_syl  = rd16(a.data + 4);
    s_n_cand = (int)rd32(a.data + 8);
    size_t need = 16 + (size_t)s_n_syl * 12 + (size_t)s_n_cand * 2;
    if (a.size < need) {
        ESP_LOGE(TAG, "拼音表被截断：需要 %u 字节，只有 %u", (unsigned)need, (unsigned)a.size);
        return ESP_ERR_INVALID_SIZE;
    }
    s_tab = a.data;
    s_len = a.size;
    s_syl = a.data + 16;
    s_cand = s_syl + (size_t)s_n_syl * 12;
    ESP_LOGW(TAG, "拼音表已加载：%d 个音节 / %d 条候选（%u KB，mmap 零拷贝）",
             s_n_syl, s_n_cand, (unsigned)(a.size / 1024));
    return ESP_OK;
}

bool pinyin_ready(void) { return s_tab != NULL; }

int pinyin_candidates(const char *syllable, uint16_t *out, int max)
{
    int i = syl_find(syllable);
    if (i < 0) return 0;
    const uint8_t *e = syl_at(i);
    int off = rd16(e + 8);
    int cnt = rd16(e + 10);
    if (out == NULL || max <= 0) return cnt;      /* 只问数量 */
    if (cnt > max) cnt = max;
    for (int k = 0; k < cnt; k++) out[k] = rd16(s_cand + (size_t)(off + k) * 2);
    return cnt;
}

bool pinyin_is_syllable(const char *s) { return syl_find(s) >= 0; }

/* 有没有音节以 s 开头？"yu" 是（yuan/yue/yun），"yue" 不是任何更长音节的前缀 */
static bool has_prefix(const char *s, bool require_longer)
{
    if (s_tab == NULL || s == NULL || s[0] == 0) return false;
    size_t n = strlen(s);
    for (int i = 0; i < s_n_syl; i++) {
        const char *name = (const char *)syl_at(i);
        if (strncmp(name, s, n) != 0) continue;
        if (!require_longer || name[n] != 0) return true;
    }
    return false;
}

bool pinyin_is_prefix(const char *s)  { return has_prefix(s, false); }
bool pinyin_can_extend(const char *s) { return has_prefix(s, true); }
