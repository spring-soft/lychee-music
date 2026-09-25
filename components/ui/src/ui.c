/*
 * UI 核心：显示初始化 + 页面注册/切换 + 播放页 + 启动页 + 封面异步加载
 *
 * 三条"避免整屏重绘"的铁律（性能全靠它们）：
 *   1) 静态部件全在预渲染底图里（bg_*），运行时只叠加小的动态控件
 *   2) 动态控件的父对象【无填充/无圆角/无阴影】——圆角和阴影会把 LVGL 重绘区
 *      撑到整块，这是嵌入式 LVGL 最常见的性能陷阱
 *   3) 定时器里先比较再 invalidate，禁止无条件整屏重绘
 *
 * 线程模型：只有 ui 任务碰 LVGL 对象。别的任务（player / 按键回调 / 未来的网页）
 * 只能调 ui_splash_set() / ui_toast() 这类"写字符串 + 加锁"的接口，
 * 由 ui 任务在 tick 里落到 LVGL 上。
 */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>
#include "ui.h"
#include "ui_internal.h"
#include "ui_text_chars.h"
#include "assets.h"
#include "app_core.h"
#include "audio_engine.h"
#include "subsonic.h"
#include "board.h"
#include "store.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "lvgl.h"

static const char *TAG = "ui";

#define UI_TASK_STACK   8192
#define UI_TASK_PRIO    5
#define UI_TASK_CORE    1

/* 1 = 换歌时把屏幕像素回读到串口（排障用，平时必须 0，否则日志被字符画淹没） */
#define UI_DUMP_ON_SONG 0

/* 1 = 开机进显示校准模式（轮流试 4 组 invert × RGB/BGR）。
 * 定下来之后改成 0，并把结果写回 ui_display.c 的 s_invert / s_bgr 初值。 */
#define UI_CALIBRATE    0

/* 动画工具（定义在下面"动画"那一节，这里先声明 —— 提示条在它之前就要用） */
static void anim_fade_in(lv_obj_t *o, uint32_t ms);
static void anim_fade_out(lv_obj_t *o, uint32_t ms, lv_anim_ready_cb_t done);
static void anim_slide_y(lv_obj_t *o, int from, int to, uint32_t ms);
static void anim_hop(lv_obj_t *o, int dy, uint32_t ms);

lv_font_t       *ui_font_16  = NULL;   /* 16px 中文（bin 加载，顶栏歌名/列表） */
lv_font_t       *ui_font_12  = NULL;   /* 12px 中文（菜单行/歌手/专辑） */
const lv_font_t *ui_font_lat = &lv_font_montserrat_12;

/* 播放页控件（build_play_screen 里赋值，play_refresh 里更新） */
static lv_obj_t *s_lbl_title, *s_lbl_artist, *s_lbl_time_cur, *s_lbl_time_tot;
static lv_obj_t *s_bar, *s_img_cover, *s_img_state, *s_img_star;

/* 屏幕注册表：4 个页面开机全建好，切换只 lv_screen_load（运行时绝不建/删 widget） */
static lv_obj_t *s_screens[UI_PAGE_COUNT];
static ui_page_t s_page = UI_PAGE_PLAY;
static bool      s_splash_active;
static bool      s_play_refresh_all = true;    /* 切回播放页时整块刷一次 */

/* 封面（PSRAM 缓冲 + 一份 dsc；数据由 cover_task 填，UI 只换 src） */
#define COVER_PX        72                      /* 与底图上的封面框一致 */
#define COVER_BYTES     (COVER_PX * COVER_PX * 2)
static uint8_t        *s_cover_buf;
static lv_image_dsc_t  s_cover_dsc;
static volatile uint32_t s_cover_gen;           /* 已解码好的封面属于哪一代 */
static volatile bool     s_cover_ready;
static uint32_t          s_ui_gen;              /* UI 当前显示的歌曲代际 */

/* ============================================================ 素材 → LVGL 对象 */

lv_image_dsc_t *ui_img_dsc(asset_id_t id)
{
    static struct { asset_id_t id; lv_image_dsc_t dsc; bool ready; } cache[ASSET_COUNT];
    for (int i = 0; i < ASSET_COUNT; i++) {
        if (cache[i].ready && cache[i].id == id) return &cache[i].dsc;
    }
    asset_t a;
    if (!assets_get(id, &a)) {
        ESP_LOGW(TAG, "素材缺失: %s", id < ASSET_COUNT ? asset_names[id] : "?");
        return NULL;
    }
    for (int i = 0; i < ASSET_COUNT; i++) {
        if (!cache[i].ready) {
            memset(&cache[i].dsc, 0, sizeof(cache[i].dsc));
            /* fmt: 0=RGB565（底图）2=RGB565A8（图标，带 alpha 让 LVGL 混合到背景上） */
            cache[i].dsc.header.cf = (a.fmt == 2) ? LV_COLOR_FORMAT_RGB565A8
                                                   : LV_COLOR_FORMAT_RGB565;
            cache[i].dsc.header.w  = a.w;
            cache[i].dsc.header.h  = a.h;
            cache[i].dsc.data_size = a.size;
            cache[i].dsc.data      = a.data;
            cache[i].id = id;
            cache[i].ready = true;
            return &cache[i].dsc;
        }
    }
    return NULL;
}

lv_obj_t *ui_put_img(lv_obj_t *parent, asset_id_t id, int x, int y)
{
    lv_image_dsc_t *d = ui_img_dsc(id);
    if (!d) return NULL;
    lv_obj_t *im = lv_image_create(parent);
    lv_image_set_src(im, d);
    lv_obj_set_pos(im, x, y);
    lv_obj_remove_flag(im, LV_OBJ_FLAG_SCROLLABLE);
    return im;
}

/* 透明容器：不参与绘制，只是给动态控件一个"无填充无圆角"的父对象 */
lv_obj_t *ui_plain_cont(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

/* ⚠️⚠️ LV_LABEL_LONG_MODE_DOTS 是"【折行】+ 在最后一行末尾加省略号"，
 * 不是"单行省略"！只限宽度不限高度的话，字多了会折成两三行、把下面的行盖住
 * （用户实测报的"歌名过长就会换行挡住下面"）。
 * 要做真正的单行省略，必须【同时】把高度限成一行。
 *
 * 列表行 / 菜单行 / 提示条 / 信息页这些"一行一格"的地方都用这个包装函数。 */
lv_obj_t *ui_put_label_1line(lv_obj_t *parent, const char *txt, int x, int y, int w,
                             const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = ui_put_label(parent, txt, x, y, w, font, color, LV_LABEL_LONG_MODE_DOTS);
    /* 限成【一行多一点】的高度：DOTS 就不会折行，同时给字形上下边缘留余量。
     * ⚠️ 不留余量会裁字：汉字的实际包围盒可能比 line_height 高一点
     *    （16px 字库行高 18，但字形要占 ~19px），写死 18 会把上下边缘切掉。
     * ⚠️ 余量不能大到能放下第二行（2×18=36），这里 +4 是安全的。 */
    lv_obj_set_height(l, lv_font_get_line_height(font) + 4);
    return l;
}

lv_obj_t *ui_put_label(lv_obj_t *parent, const char *txt, int x, int y, int w,
                       const lv_font_t *font, uint32_t color,
                       lv_label_long_mode_t mode)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_pos(l, x, y);
    if (w > 0) lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, mode);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

const lv_font_t *ui_font_cjk(void) { return ui_font_16; }

/* 只在文本真的变了时才 set_text —— LVGL 内部有缓存，但省掉比较逻辑外的重绘更稳 */
void ui_set_text(lv_obj_t *lbl, const char *txt)
{
    const char *cur = lv_label_get_text(lbl);
    if (cur == NULL || strcmp(cur, txt) != 0) {
        lv_label_set_text(lbl, txt);
    }
}

void ui_fmt_mmss(uint32_t ms, char *buf, size_t len)
{
    snprintf(buf, len, "%" PRIu32 ":%02" PRIu32, ms / 60000, (ms / 1000) % 60);
}

/* 屏幕根对象：⚠️ 必须关掉滚动+滚动条。
 * LVGL 默认给 screen 开了滚动，只要有子对象超出可视范围，右边/底部就会冒出
 * 滚动条（第一次上屏就出现了）。 */
lv_obj_t *ui_new_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    /* 纯白底：之前用冷灰 #F2F4F7，在 TN 屏上整体泛紫，用户实测说"太丑" */
    lv_obj_set_style_bg_color(scr, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    return scr;
}

/* ============================================================ 页面注册 / 切换 */

void ui_page_register(ui_page_t p, lv_obj_t *scr)
{
    if (p >= 0 && p < UI_PAGE_COUNT) s_screens[p] = scr;
}

void ui_page_show(ui_page_t p)
{
    if (p < 0 || p >= UI_PAGE_COUNT) return;
    if (s_splash_active) return;                 /* 启动页期间不许乱跳 */
    if (p == s_page) return;
    if (s_screens[p] == NULL) return;
    s_page = p;
    if (p == UI_PAGE_PLAY) s_play_refresh_all = true;
    ui_pages_enter(p);
    /* 页面切换淡入。⚠️ 这是全屏重绘（40KB/帧），所以时长压到 180ms ——
     * 实测 [ui] lv_timer 最坏耗时没有因为这一步明显变差。 */
#if UI_ANIM
    lv_screen_load_anim(s_screens[p], LV_SCR_LOAD_ANIM_FADE_IN, 180, 0, false);
#else
    lv_screen_load(s_screens[p]);
#endif
    ESP_LOGI(TAG, "页面 -> %d", (int)p);
}

ui_page_t ui_page_current(void) { return s_page; }
bool ui_splash_running(void) { return s_splash_active; }

const char *ui_page_name(ui_page_t p)
{
    switch (p) {
        case UI_PAGE_PLAY:        return "播放页";
        case UI_PAGE_LIST:        return "列表页";
        case UI_PAGE_MENU:        return "菜单";
        case UI_PAGE_VOL:         return "音量页";
        case UI_PAGE_INFO:        return "系统信息";
        case UI_PAGE_BT:          return "蓝牙控制";
        case UI_PAGE_SRC:         return "来源二级";
        case UI_PAGE_SEARCH_MODE: return "搜索方式";
        case UI_PAGE_SEARCH_IN:   return "搜索输入";
        case UI_PAGE_SEARCH_RES:  return "搜索结果";
        default:                  return "?";
    }
}

/* ============================================================ 顶部提示（toast） */

static SemaphoreHandle_t s_toast_lock;
static char              s_toast_text[64];
static int64_t           s_toast_until;
static lv_obj_t         *s_toast_box, *s_toast_lbl;

void ui_toast(const char *text)
{
    if (s_toast_lock == NULL || text == NULL) return;
    xSemaphoreTake(s_toast_lock, portMAX_DELAY);
    strlcpy(s_toast_text, text, sizeof(s_toast_text));
    s_toast_until = esp_timer_get_time() + 1800000;    /* 1.8 秒 */
    xSemaphoreGive(s_toast_lock);
}

#define TOAST_Y 93
static bool s_toast_fading;
static void toast_hidden_cb(lv_anim_t *a)
{
    lv_obj_add_flag((lv_obj_t *)a->var, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_local_style_prop((lv_obj_t *)a->var, LV_STYLE_OPA, 0);
    s_toast_fading = false;
}

static void build_toast(void)
{
    /* 挂在 top layer 上：任何页面都能看到，不需要每页各建一个 */
    lv_obj_t *top = lv_layer_top();
    s_toast_box = lv_obj_create(top);
    lv_obj_remove_flag(s_toast_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_toast_box, 5, TOAST_Y);
    lv_obj_set_size(s_toast_box, 150, 22);
    lv_obj_set_style_bg_color(s_toast_box, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_bg_opa(s_toast_box, LV_OPA_90, 0);
    lv_obj_set_style_radius(s_toast_box, 6, 0);
    lv_obj_set_style_border_width(s_toast_box, 0, 0);
    lv_obj_set_style_pad_all(s_toast_box, 0, 0);

    s_toast_lbl = ui_put_label_1line(s_toast_box, "", 6, 3, 138, ui_font_12, 0xFFFFFF);
    lv_obj_set_style_text_align(s_toast_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(s_toast_box, LV_OBJ_FLAG_HIDDEN);
}

static void toast_apply(void)
{
    if (s_toast_box == NULL) return;
    char text[64];
    int64_t until;
    xSemaphoreTake(s_toast_lock, portMAX_DELAY);
    strlcpy(text, s_toast_text, sizeof(text));
    until = s_toast_until;
    xSemaphoreGive(s_toast_lock);

    bool show = (text[0] != 0) && (esp_timer_get_time() < until);
    bool hidden = lv_obj_has_flag(s_toast_box, LV_OBJ_FLAG_HIDDEN);
    if (show) {
        ui_set_text(s_toast_lbl, text);
        if (hidden) {
            lv_obj_remove_flag(s_toast_box, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_opa(s_toast_box, LV_OPA_TRANSP, 0);
            anim_slide_y(s_toast_box, TOAST_Y + 8, TOAST_Y, 200);   /* 从下方滑上来 */
            anim_fade_in(s_toast_box, 200);
        }
    } else if (!hidden && !s_toast_fading) {
        /* 已经在淡出中就别重复启动 —— 否则动画叠加，ready 回调会互相打架
         * （表现是提示条卡在半透明状态不消失） */
        s_toast_fading = true;
        anim_fade_out(s_toast_box, 180, toast_hidden_cb);
    }
}

/* ============================================================ 字库缺字替换
 *
 * ⚠️ 上屏字符串里出现字库没有的字符时，LVGL 什么都画不出来 —— 结果是【一片空白】。
 *    实测用户的曲库里有首歌叫 "ᯤ"（U+1BE4，Batak 文字），屏幕上歌名就是空的，
 *    用户报的是"为什么显示不出歌名"。
 *    字库是 GB2312 + 中文标点 + 拉丁（Montserrat 兜底），任何超出范围的字符
 *    （emoji、泰文、Batak、罕见符号…）都会踩这个坑。
 *
 * 做法：逐码点查两次 —— 先查中文字库，再查它的 fallback（ASCII 在 fallback 里）——
 *       查不到就换成 '?'。宁可显示一个问号，也别让用户看到空白。
 */
static bool font_has_glyph(const lv_font_t *f, uint32_t cp)
{
    lv_font_glyph_dsc_t d;
    if (f == NULL) return false;
    if (lv_font_get_glyph_dsc(f, &d, cp, 0)) return true;
    if (f->fallback && lv_font_get_glyph_dsc(f->fallback, &d, cp, 0)) return true;
    return false;
}

/* 把 UI 文案里用到的字符逐个查字库，缺的报出来。
 * 列表由 tools/gen_ui_text.py 从源码里的字符串字面量抽出来（改了文案要重跑那个脚本）。 */
/* ⚠️ 光查"字体里有没有这个码点"不够：还有一种坑是**码点有、但字形位图是空的**
 * （源字体把某些符号映射成了空轮廓）。这种情况下 LVGL 查得到字形、
 * 自检也认为"有"，但屏上什么都不画 —— 我们那个 `…` 就是这一类
 * （charset.txt 说有、码点也在，可屏上是空的）。所以这里连 box 尺寸一起看。 */
static bool glyph_bad(uint32_t cp)
{
    lv_font_glyph_dsc_t d;
    if (lv_font_get_glyph_dsc(ui_font_16, &d, cp, 0) && d.box_w > 0 && d.box_h > 0) return false;
    if (lv_font_get_glyph_dsc(ui_font_12, &d, cp, 0) && d.box_w > 0 && d.box_h > 0) return false;
    return true;
}


void ui_font_audit_text(void)
{
    int missing = 0;
    const char *p = UI_TEXT_CHARS;
    while (*p) {
        /* UTF-8 解码出一个码点 */
        unsigned char c = (unsigned char)*p;
        uint32_t cp = 0;
        int extra = 0;
        if (c < 0x80)        { cp = c;         extra = 0; }
        else if (c < 0xE0)   { cp = c & 0x1F;  extra = 1; }
        else if (c < 0xF0)   { cp = c & 0x0F;  extra = 2; }
        else                 { cp = c & 0x07;  extra = 3; }
        p++;
        for (int i = 0; i < extra && *p; i++, p++) {
            cp = (cp << 6) | ((unsigned char)*p & 0x3F);
        }
        if (glyph_bad(cp)) {
            ESP_LOGE(TAG, "[字库] UI 文案用到的 U+%04X 屏上画不出来（缺码点或字形是空的）"
                          " -- 换个字符，否则那格是空白", (unsigned)cp);
            missing++;
        }
    }
    ESP_LOGW(TAG, "UI 文案字库自检：%d 个非 ASCII 字符，画不出来 %d 个",
             UI_TEXT_CHARS_COUNT, missing);

    /* 固定探针（不过这些只用于诊断，屏上也未必用到） */
    char bad[64];
    int n = 0;
    const char *q = UI_FONT_PROBES;
    while (*q) {
        unsigned char c = (unsigned char)*q++;
        uint32_t cp = 0;
        int extra = (c < 0x80) ? 0 : (c < 0xE0 ? 1 : (c < 0xF0 ? 2 : 3));
        cp = (c < 0x80) ? c : (extra == 1 ? (c & 0x1F) : (c < 0xF0 ? (c & 0x0F) : (c & 0x07)));
        for (int i = 0; i < extra && *q; i++, q++) cp = (cp << 6) | ((unsigned char)*q & 0x3F);
        if (glyph_bad(cp)) {
            if (n + 8 < (int)sizeof(bad)) n += snprintf(bad + n, sizeof(bad) - n, "U+%04X ", (unsigned)cp);
        }
    }
    bad[n] = 0;
    ESP_LOGW(TAG, "字形探针检查完毕，画不出来的: %s", n ? bad : "（无）");
}

void ui_sanitize_text(const char *in, char *out, size_t out_len, const lv_font_t *font)
{
    if (out == NULL || out_len == 0) return;
    if (in == NULL) { out[0] = 0; return; }
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < out_len; ) {
        unsigned char c = (unsigned char)*p;
        uint32_t cp = 0;
        int n = 1;
        if (c < 0x80)                     { cp = c;        n = 1; }
        else if ((c & 0xE0) == 0xC0)      { cp = c & 0x1F; n = 2; }
        else if ((c & 0xF0) == 0xE0)      { cp = c & 0x0F; n = 3; }
        else if ((c & 0xF8) == 0xF0)      { cp = c & 0x07; n = 4; }
        else                              { p++; continue; }   /* 非法起始字节，丢掉 */
        bool ok = true;
        for (int i = 1; i < n; i++) {
            if (((unsigned char)p[i] & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | ((unsigned char)p[i] & 0x3F);
        }
        if (!ok) { p++; continue; }                            /* 截断的多字节，丢掉 */
        if (font_has_glyph(font, cp)) {
            if (o + (size_t)n < out_len) { memcpy(out + o, p, (size_t)n); o += (size_t)n; }
        } else {
            ESP_LOGW(TAG, "[字库] 缺字形 U+%04X，屏上换成 ? 显示", (unsigned)cp);
            out[o++] = '?';
        }
        p += n;
    }
    out[o] = 0;
}

/* ============================================================ 动画
 *
 * 设计原则（和"避免整屏重绘"是同一套思路）：
 *   - **只动小面积**：淡入一张 72×72 封面 ≈ 10KB/帧，播放/暂停钮 32×32 ≈ 2KB/帧，
 *     提示条 150×22 ≈ 6.6KB/帧 —— 都是几 KB 级别，SPI 40MHz 完全吃得下。
 *     而全屏淡入是 40KB/帧，所以页面切换只给 180ms 的短淡入，且实测过开销。
 *   - **用 LVGL 自带的 anim**，不自己起定时器逐帧改（LVGL 的 anim 是脏矩形驱动的）。
 *   - **时长都短**（180~260ms），别拖成长动画一直占着重绘。
 *
 * ⚠️ 不要给进度条加动画：它是 4Hz 步进值，淡过去会看起来"追不上"。
 */
/* 0 = 关掉全部动画（用来做 A/B 实测开销，平时保持 1） */
#define UI_ANIM 1

static void anim_opa_cb(void *var, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}
static void anim_y_cb(void *var, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)var, v);
}

/* 淡入结束后把 opa 样式【清掉】。
 * ⚠️ 这一步是必须的，不是洁癖：LVGL 对"设了 opa 样式的对象"会走图层/混合路径，
 *    样式留着就等于每帧重绘整个对象 —— 实测封面 72×72 会让 5 秒刷新量
 *    从 ~7KB 涨到 ~50KB（4 倍），白白吃掉 SPI 带宽和 UI 任务时间。
 *    动画本身只要 240ms，代价可以接受；留下的样式才是问题。 */
static void anim_clear_opa_cb(lv_anim_t *a)
{
    lv_obj_remove_local_style_prop((lv_obj_t *)a->var, LV_STYLE_OPA, 0);
}

static void anim_fade_in(lv_obj_t *o, uint32_t ms)
{
#if !UI_ANIM
    lv_obj_set_style_opa(o, LV_OPA_COVER, 0);
    return;
#endif
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_values(&a, 0, 255);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_exec_cb(&a, anim_opa_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_ready_cb(&a, anim_clear_opa_cb);
    lv_anim_start(&a);
}

/* 淡出，结束后回调（提示条用它收起来） */
static void anim_fade_out(lv_obj_t *o, uint32_t ms, lv_anim_ready_cb_t done)
{
#if !UI_ANIM
    lv_obj_set_style_opa(o, LV_OPA_TRANSP, 0);
    if (done) { lv_anim_t d; lv_anim_init(&d); lv_anim_set_var(&d, o); done(&d); }
    return;
#endif
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_values(&a, lv_obj_get_style_opa(o, 0), 0);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_exec_cb(&a, anim_opa_cb);
    if (done) lv_anim_set_ready_cb(&a, done);
    lv_anim_start(&a);
}

/* 纵向滑动（提示条从下方滑上来用） */
static void anim_slide_y(lv_obj_t *o, int from, int to, uint32_t ms)
{
#if !UI_ANIM
    lv_obj_set_y(o, to);
    return;
#endif
    lv_obj_set_y(o, from);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_exec_cb(&a, anim_y_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/* "跳一下"：按下去有反馈。位移 3px 再回来，面积小到几乎免费。 */
static void anim_hop(lv_obj_t *o, int dy, uint32_t ms)
{
#if !UI_ANIM
    (void)o; (void)dy; (void)ms;
    return;
#endif
    int y0 = lv_obj_get_y(o);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_values(&a, y0, y0 - dy);
    lv_anim_set_duration(&a, ms / 2);
    lv_anim_set_exec_cb(&a, anim_y_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    /* 用 playback 自动弹回来，省一次手动反向动画 */
    lv_anim_set_playback_duration(&a, ms / 2);
    lv_anim_set_playback_delay(&a, 0);
    lv_anim_start(&a);
}

/* 给别的文件用（ui_pages.c 的高亮滑动）：滑到绝对位置 y */
void ui_anim_slide_obj(lv_obj_t *o, int y, uint32_t ms)
{
    if (o == NULL) return;
    int y0 = lv_obj_get_y(o);
    if (y0 == y) return;
    anim_slide_y(o, y0, y, ms);
}

/* ============================================================ 播放页 */

static void play_refresh(void)
{
    player_status_t st;
    player_status_get(&st);

    /* 实时位置/播放状态直接从引擎的原子量读（无锁）
     * ⚠️⭐ 暂停时引擎状态【仍然是 PLAYING】（暂停是个独立标志：out 任务继续往 I2S
     *    写静音、位置冻结，状态机里根本不出 PAUSED）。
     *    所以 "playing" 必须写成 "状态是 PLAYING 且没暂停" —— 只看状态的话，
     *    暂停时 playing 还是 true，播放/暂停图标就永远不翻（用户报的
     *    "按了会暂停但按钮不变化"就是这个）。 */
    uint32_t pos = audio_engine_position_ms();
    ae_state_t aes = audio_engine_state();
    bool paused  = audio_engine_is_paused();
    bool playing = (aes == AE_STATE_PLAYING) && !paused;
    /* 顺手把最新进度写回共享快照，供不依赖引擎的读者（M5 的网页）使用 */
    player_status_set_progress(pos, playing, paused, aes == AE_STATE_FINISHED);

    bool force = s_play_refresh_all;
    s_play_refresh_all = false;

    /* 换歌了：刷新文字与封面 */
    if (force || st.gen != s_ui_gen) {
        s_ui_gen = st.gen;
#if UI_DUMP_ON_SONG
        /* 排障开关：换歌时把屏幕像素打到串口（见 ui_display.c 的说明）。
         * 默认关 —— 打开后每换一首歌会刷十几块字符画，只适合排查"屏上显示不对"。 */
        ui_display_dump_arm(14);
#endif
        /* 过一遍字库缺字替换，否则罕见字符会让整行变空白 */
        char tbuf[176], abuf[112];
        /* ⚠️ 空标题的占位别用 "—"（U+2014 破折号）：字体里【没有】这个码点，屏上是一片空白
         * （开机自检查出来的）。用中文，字库里一定有。 */
        ui_sanitize_text(st.title[0] ? st.title : "(无标题)", tbuf, sizeof(tbuf), ui_font_16);
        ui_sanitize_text(st.artist, abuf, sizeof(abuf), ui_font_12);
        ui_set_text(s_lbl_title, tbuf);
        ui_set_text(s_lbl_artist, abuf);
        /* 这行是给"屏上显示不出歌名"这类问题排查用的：能一眼看出是替换误伤还是真缺字 */
        ESP_LOGI(TAG, "屏上显示: 歌名=\"%s\" 作者=\"%s\"（原始歌名 \"%s\"）",
                 tbuf, abuf, st.title);
        char buf[16];
        ui_fmt_mmss(st.duration_ms, buf, sizeof(buf));
        ui_set_text(s_lbl_time_tot, buf);
        /* 封面：**优先复用已经解码好的当前封面**，没有才退回占位图。
         * ⚠️ 这里以前是无条件设成占位图 —— 于是"长按 K4 进菜单再回来"封面就没了：
         *    回到播放页会走这条 force 分支（s_play_refresh_all），而真封面只在
         *    "新封面刚解码好"（下面的 s_cover_ready）时才贴；歌曲没变就不会重新触发，
         *    占位图就一直挂到下一首歌。（用户实测报的就是这个。） */
        if (s_cover_dsc.data != NULL && s_cover_gen == st.gen) {
            lv_image_set_src(s_img_cover, &s_cover_dsc);
            /* 这行日志是"回页面后封面还在不在"的直接证据 ——
             * 别再靠"看起来没变"来判断（用户报过一次，我一开始只验证了逻辑没验证现象）。 */
            ESP_LOGI(TAG, "封面: 复用第 %u 代已解码的封面", (unsigned)st.gen);
        } else {
            asset_t ph;
            if (assets_get(ASSET_PLACEHOLDER_COVER_72, &ph)) {
                lv_image_dsc_t *d = ui_img_dsc(ASSET_PLACEHOLDER_COVER_72);
                if (d) lv_image_set_src(s_img_cover, d);
            }
            ESP_LOGI(TAG, "封面: 还没有当前这代的封面，先挂占位图");
        }
        lv_image_set_src(s_img_star, ui_img_dsc(st.starred ? ASSET_IC_STAR_ON : ASSET_IC_STAR_OFF));
        /* 强制把播放状态图标也重刷（离屏期间可能变过） */
        lv_image_set_src(s_img_state,
                         ui_img_dsc(playing ? ASSET_IC_PAUSE_BIG : ASSET_IC_PLAY_BIG));
    }

    /* 封面异步到达且属于当前这一代 */
    if (s_cover_ready && s_cover_gen == s_ui_gen) {
        s_cover_ready = false;
        s_cover_dsc.header.cf = LV_COLOR_FORMAT_RGB565_SWAPPED;   /* 解码器输出大端 */
        lv_image_set_src(s_img_cover, &s_cover_dsc);
        anim_fade_in(s_img_cover, 240);      /* 新封面淡入，比"啪一下换掉"顺眼 */
    }

    /* 进度条与时间：只在变化时更新（重绘面积 = 进度条本身，约 1KB） */
    uint32_t dur = st.duration_ms ? st.duration_ms : 1;
    if (pos > dur) pos = dur;
    int32_t permille = (int32_t)((uint64_t)pos * 1000 / dur);
    if (lv_bar_get_value(s_bar) != permille) {
        /* ⚠️ 这里【不能】用 LV_ANIM_ON：4Hz 步进值淡过去会像"追不上"。
         *    只有音量/亮度那种"一下跳 5%"的才适合动画，见 ui_pages.c。 */
        lv_bar_set_value(s_bar, permille, LV_ANIM_OFF);
    }
    char buf[16];
    ui_fmt_mmss(pos, buf, sizeof(buf));
    ui_set_text(s_lbl_time_cur, buf);

    /* 收藏状态（长按 K1 之后要立刻反映） */
    static bool last_starred;
    if (force || st.starred != last_starred) {
        last_starred = st.starred;
        lv_image_set_src(s_img_star, ui_img_dsc(st.starred ? ASSET_IC_STAR_ON
                                                           : ASSET_IC_STAR_OFF));
    }

    /* 播放状态图标（语义 = "按下去会发生什么"，与播放器一致）：
     *   正在播放 → 竖杠 ⏸（按下暂停）
     *   暂停/停止 → 三角 ▶（按下播放）
     * 一开始我按"当前状态"显示（播放中显示三角），用户反馈"为什么不变竖杠" → 反过来了。 */
    static int last_state = -1;
    int state = playing ? 1 : 0;
    if (force || state != last_state) {
        bool first = (last_state == -1);
        last_state = state;
        lv_image_set_src(s_img_state,
                         ui_img_dsc(state ? ASSET_IC_PAUSE_BIG : ASSET_IC_PLAY_BIG));
        if (!first) anim_hop(s_img_state, 3, 200);   /* 状态变了才跳，开机别乱动 */
    }
}

/* 播放页（垂直预算见 tools/gen_ui_png.py 的注释，每行都按实际行高留够） */
static void build_play_screen(void)
{
    lv_obj_t *scr = ui_new_screen();
    ui_page_register(UI_PAGE_PLAY, scr);

    ui_put_img(scr, ASSET_BG_PLAY_A, 0, 0);              /* 预渲染底图 */
    ui_put_img(scr, ASSET_IC_NOTE, 4, 5);                /* 顶栏音符 */

    /* 顶栏：只显歌名、截断、不带作者 */
    s_lbl_title = ui_put_label(scr, "", 20, 2, 132, ui_font_16, 0x1C1C1E,
                               LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_duration(s_lbl_title, 9000, 0);   /* 滚一轮 9 秒，太快看不清 */

    /* 顶栏右侧：收藏状态 */
    s_img_star = ui_put_img(scr, ASSET_IC_STAR_OFF, 144, 4);

    /* 封面（先占位图，封面到了换成真实封面） */
    s_img_cover = ui_put_img(scr, ASSET_PLACEHOLDER_COVER_72, 5, 22);

    /* 右栏信息卡：只放作者。
     * 必须同时限定宽和高，否则 LVGL 的 label 会自己长高、溢出卡片。
     * 作者名经常很长（实测 "Le Bober/George Daniel/Adam Hann/..."），
     * 单行 + 循环滚动才看得全 */
    s_lbl_artist = ui_put_label(scr, "", 86, 32, 66, ui_font_12, 0x3C3C40,
                               LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_height(s_lbl_artist, 16);
    lv_obj_set_style_anim_duration(s_lbl_artist, 9000, 0);

    /* 作者下方：播放状态按钮（32x32 圆钮，居中在卡片里 81..155 → x=102） */
    s_img_state = ui_put_img(scr, ASSET_IC_PLAY_BIG, 102, 58);

    /* 时间行：已播（左）/ 总时长（右） */
    s_lbl_time_cur = ui_put_label(scr, "0:00",   5, 100, 0, ui_font_lat, 0x6E6E73,
                                 LV_LABEL_LONG_MODE_CLIP);
    s_lbl_time_tot = ui_put_label(scr, "0:00", 126, 100, 0, ui_font_lat, 0x6E6E73,
                                 LV_LABEL_LONG_MODE_CLIP);

    /* 底部全宽细进度条（槽在底图里，这里只画填充，重绘面积=进度条本身） */
    s_bar = lv_bar_create(scr);
    lv_obj_remove_style_all(s_bar);
    lv_obj_set_pos(s_bar, 5, 118);
    lv_obj_set_size(s_bar, 150, 6);
    lv_bar_set_range(s_bar, 0, 1000);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x1A73E8), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);

    ESP_LOGI(TAG, "播放页已建好");
}

/* ============================================================ 显示校准画面
 *
 * 开机轮流展示 4 组显示参数（反相 × RGB/BGR），底部大号数字标出当前是第几组。
 * 用户看到"白条是白的、红条是红的、绿条是绿的、蓝条是蓝的"那一组的编号，
 * 就是这台屏的正确参数 —— 一次烧录就能定位，不用来回试。
 */
typedef struct { bool invert, bgr; } cal_combo_t;

static const cal_combo_t k_cal_combos[] = {
    { true,  true  },
    { false, true  },
    { true,  false },
    { false, false },
};
#define CAL_COMBOS (sizeof(k_cal_combos) / sizeof(k_cal_combos[0]))
#define CAL_INTERVAL_MS 6000

static int        s_cal_idx;
static lv_obj_t  *s_cal_num;

static void cal_apply(int idx, bool announce)
{
    ui_display_set_invert(k_cal_combos[idx].invert);
    ui_display_set_bgr(k_cal_combos[idx].bgr);
    if (s_cal_num) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", idx + 1);
        lv_label_set_text(s_cal_num, buf);
    }
    if (announce) {
        ESP_LOGW(TAG, "===== 校准组合 %d/%d：invert=%d  bgr=%d  （白/红/绿/蓝 四条色带哪一组正常？）=====",
                 idx + 1, (int)CAL_COMBOS, k_cal_combos[idx].invert, k_cal_combos[idx].bgr);
    }
}

static void cal_timer_cb(lv_timer_t *t)
{
    s_cal_idx = (s_cal_idx + 1) % (int)CAL_COMBOS;
    cal_apply(s_cal_idx, true);
}

static void build_cal_screen(void)
{
    lv_obj_t *scr = ui_new_screen();

    static const uint32_t bands[4] = { 0xFFFFFF, 0xFF0000, 0x00FF00, 0x0000FF };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = lv_obj_create(scr);
        lv_obj_remove_style_all(b);
        lv_obj_set_pos(b, 0, i * 28);
        lv_obj_set_size(b, 160, 28);
        lv_obj_set_style_bg_color(b, lv_color_hex(bands[i]), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_t *strip = lv_obj_create(scr);
    lv_obj_remove_style_all(strip);
    lv_obj_set_pos(strip, 0, 112);
    lv_obj_set_size(strip, 160, 16);
    lv_obj_set_style_bg_color(strip, lv_color_hex(0x3C3C3C), 0);
    lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, 0);

    s_cal_num = lv_label_create(scr);
    lv_obj_set_style_text_font(s_cal_num, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_cal_num, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(s_cal_num, "1");
    lv_obj_set_pos(s_cal_num, 74, 106);

    lv_screen_load(scr);
    ESP_LOGW(TAG, "显示校准画面：每 %d 秒换一组参数，共 %d 组", CAL_INTERVAL_MS, (int)CAL_COMBOS);
}

/* ============================================================ 封面异步加载
 *
 * 低优先级、core 0 —— 绝不和 UI/音频抢核。看到 gen 变化就去拉封面 + JPEG 解码，
 * 结果连同 gen 一起交给 UI，UI 只接受"属于当前这一代"的封面（否则快速连按
 * "下一首"必然串台）。
 */
/* 已知取不到封面的 cover id 黑名单。
 * 起因：Navidrome 对"内嵌封面是 WebP"的专辑会原样透传 WebP（它的图像库转不了码），
 * 而 esp_new_jpeg 只认 JPEG。不记下来的话，每次播到这些专辑都要白下一次几万字节。 */
#define COVER_BAD_MAX 24
static char s_cover_bad[COVER_BAD_MAX][48];
static int  s_cover_bad_n;

static bool cover_is_bad(const char *id)
{
    for (int i = 0; i < s_cover_bad_n; i++) {
        if (!strcmp(s_cover_bad[i], id)) return true;
    }
    return false;
}

static void cover_mark_bad(const char *id)
{
    if (cover_is_bad(id)) return;
    if (s_cover_bad_n == COVER_BAD_MAX) {          /* 满了：丢最旧的一条 */
        memmove(s_cover_bad, s_cover_bad + 1, sizeof(s_cover_bad[0]) * (COVER_BAD_MAX - 1));
        s_cover_bad_n--;
    }
    strlcpy(s_cover_bad[s_cover_bad_n++], id, sizeof(s_cover_bad[0]));
    ESP_LOGW(TAG, "封面 %s 记入黑名单（共 %d 个），不再重试", id, s_cover_bad_n);
}

/* ---------------- M6：封面落盘缓存 ----------------
 *
 * 每首歌的封面要"下载 + JPEG 解码"两跳，实测 300~900ms —— 重复播同一张专辑时
 * 完全没必要重来。这里把**解码后的 RGB565** 整块存进 cache 分区（`COVER/<哈希>.565`），
 * 命中就完全不走网络。
 *
 * 文件布局（头 48 字节 + 像素）：
 *   0..3   'E','M','C','V'      魔数
 *   4,5    w, h                  像素尺寸（换 COVER_PX 后老缓存自动失效）
 *   6      版本（=1）
 *   7      保留
 *   8..47  cover id（NUL 填充）  ⚠️ 文件名是 32 位哈希，会撞；头里存原 id 用来
 *                                识破碰撞（撞了就当没缓存，重新下，不显示错封面）
 */
#define COVER_HDR        48
#define COVER_CACHE_MAX  300
#define COVER_CACHE_KEEP 240

/* 像素数据在缓冲里的位置（缓冲前面 48 字节是文件头） */
#define COVER_PIX (s_cover_buf + COVER_HDR)

/* 数目录的开销（一次 FATFS 目录遍历）不值得每存一张就做一次，也不该在
 * ui_start 时数一次就当真 —— store_init() 排在 ui_start 之后，那时还没挂载。
 * 所以每存 COVER_COUNT_EVERY 张才真去数一遍，超了才淘汰：简单且自我校正。 */
#define COVER_COUNT_EVERY 16
static int s_cover_stores;

static uint32_t str_hash32(const char *s)
{
    uint32_t h = 2166136261u;                  /* FNV-1a */
    for (const uint8_t *p = (const uint8_t *)s; *p; p++) {
        h ^= *p;
        h *= 16777619u;
    }
    return h;
}

/* 名字必须严格 8.3（FATFS 关着长文件名，超长会创建失败） */
static void cover_cache_name(const char *cover_id, char *out, size_t cap)
{
    snprintf(out, cap, "COVER/%08X.565", (unsigned)str_hash32(cover_id));
}

static bool cover_cache_load(const char *cover_id)
{
    if (!store_ready() || s_cover_buf == NULL) return false;
    char name[32];
    cover_cache_name(cover_id, name, sizeof(name));

    size_t n = 0;
    /* 一次读整份（头 + 像素），正好填满缓冲 —— 分两次读要多一次 open/close */
    if (store_read(name, s_cover_buf, COVER_HDR + COVER_BYTES, &n) != ESP_OK) return false;
    if (n != (size_t)(COVER_HDR + COVER_BYTES)) return false;
    if (memcmp(s_cover_buf, "EMCV", 4) != 0) return false;
    if (s_cover_buf[4] != COVER_PX || s_cover_buf[5] != COVER_PX || s_cover_buf[6] != 1) {
        return false;                       /* 尺寸或版本变了，老缓存作废 */
    }
    if (strncmp((const char *)s_cover_buf + 8, cover_id, COVER_HDR - 8) != 0) {
        ESP_LOGW(TAG, "封面缓存哈希撞了（%s），丢掉重下", cover_id);
        return false;
    }
    return true;
}

static void cover_cache_store(const char *cover_id)
{
    if (!store_ready() || s_cover_buf == NULL) return;

    memcpy(s_cover_buf, "EMCV", 4);
    s_cover_buf[4] = COVER_PX;
    s_cover_buf[5] = COVER_PX;
    s_cover_buf[6] = 1;
    s_cover_buf[7] = 0;
    memset(s_cover_buf + 8, 0, COVER_HDR - 8);
    strlcpy((char *)s_cover_buf + 8, cover_id, COVER_HDR - 8);

    char name[32];
    cover_cache_name(cover_id, name, sizeof(name));
    if (store_write(name, s_cover_buf, COVER_HDR + COVER_BYTES) != ESP_OK) return;

    if (++s_cover_stores % COVER_COUNT_EVERY == 0
        && store_dir_count("COVER") > COVER_CACHE_MAX) {
        store_evict("COVER", COVER_CACHE_KEEP);
    }
}

static void cover_publish(uint32_t gen)
{
    s_cover_dsc.header.w  = COVER_PX;
    s_cover_dsc.header.h  = COVER_PX;
    s_cover_dsc.data_size = COVER_BYTES;
    s_cover_dsc.data      = COVER_PIX;
    s_cover_gen   = gen;
    s_cover_ready = true;
}

static void cover_task(void *arg)
{
    uint32_t last_gen = 0;
    for (;;) {
        player_status_t st;
        player_status_get(&st);
        if (st.gen != last_gen && st.cover_id[0] && s_cover_buf
            && !cover_is_bad(st.cover_id)) {
            last_gen = st.gen;
            if (cover_cache_load(st.cover_id)) {
                cover_publish(st.gen);              /* 命中：不碰网络 */
                ESP_LOGI(TAG, "封面命中缓存: %s", st.cover_id);
            } else {
                uint16_t w = 0, h = 0;
                esp_err_t ce = subsonic_get_cover_rgb565(st.cover_id, COVER_PX, COVER_PIX,
                                                         COVER_BYTES, &w, &h);
                if (ce == ESP_OK && w == COVER_PX && h == COVER_PX) {
                    cover_publish(st.gen);
                    cover_cache_store(st.cover_id); /* 存下来给下次/下次开机 */
                } else if (ce != ESP_OK) {
                    cover_mark_bad(st.cover_id);    /* 占位图继续显示，不再反复尝试 */
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/* ============================================================ 启动页
 *
 * 开机要连 WiFi + 拉歌单，实测 10~19 秒。没有反馈用户会以为卡死，所以给一个
 * 看得见的进度（logo + 状态文字 + 进度条），每步更新一次。
 *
 * 线程安全：别的任务只往 s_splash_* 写字符串（加锁），由 UI 任务在 tick 里应用 ——
 * 不让别的任务直接碰 LVGL 对象。
 */
static SemaphoreHandle_t s_splash_lock;
static char  s_splash_text[64];
static int   s_splash_step, s_splash_total = 1;
static bool  s_splash_finish_req;

static lv_obj_t *s_splash_lbl, *s_splash_bar, *s_scr_splash;

static void build_splash_screen(void)
{
    lv_obj_t *scr = ui_new_screen();
    s_scr_splash = scr;
    ui_put_img(scr, ASSET_BG_SPLASH, 0, 0);

    s_splash_lbl = ui_put_label_1line(scr, "启动中...", 0, 74, 160, ui_font_12, 0x6E6E73);
    lv_obj_set_style_text_align(s_splash_lbl, LV_TEXT_ALIGN_CENTER, 0);

    s_splash_bar = lv_bar_create(scr);
    lv_obj_remove_style_all(s_splash_bar);
    lv_obj_set_pos(s_splash_bar, 5, 104);
    lv_obj_set_size(s_splash_bar, 150, 6);
    lv_bar_set_range(s_splash_bar, 0, 1000);
    lv_bar_set_value(s_splash_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_opa(s_splash_bar, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_splash_bar, lv_color_hex(0x1A73E8), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_splash_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_splash_bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
}

void ui_splash_set(const char *text, int step, int total)
{
    if (s_splash_lock == NULL) return;
    xSemaphoreTake(s_splash_lock, portMAX_DELAY);
    strlcpy(s_splash_text, text ? text : "", sizeof(s_splash_text));
    s_splash_step  = step;
    s_splash_total = total > 0 ? total : 1;
    xSemaphoreGive(s_splash_lock);
}

void ui_splash_finish(void)
{
    if (s_splash_lock == NULL) return;
    xSemaphoreTake(s_splash_lock, portMAX_DELAY);
    s_splash_finish_req = true;
    xSemaphoreGive(s_splash_lock);
}

/* UI 任务侧：应用启动页状态；收到 finish 就切到播放页 */
static void splash_apply(void)
{
    char text[64];
    int step, total;
    bool fin;
    xSemaphoreTake(s_splash_lock, portMAX_DELAY);
    strlcpy(text, s_splash_text, sizeof(text));
    step = s_splash_step;
    total = s_splash_total;
    fin = s_splash_finish_req;
    s_splash_finish_req = false;
    xSemaphoreGive(s_splash_lock);

    if (fin && s_splash_active) {
        s_splash_active = false;
        s_page = UI_PAGE_PLAY;
        s_play_refresh_all = true;
        lv_screen_load(s_screens[UI_PAGE_PLAY]);
        ESP_LOGI(TAG, "启动页结束 -> 播放页");
        return;
    }
    if (!s_splash_active) return;

    ui_set_text(s_splash_lbl, text);
    int32_t v = (int32_t)((int64_t)step * 1000 / total);
    if (lv_bar_get_value(s_splash_bar) != v) {
        lv_bar_set_value(s_splash_bar, v, LV_ANIM_OFF);
    }
}

/* ============================================================ 状态同步（250ms） */

#define UI_TICK_MS 250

static void ui_tick_cb(lv_timer_t *t)
{
    splash_apply();
    toast_apply();
    if (s_splash_active) return;      /* 还在启动页：先不管别的页面 */

    ui_bt_mode_follow();              /* 蓝牙控制模式切了就把页面跟过去（无条件！） */

    if (s_page == UI_PAGE_PLAY) {
        play_refresh();
    } else {
        /* 不在播放页时也要把进度写进快照（网页控制台要用） */
        player_status_set_progress(audio_engine_position_ms(),
                                   audio_engine_state() == AE_STATE_PLAYING,
                                   audio_engine_is_paused(),
                                   audio_engine_state() == AE_STATE_FINISHED);
        ui_pages_tick();
    }
}

/* ============================================================ UI 任务 */

static void ui_task(void *arg)
{
    uint32_t worst = 0, n = 0;
    uint32_t last_px = 0, last_cnt = 0;
    while (1) {
        uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000);
        uint32_t idle = lv_timer_handler();
        uint32_t dt = (uint32_t)(esp_timer_get_time() / 1000) - t0;
        if (dt > worst) worst = dt;
        if (++n >= 200) {
            uint32_t px, cnt;
            ui_display_flush_stats(&px, &cnt);
            /* 5 秒窗口：刷新字节数 / 5s；整屏一次 = 40KB。数值很高就是"在反复重绘"，
             * 数值很低但 lv_timer 很慢，则是 LVGL 在等上一次 DMA 完成 */
            ESP_LOGI(TAG, "[ui] lv_timer 最坏 %" PRIu32 " ms | 5 秒内刷新 %" PRIu32 " 次 / %" PRIu32 " KB（整屏=40KB）",
                     worst, cnt - last_cnt, (px - last_px) * 2 / 1024);
            n = 0; worst = 0;
            last_px = px; last_cnt = cnt;
        }
        uint32_t wait = idle > 10 ? 10 : idle;
        vTaskDelay(pdMS_TO_TICKS(wait ? wait : 1));
    }
}

esp_err_t ui_start(void)
{
    if (assets_init() != ESP_OK) {
        ESP_LOGW(TAG, "素材没准备好，UI 会缺图（先跑 tools/gen_assets.py 并烧 assets 分区）");
    }
    if (ui_display_init() != ESP_OK) {
        return ESP_FAIL;
    }

    /* 中文字体：直接引用 mmap 的 flash，RAM 占用≈0；ASCII 靠内置字体兜底
     * （源字体 DroidSansFallback 的 ASCII 是全角的，子集里已排除） */
    asset_t fa;
    if (assets_get(ASSET_FONT_CJK_16, &fa) && fa.fmt == 1) {
        ui_font_16 = lv_binfont_create_from_buffer((void *)fa.data, fa.size);
        if (ui_font_16) {
            ui_font_16->fallback = (lv_font_t *)&lv_font_montserrat_14;
            ESP_LOGI(TAG, "中文字库 16px 加载成功（%u KB，零拷贝）", (unsigned)(fa.size / 1024));
        } else {
            ESP_LOGE(TAG, "16px 中文字库解析失败");
        }
    }
    if (assets_get(ASSET_FONT_CJK_12, &fa) && fa.fmt == 1) {
        ui_font_12 = lv_binfont_create_from_buffer((void *)fa.data, fa.size);
        if (ui_font_12) {
            ui_font_12->fallback = (lv_font_t *)&lv_font_montserrat_12;
            ESP_LOGI(TAG, "中文字库 12px 加载成功（%u KB，零拷贝）", (unsigned)(fa.size / 1024));
        } else {
            ESP_LOGE(TAG, "12px 中文字库解析失败");
        }
    }
    ui_font_lat = (lv_font_t *)&lv_font_montserrat_12;
    /* 行高一定要打出来：列表行 22px / 菜单行 17px / 信息行 15px 都是硬编码的，
     * 换字库后如果行高变了，行距就不对了（"折行盖住下一行"就是这么来的）。*/
    ESP_LOGI(TAG, "行高: 16px=%d  12px=%d  拉丁12=%d ｜ 行距: 列表22 菜单17 信息15",
             ui_font_16 ? lv_font_get_line_height(ui_font_16) : -1,
             ui_font_12 ? lv_font_get_line_height(ui_font_12) : -1,
             lv_font_get_line_height(ui_font_lat));

#if UI_CALIBRATE
    /* 显示校准模式：开机轮流试 4 组（invert × RGB/BGR）参数。定下来后把
     * UI_CALIBRATE 改成 0，并把结果写进 ui_display.c 的 s_invert/s_bgr 初值。 */
    build_cal_screen();
    s_cal_idx = 0;
    cal_apply(0, true);
    lv_timer_create(cal_timer_cb, CAL_INTERVAL_MS, NULL);
#else
    /* 四个页面全建好：先显示启动页，数据就绪后 ui_splash_finish() 切到播放页。
     * 运行时绝不建/删 widget，页面切换只 lv_screen_load —— 避免堆碎片与卡顿。 */
    s_splash_lock = xSemaphoreCreateMutex();
    s_toast_lock  = xSemaphoreCreateMutex();
    build_splash_screen();
    build_play_screen();
    ui_pages_build();            /* 列表 / 菜单 / 音量 */
    build_toast();

    /* M8：拼音表（mmap 零拷贝，只读不占 RAM）。失败非致命：拼音搜索用不了，
     * 字母直搜照常。 */
    pinyin_init();

    /* 开机核对：UI 文案里用到的字符，字库到底有没有。
     * 为什么值得专门做一遍（这个坑已经浪费过两轮排查）：charset.txt 是由
     * "传给 lv_font_conv 的 range" 推导的，而 lv_font_conv 对源字体没有的字形
     * **静默跳过** —— 于是文件里写着有 `…`，字体里其实没有，屏上是一片空白
     * 而不是方框。缺字形只有真画到屏上才发现，而我看不见屏幕。
     * 这里逐字符查一遍（font_has_glyph 会连 fallback 一起查），缺谁一眼看到。 */
    ui_font_audit_text();

    s_splash_active = true;      /* 先置位：按键回调里靠它挡住"启动页期间乱跳页" */
    lv_screen_load(s_scr_splash);
    ui_keys_init();              /* K1-K4 按键（esp_timer 驱动，不占任务） */

    /* 封面缓冲（PSRAM）与 dsc —— 数据由 cover_task 填，UI 只换 src */
    /* ⚠️ JPEG 解码器要求 outbuf 16 字节对齐；PSRAM malloc 只保证 8 字节，
     *    用 aligned_alloc 明确对齐（否则会返回 -4 INVALID_PARAM） */
    s_cover_buf = heap_caps_aligned_alloc(16, COVER_HDR + COVER_BYTES, MALLOC_CAP_SPIRAM);
    if (s_cover_buf == NULL) {
        ESP_LOGE(TAG, "封面缓冲分配失败（%u 字节）", COVER_HDR + COVER_BYTES);
    } else {
        memset(&s_cover_dsc, 0, sizeof(s_cover_dsc));
        /* 颜色格式在这里定一次：解码器输出大端，屏幕要的就是这个。
         * 以前只在"封面异步到达"时设，现在 force 分支也会直接用它，所以提前设好。 */
        s_cover_dsc.header.cf = LV_COLOR_FORMAT_RGB565_SWAPPED;
        /* M6：封面会落盘缓存到 cache 分区（COVER/<哈希>.565），命中就不走网络。
         * ⚠️ store_init() 排在 ui_start 之后，所以这里不能去数目录 —— 数了是 0。
         *    淘汰阈值改在 cover_cache_store 里每隔几张数一次（自我校正）。 */
        xTaskCreatePinnedToCore(cover_task, "cover", 6144, NULL, 3, NULL, 0);
    }

    /* 250ms 状态同步：读快照 + 引擎位置 → 更新控件（只在值变化时改） */
    lv_timer_create(ui_tick_cb, UI_TICK_MS, NULL);
    ESP_LOGI(TAG, "数据绑定就绪（%d ms 同步一次，封面任务在 core 0）", UI_TICK_MS);
#endif

    /* 画面建好再开背光，避免上电瞬间花屏。
     * 亮度用 NVS 里存的值（ui_pages_build 里读过），没有就是 100%。 */
    board_backlight_init();
    ui_pages_apply_brightness();

    /* 栈放 PSRAM：ui 任务只读快照 + 画 LVGL，不写 flash（内部 SRAM 太紧，
     * 见 audio_engine.c 里那段说明）。cover 任务【不搬】—— 它要写封面缓存到
     * FATFS，那是 flash 写，栈不能在 PSRAM。 */
    xTaskCreatePinnedToCoreWithCaps(ui_task, "ui", UI_TASK_STACK, NULL,
                                    UI_TASK_PRIO, NULL, UI_TASK_CORE, MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "UI 任务已启动（core %d, prio %d）", UI_TASK_CORE, UI_TASK_PRIO);
    return ESP_OK;
}
