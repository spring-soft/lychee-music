/*
 * 三个次级页面：列表页 / 菜单页 / 音量（亮度）页
 *
 * 布局全部对齐 tools/gen_ui_png.py 预渲染出来的底图坐标 —— 改视觉改那个脚本，
 * 改这里只是在底图上叠动态内容：
 *   bg_list  顶栏 20px + 5 行 × 22px（y = 20/42/64/86/108），行分隔线 6..153
 *   bg_menu  顶栏 20px + 6 行 × 17px（y = 22/39/56/73/90/107），分隔线 10..149
 *   bg_vol   顶栏 20px + 卡片 (5,26)-(154,96) + 槽 (12,74) 136×10
 *
 * 性能约定：行文本用 LV_LABEL_LONG_MODE_DOTS（截断，不做滚动动画）。
 * 5 行 × 16px 文字如果都开循环滚动动画，等于 5 个动画同时在跑，重绘面积会翻几倍；
 * 完整歌名在播放页顶栏滚动显示（那里只有 1 个动画）。
 */
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"   /* pcTaskGetName：崩溃排查要看"死在哪个任务里" */
#include "freertos/task.h"
#include "esp_heap_caps.h"   /* 内存图表页要读 PSRAM 的总量/余量 */
#include "esp_timer.h"
#include "ui.h"
#include "ui_internal.h"
#include "assets.h"
#include "player.h"
#include "app_core.h"
#include "audio_engine.h"
#include "board.h"
#include "net_mgr.h"
#include "store.h"        /* 清空缓存：store_evict / store_free_kb */
#include "settings.h"
#include "hid_remote.h"
#include "qqlogin.h"      /* 设置 → QQ 登录：登录情况 + 扫码（点阵是模块级的，本页自己放大画）*/
#include "qqmusic.h"      /* qqmusic_host()：登录页要显示服务地址 */
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

static const char *TAG = "ui.pages";

/* 音量页的两种模式共用一屏：0 = 调音量，1 = 调亮度。
 * ⚠️ 亮度值本身存在 player 侧（s_bright_target），不在这里存第二份 ——
 * 网页控制台也能改亮度，两处各存一份必然会不一致。 */
static bool s_vol_brightness;

/* ============================================================ 列表页 */

#define LIST_ROWS  5
#define LIST_ROW_H 22
#define LIST_Y0    20          /* 对应 bg_list 的 TOPBAR_H */

static lv_obj_t *s_scr_list;
static lv_obj_t *s_list_top;                     /* 顶栏：来源名 + 位置计数 */
static lv_obj_t *s_list_hl;                      /* 选中高亮（hl_row.png，带 alpha） */
static lv_obj_t *s_list_lbl[LIST_ROWS];
static lv_obj_t *s_list_play[LIST_ROWS];         /* 正在播的小音符指示 */

static int      s_list_sel;
static int      s_list_win;                      /* 可视窗口的第一首下标 */
static int      s_list_r_win = -1, s_list_r_sel = -1, s_list_r_play = -2;
static uint32_t s_list_r_gen = 0xFFFFFFFF;

static void list_window(int n, int count)
{
    if (count <= 0) { s_list_win = 0; return; }
    if (s_list_sel < s_list_win) s_list_win = s_list_sel;
    if (s_list_sel >= s_list_win + n) s_list_win = s_list_sel - n + 1;
    if (s_list_win > count - n) s_list_win = count - n;
    if (s_list_win < 0) s_list_win = 0;
}

static void list_render(bool force)
{
    int count = player_queue_count();
    int play  = player_queue_index();
    uint32_t gen = player_queue_gen();
    int n = count < LIST_ROWS ? count : LIST_ROWS;

    /* 队列变短（换来源）时选中项可能越界 —— 不夹住的话高亮会跑到屏幕外，
     * 而且 K3 会"点了没反应" */
    if (s_list_sel > count - 1) s_list_sel = count > 0 ? count - 1 : 0;
    if (s_list_sel < 0) s_list_sel = 0;

    list_window(n, count);

    bool rows_dirty = force || gen != s_list_r_gen || s_list_win != s_list_r_win
                      || play != s_list_r_play;
    if (rows_dirty) {
        s_list_r_gen  = gen;
        s_list_r_win  = s_list_win;
        s_list_r_play = play;
        for (int i = 0; i < LIST_ROWS; i++) {
            int idx = s_list_win + i;
            sub_song_t s;
            if (i < n && player_queue_get(idx, &s)) {
                char tbuf[168];
                ui_sanitize_text(s.title[0] ? s.title : "(无标题)", tbuf, sizeof(tbuf), ui_font_16);
                ui_set_text(s_list_lbl[i], tbuf);
                /* 正在播的那一行右边点一个小音符 */
                if (idx == play) lv_obj_remove_flag(s_list_play[i], LV_OBJ_FLAG_HIDDEN);
                else             lv_obj_add_flag(s_list_play[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                ui_set_text(s_list_lbl[i], "");
                lv_obj_add_flag(s_list_play[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    if (force || s_list_sel != s_list_r_sel || s_list_win != s_list_r_win) {
        s_list_r_sel = s_list_sel;
        ui_anim_slide_obj(s_list_hl, LIST_Y0 + (s_list_sel - s_list_win) * LIST_ROW_H, 120);
    }

    /* 顶栏：当前**模式** + "3/30"（%.16s 限宽：GCC 会按 const char* 的最坏长度算截断，
     * 不加会被 -Werror=format-truncation 拦下来）
     * ⚠️ 必须用 player_mode_name() 而不是 player_src_name() —— 后者只认 Navidrome 那三个，
     *    切到 QQ 音乐后顶栏会显示成"最近播放"这类完全不相干的名字。 */
    char buf[64];
    if (count > 0) {
        snprintf(buf, sizeof(buf), "%.16s %d/%d", player_mode_name(player_mode()),
                 s_list_sel + 1, count);
    } else {
        snprintf(buf, sizeof(buf), "列表为空");
    }
    ui_set_text(s_list_top, buf);
}

void ui_list_move(int delta)
{
    int count = player_queue_count();
    if (count <= 0) return;
    int sel = s_list_sel + delta;
    if (sel < 0) sel = 0;
    if (sel > count - 1) sel = count - 1;
    if (sel != s_list_sel) {
        s_list_sel = sel;
        list_render(false);
    }
}

void ui_list_play_selected(void)
{
    sub_song_t s;
    if (!player_queue_get(s_list_sel, &s)) {
        ui_toast("列表为空");
        return;
    }
    player_play_index(s_list_sel);
    /* ⚠️ 上屏字符串只能用字库里的字符（GB2312 + 中文标点）。
     * "▶" U+25B6 不在里面，屏上会显示成方框 —— 用文字代替。 */
    char buf[64];
    snprintf(buf, sizeof(buf), "播放 %.40s", s.title);   /* 限宽，见上面 render 的注释 */
    ui_toast(buf);
}

static void build_list_screen(void)
{
    lv_obj_t *scr = ui_new_screen();
    s_scr_list = scr;
    ui_page_register(UI_PAGE_LIST, scr);

    ui_put_img(scr, ASSET_BG_LIST, 0, 0);
    ui_put_img(scr, ASSET_IC_NOTE, 4, 5);

    s_list_top = ui_put_label_1line(scr, "", 20, 2, 134, ui_font_16, 0x1C1C1E);

    /* ⚠️ 高亮必须在行文字【之前】创建：LVGL 的层级 = 创建顺序，
     *    后建的画在上面。建反了高亮会盖住文字。 */
    s_list_hl = ui_put_img(scr, ASSET_HL_ROW, 0, LIST_Y0);

    for (int i = 0; i < LIST_ROWS; i++) {
        int y = LIST_Y0 + i * LIST_ROW_H;
        /* ★ 必须用 1line 版：列表行只有 22px，折行会盖住下一行 */
        s_list_lbl[i] = ui_put_label_1line(scr, "", 10, y + 1, 132, ui_font_16, 0x1C1C1E);
        s_list_play[i] = ui_put_img(scr, ASSET_IC_NOTE, 144, y + 5);
        lv_obj_add_flag(s_list_play[i], LV_OBJ_FLAG_HIDDEN);
    }
    ESP_LOGI(TAG, "列表页已建好（%d 行 x %dpx，窗口式滚动）", LIST_ROWS, LIST_ROW_H);
}

/* ============================================================ 菜单页 */

#define MENU_ROWS  6                       /* 屏上能放 6 行 */
#define MENU_ROW_H 17
#define MENU_Y0    22                      /* 对应 bg_menu 的 TOPBAR_H + 2 */
/* 菜单总项数。2026-10-01 用户要求简化：删掉"音质"（固定用最高）、删掉"蓝牙控制"（BLE 已编掉）。
 * ★ 2026-10-11 用户要求：把空着的那一行用起来 —— 新增「来源」，原来那行「来源」改名「模式」。
 *   现在正好 6 项，一屏 MENU_ROWS=6 行放满，不用滚动。 */
#define MENU_N     6

static lv_obj_t *s_scr_menu;
static lv_obj_t *s_menu_hl;
static lv_obj_t *s_menu_lbl[MENU_ROWS];
static int       s_menu_sel;
static int       s_menu_r_sel = -1;
static int       s_menu_win;
static bool      s_menu_dirty = true;

static const char *menu_name(int i)
{
    /* 「模式」和「来源」两行的文字是动态的，文本放在这两个静态缓冲里 */
    static char s_menu_mode_lbl[48];
    static char s_menu_back_lbl[48];
    switch (i) {
        /* ★ 两个概念分开，就不打架了：
         *     来源 = 音乐从哪个**服务**来（Navidrome / QQ 音乐）
         *     模式 = 这个服务下的哪一类**曲目**（最近播放… / 每日30首…）
         *   原来只有"来源"一项，切的是后者 —— 名不副实，所以改名。 */
        case 0:
            snprintf(s_menu_mode_lbl, sizeof(s_menu_mode_lbl), "模式: %s",
                     player_mode_name(player_mode()));
            return s_menu_mode_lbl;
        case 1:
            snprintf(s_menu_back_lbl, sizeof(s_menu_back_lbl), "来源: %s",
                     player_back_name(player_back()));
            return s_menu_back_lbl;
        case 2: return "搜索";
        case 3: return "屏幕亮度";
        /* ★ 2026-10-01：这里原来是"重连 WiFi"，用户要求换成"设置"，
         *   重连挪进设置二级菜单（并且在那边加了二次确认）。 */
        case 4: return "设置";
        case 5: return "系统信息";
        default: return "";
    }
}

static void menu_render(void)
{
    /* 「模式」和「来源」两行都是动态的。任一变就把菜单标脏，否则要等到用户翻页才刷新
     * —— 用户会以为"换了来源但屏幕上没反应"。 */
    {
        static char prev[128];
        char now[128];
        snprintf(now, sizeof(now), "%s|%s",
                 player_back_name(player_back()), player_mode_name(player_mode()));
        if (strcmp(now, prev) != 0) {
            strlcpy(prev, now, sizeof(prev));
            s_menu_dirty = true;
        }
    }

    /* 窗口跟随选中项（菜单 8 项、屏上 6 行，滚到最后一项时窗口下移一行） */
    int top = s_menu_win;
    if (s_menu_sel < top) top = s_menu_sel;
    if (s_menu_sel >= top + MENU_ROWS) top = s_menu_sel - MENU_ROWS + 1;
    if (top > MENU_N - MENU_ROWS) top = MENU_N - MENU_ROWS;
    if (top < 0) top = 0;

    if (s_menu_dirty || top != s_menu_win || s_menu_r_sel < 0) {
        s_menu_win   = top;
        s_menu_dirty = false;
        for (int i = 0; i < MENU_ROWS; i++) {
            ui_set_text(s_menu_lbl[i], menu_name(s_menu_win + i));
        }
    }

    /* 高亮只在真的移位时才动（不然每 250ms 都 invalidate 一次）。
     * 用 120ms 滑过去，比瞬间跳行看起来连贯。 */
    int y = MENU_Y0 + (s_menu_sel - s_menu_win) * MENU_ROW_H;
    if (s_menu_r_sel < 0 || lv_obj_get_y(s_menu_hl) != y) {
        ui_anim_slide_obj(s_menu_hl, y, 120);
    }
    s_menu_r_sel = s_menu_sel;
}

/* 音量页是从哪一页进来的（K4 靠它回"上一级"）。以前是"一律回播放页"，
 * 于是从菜单的"屏幕亮度"进来、按 K4 会跳到播放页而不是回菜单。
 * ⚠️ 声明必须在 ui_page_parent() 之前 —— 那张表要用它。 */
static ui_page_t s_vol_back = UI_PAGE_PLAY;

/* ============================================================ 页面层级（K4 用）
 *
 * 用户要求把 K4 统一成：**短按回上一级、长按回播放器**。
 * 集中一张表比每个页面各写一遍强 —— 加页面时只改这里，
 * 不会出现"某个页面的 K4 忘了改"。
 * ⚠️ 播放页故意不在这张表里：它是最外层，没有上一级，
 *    短按仍然是"换下一个来源"、长按进菜单（用户确认保持现状）。
 */
ui_page_t ui_page_parent(ui_page_t p)
{
    switch (p) {
        case UI_PAGE_MENU:        return UI_PAGE_PLAY;
        case UI_PAGE_SRC:         return UI_PAGE_MENU;
        case UI_PAGE_BACK:        return UI_PAGE_MENU;   /* 来源二级菜单的上一级也是主菜单 */
        case UI_PAGE_SETTINGS:    return UI_PAGE_MENU;   /* 设置页的上一级 = 菜单 */
        case UI_PAGE_QQLOGIN:     return UI_PAGE_SETTINGS;  /* QQ 登录的上一级 = 设置 */
        case UI_PAGE_SEARCH_MODE: return UI_PAGE_MENU;
        case UI_PAGE_SEARCH_IN:   return UI_PAGE_SEARCH_MODE;   /* 用户要的：回输入页要先过方式选择 */
        case UI_PAGE_SEARCH_RES:  return UI_PAGE_SEARCH_IN;     /* 用户要的：方便改词重搜 */
        case UI_PAGE_LIST:        return UI_PAGE_PLAY;
        case UI_PAGE_INFO:        return UI_PAGE_MENU;
        case UI_PAGE_MEM:         return UI_PAGE_INFO;   /* 内存图表页的上一级 = 系统信息 */
        case UI_PAGE_VOL:         return s_vol_back;
        case UI_PAGE_BT:          return UI_PAGE_PLAY;          /* 遥控页的"上一级"= 退出模式 */
        default:                  return UI_PAGE_PLAY;
    }
}

/* ============================================================ 「模式」二级菜单
 *
 * 2026-10-11 改名 + 改成动态：
 *   · 这一页原来叫"来源"，但它切的是"最近播放 / 我的收藏 / 随机30首" —— 那是**模式**。
 *     真正的"来源"（Navidrome / QQ 音乐）是主菜单新加的那一行。
 *   · 项数和文字现在**随后端动态变**（QQ 6 个 / Navidrome 3 个），
 *     所以不再写死项数，一律用 player_mode_count()/player_mode_name() 现取。
 *     → 以后服务端加模式（改 qqmusic 的 k_modes 表）时，**UI 一行都不用改**。
 *
 * 用户要求：主菜单里"模式"只占一项（冒号后面显示当前模式），
 * 按 K3 进这一页，K1/K2 选、K3 确定并退出、K4 返回。
 * 底图/高亮条直接复用菜单页的（同一套视觉，不额外做素材）。
 */
#define MODE_MAX  MENU_ROWS          /* 一屏就 6 行 —— QQ 的模式数正好也是 6 */

static lv_obj_t *s_scr_src, *s_src_hl, *s_src_lbl[MODE_MAX];
static int       s_src_sel;
static int       s_src_r_sel = -1;

static void src_render(void)
{
    int n = player_mode_count();
    if (n > MODE_MAX) {
        /* 一屏放不下就先截断（当前两边都不会触发：QQ 6 个、Navidrome 3 个）。
         * 真要更多模式，这页得加滚动 —— 但别默默藏掉，至少日志里说一声。 */
        ESP_LOGW(TAG, "模式有 %d 个，一屏只显示 %d 个", n, MODE_MAX);
        n = MODE_MAX;
    }
    for (int i = 0; i < MODE_MAX; i++) {
        if (i >= n) { ui_set_text(s_src_lbl[i], ""); continue; }   /* 用不到的行清空 */
        char buf[48];
        bool cur = (player_mode() == i);
        /* "（空）"只对 Navidrome 的"最近播放"有意义：一首歌都没听过时提醒一下，
         * 不然用户会奇怪"选了怎么没反应"。QQ 的模式都由服务端给，不存在空。 */
        bool empty = (player_back() == PLAYER_BACK_NAV && i == PLAYER_SRC_RECENT
                      && player_recent_count() == 0);
        snprintf(buf, sizeof(buf), "%s%s%s", player_mode_name(i),
                 cur ? "（当前）" : "", empty ? "（空）" : "");
        ui_set_text(s_src_lbl[i], buf);
    }
    int y = MENU_Y0 + s_src_sel * MENU_ROW_H;
    if (s_src_r_sel < 0 || lv_obj_get_y(s_src_hl) != y) {
        ui_anim_slide_obj(s_src_hl, y, 120);
    }
    s_src_r_sel = s_src_sel;
}

static void build_src_screen(void)
{
    lv_obj_t *scr = ui_new_screen();
    s_scr_src = scr;
    ui_page_register(UI_PAGE_SRC, scr);

    ui_put_img(scr, ASSET_BG_MENU, 0, 0);
    ui_put_img(scr, ASSET_IC_NOTE, 4, 5);
    ui_put_label_1line(scr, "模式", 20, 2, 134, ui_font_16, 0x1C1C1E);

    s_src_hl = ui_put_img(scr, ASSET_HL_MENU, 0, MENU_Y0);
    for (int i = 0; i < MODE_MAX; i++) {
        s_src_lbl[i] = ui_put_label_1line(scr, "", 12, MENU_Y0 + i * MENU_ROW_H + 1,
                                         136, ui_font_12, 0x1C1C1E);
    }
    ESP_LOGI(TAG, "模式二级菜单已建好（最多 %d 项：K1/K2 选、K3 确定、K4 返回）", MODE_MAX);
}

void ui_srcmenu_open(void)
{
    /* 光标默认停在【当前模式】上，用户一进来就知道自己在哪 */
    s_src_sel = player_mode();
    if (s_src_sel < 0 || s_src_sel >= player_mode_count()) s_src_sel = 0;
    s_src_r_sel = -1;
    ui_page_show(UI_PAGE_SRC);
}

void ui_srcmenu_move(int delta)
{
    int n = player_mode_count();
    if (n <= 0) return;
    s_src_sel = (s_src_sel + delta + n) % n;
}

void ui_srcmenu_activate(void)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "正在获取: %.16s", player_mode_name(s_src_sel));
    ui_toast(buf);
    player_set_mode(s_src_sel, true);     /* 立刻播第一首 */
    s_menu_dirty = true;                  /* 主菜单那行"模式: X"要跟着变 */
    ui_page_show(UI_PAGE_MENU);           /* 确定并退出（回上级菜单） */
}

/* ============================================================ 「来源」二级菜单（切后端）
 *
 * 2026-10-11 新增。只有两项：Navidrome / QQ 音乐。
 * ⚠️ 切过去**不一定成功**（Navidrome 没配、QQ 服务连不上都会失败）——
 *    player 那边会回退状态、队列不动，所以提示语用"正在切换"而不是"已切换到"，
 *    别让用户在失败时以为切成功了。
 */
#define BACK_N  ((int)PLAYER_BACK_COUNT)

static lv_obj_t *s_scr_back, *s_back_hl, *s_back_lbl[BACK_N];
static int       s_back_sel;
static int       s_back_r_sel = -1;

static void back_render(void)
{
    for (int i = 0; i < BACK_N; i++) {
        char buf[40];
        snprintf(buf, sizeof(buf), "%s%s", player_back_name((player_back_t)i),
                 (player_back() == (player_back_t)i) ? "（当前）" : "");
        ui_set_text(s_back_lbl[i], buf);
    }
    int y = MENU_Y0 + s_back_sel * MENU_ROW_H;
    if (s_back_r_sel < 0 || lv_obj_get_y(s_back_hl) != y) {
        ui_anim_slide_obj(s_back_hl, y, 120);
    }
    s_back_r_sel = s_back_sel;
}

static void build_back_screen(void)
{
    lv_obj_t *scr = ui_new_screen();
    s_scr_back = scr;
    ui_page_register(UI_PAGE_BACK, scr);

    ui_put_img(scr, ASSET_BG_MENU, 0, 0);
    ui_put_img(scr, ASSET_IC_NOTE, 4, 5);
    ui_put_label_1line(scr, "来源", 20, 2, 134, ui_font_16, 0x1C1C1E);

    s_back_hl = ui_put_img(scr, ASSET_HL_MENU, 0, MENU_Y0);
    for (int i = 0; i < BACK_N; i++) {
        s_back_lbl[i] = ui_put_label_1line(scr, "", 12, MENU_Y0 + i * MENU_ROW_H + 1,
                                           136, ui_font_12, 0x1C1C1E);
    }
    ESP_LOGI(TAG, "来源二级菜单已建好（%d 项）", BACK_N);
}

void ui_backmenu_open(void)
{
    s_back_sel = (int)player_back();
    if (s_back_sel < 0 || s_back_sel >= BACK_N) s_back_sel = 0;
    s_back_r_sel = -1;
    ui_page_show(UI_PAGE_BACK);
}

void ui_backmenu_move(int delta)
{
    s_back_sel = (s_back_sel + delta + BACK_N) % BACK_N;
}

void ui_backmenu_activate(void)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "正在切换: %.16s", player_back_name((player_back_t)s_back_sel));
    ui_toast(buf);
    player_set_back((player_back_t)s_back_sel, true);   /* 换了后端就重建队列并起播 */
    s_menu_dirty = true;      /* 主菜单那两行（来源/模式）都要跟着变 */
    ui_page_show(UI_PAGE_MENU);
}

/* ============================================================ 设置二级菜单
 *
 * 用户要求：主菜单原来的"重连 WiFi"换成"设置"，按 K3 进这一页。
 * （2026-10-01 再简化：删掉"开机续播"——用户不要断点续播了，所以这一页只剩重连 WiFi。）
 *
 * 重连为什么要二次确认：重连会把当前连接掐掉、正在放的歌会中断。原来它直接摆在
 * 主菜单里按一下就执行，往上翻菜单很容易误触。现在第一次按 K3 只是把这一行变成
 * "重连 WiFi？再按K3确认"，再按一次才真的动网络，K4 随时退出。
 *
 * 2026-10-14 新增第 3 项「QQ 登录」：用户要求"在设置里能看到当前 QQ 契约层服务的
 * 登录情况，按 K3 扫码登录，确认后回到显示登录情况"。这一行的**后缀就是登录结论**
 * （有效 / 已过期 / 未登录…），按 K3 进 UI_PAGE_QQLOGIN 看详情并扫码。
 * 为什么这一项不做二次确认：扫码只是"看"和"换一张码"，没有任何破坏性，
 * 多一步确认只会让人以为按错了。
 */
#define SET_N  3

static lv_obj_t *s_scr_set, *s_set_hl, *s_set_lbl[SET_N];
static int       s_set_sel;
static int       s_set_r_sel = -1;
static bool      s_set_confirm;      /* true = 正停在某一项的确认态 */
/* 「QQ 登录」那一行的状态后缀。后台任务查到结果要几秒，所以靠 set_tick 定期取；
 * 只在文字真的变了才重绘（否则每 250ms 平白重画一行）。 */
static char      s_set_qq[16];

static void set_render(void)
{
    /* 确认态下把提示直接写在行里 —— 这个屏幕上没有独立的弹窗层，
     * 复用同一行最省事，也不会盖住别的东西。 */
    char buf[48];
    snprintf(buf, sizeof(buf), "重连 WiFi%s",
             (s_set_confirm && s_set_sel == 0) ? "？再按K3确认" : "");
    ui_set_text(s_set_lbl[0], buf);
    /* 清空缓存：清掉封面缓存（可再生）。加在设置里是因为它属于"偶尔要清一下"
     * 的维护动作，不配占用主菜单一行。 */
    snprintf(buf, sizeof(buf), "清空缓存%s",
             (s_set_confirm && s_set_sel == 1) ? "？再按K3确认" : "");
    ui_set_text(s_set_lbl[1], buf);

    /* QQ 登录：后缀 = 登录结论（后台查到什么就显示什么，见 set_qq_short） */
    if (s_set_qq[0]) snprintf(buf, sizeof(buf), "QQ 登录  %s", s_set_qq);
    else             snprintf(buf, sizeof(buf), "QQ 登录");
    ui_set_text(s_set_lbl[2], buf);

    int y = MENU_Y0 + s_set_sel * MENU_ROW_H;
    if (s_set_r_sel < 0 || lv_obj_get_y(s_set_hl) != y) {
        ui_anim_slide_obj(s_set_hl, y, 120);
    }
    s_set_r_sel = s_set_sel;
}

static void build_set_screen(void)
{
    lv_obj_t *scr = ui_new_screen();
    s_scr_set = scr;
    ui_page_register(UI_PAGE_SETTINGS, scr);

    ui_put_img(scr, ASSET_BG_MENU, 0, 0);
    ui_put_img(scr, ASSET_IC_NOTE, 4, 5);
    ui_put_label_1line(scr, "设置", 20, 2, 134, ui_font_16, 0x1C1C1E);

    s_set_hl = ui_put_img(scr, ASSET_HL_MENU, 0, MENU_Y0);
    for (int i = 0; i < SET_N; i++) {
        s_set_lbl[i] = ui_put_label_1line(scr, "", 12, MENU_Y0 + i * MENU_ROW_H + 1,
                                          136, ui_font_12, 0x1C1C1E);
    }
    ESP_LOGI(TAG, "设置二级菜单已建好（%d 项）", SET_N);
}

void ui_setmenu_open(void)
{
    s_set_sel     = 0;
    s_set_r_sel   = -1;
    s_set_confirm = false;      /* 每次进来都从"没在确认"开始 */
    set_render();
    ui_page_show(UI_PAGE_SETTINGS);
}

void ui_setmenu_move(int delta)
{
    if (s_set_confirm) return;  /* 确认态下锁住移动：手一抖移开就取消了，反而更迷惑 */
    s_set_sel = (s_set_sel + delta + SET_N) % SET_N;
    set_render();
}

void ui_setmenu_activate(void)
{
    /* 两段式：第一次按 K3 只是"进入确认态"，再按一次才真的执行。
     * 这两项都有副作用（断网 / 删文件），不该一按就中。
     * ⚠️ 第 3 项「QQ 登录」**不进确认态** —— 它只是打开一页，没有副作用，
     *    多一步确认只会让人以为按错了。 */
    if (s_set_sel == 2) {
        ui_qqlogin_open();
        return;
    }

    if (!s_set_confirm) {
        s_set_confirm = true;
        ui_toast(s_set_sel == 0 ? "再按 K3 确认重连" : "再按 K3 确认清空");
        set_render();
        return;
    }
    s_set_confirm = false;

    if (s_set_sel == 0) {
        net_mgr_reconnect();
        ui_toast("正在重连 WiFi...");
    } else {
        /* ★ 只清**封面缓存**：它是纯粹的可再生数据（下次看到那首歌会自动重新下载）。
         *   最近播放 / 收藏镜像**不清** —— 那里面有用户的东西，清掉会莫名其妙。 */
        uint64_t before = store_free_kb();
        int n = store_evict("COVER", 0);        /* keep=0 = 全删 */
        uint64_t after = store_free_kb();
        char t[48];
        snprintf(t, sizeof(t), "清了 %d 个封面，释放 %u KB", n, (unsigned)(after - before));
        ui_toast(t);
        ESP_LOGW(TAG, "清空封面缓存：删 %d 个，%u KB → %u KB 可用",
                 n, (unsigned)before, (unsigned)after);
        /* 内存里还留着"这些封面取不到"的黑名单 —— 不清掉的话，
         * 清完缓存那些歌**依然不会重新去取**，等于白清。 */
        ui_cover_cache_cleared();
    }
    ui_page_show(UI_PAGE_MENU);     /* 执行完退出 */
}

/* 「QQ 登录」那一行的短后缀。把 qqlogin 给的长结论压成两三个字 ——
 * 12px 字体下一行只放得下 11 个字符，"QQ 登录  " 已经占 7 个。 */
static const char *qq_short(const char *state)
{
    if (strcmp(state, "凭据有效") == 0) return "有效";
    if (strcmp(state, "凭据已过期") == 0) return "已过期";
    if (strcmp(state, "未登录") == 0) return "未登录";
    if (strcmp(state, "凭据无效") == 0) return "无效";
    if (strcmp(state, "连不上服务") == 0) return "连不上";
    if (strcmp(state, "没配服务地址") == 0) return "没配置";
    if (strcmp(state, "等待扫码") == 0) return "等扫码";
    if (strcmp(state, "正在获取二维码...") == 0) return "取码中";
    if (strcmp(state, "已扫码，请在手机上确认") == 0) return "待确认";
    return "";
}

/* 设置页的周期刷新（ui_pages_tick 调）。
 * 唯一的作用：登录情况是**后台任务**查出来的，几秒后才回来 ——
 * 不在这里定期对一眼，"进设置页看登录情况"就永远只看到进去那一瞬间的值。 */
static void set_tick(void)
{
    qqlogin_view_t v;
    qqlogin_view(&v);
    const char *s = qq_short(v.state);
    /* 只在文字变了才重绘：否则每 250ms 就把这一行重画一次，白占屏幕带宽 */
    if (strcmp(s, s_set_qq) != 0) {
        strlcpy(s_set_qq, s, sizeof(s_set_qq));
        set_render();
    }
}


/* ============================================================ QQ 登录页
 *
 * 用户要求（2026-10-14）：设置二级菜单里能看到 QQ 契约层服务的**登录情况**，
 * 按 K3 取二维码扫码登录，确认登录后**回到显示登录情况**。
 *
 * 两个视图（同一张屏，靠显隐切换，运行时不建/删控件）：
 *   ① 登录情况：六行文字（状态 / 账号 / 服务地址 / 说明 / 键位）
 *   ② 二维码  ：一整张点阵 + 下面一行状态
 *
 * ★ 二维码为什么要"自己放大画"，而不是把 PNG 交给 LVGL 显示：
 *   服务端那张图是 150×150、29 个模块、**四周一点静区都没有**，而屏幕只有
 *   160×128。缩放显示（150/29 不是整数）会把模块边界糊掉，手机直接扫不动；
 *   留白不够同样扫不动（实测：原图直接喂 OpenCV 是解不出来的，放大 + 补静区才行）。
 *   所以走 qrpng 解出**模块点阵**，这里按**整数倍**重画，白底自己铺 —— 白底就是静区。
 *
 * ★ 性能：整屏重绘一次 40KB，绝不能每 250ms 重画一遍二维码。
 *   所以**只有 phase 真的变了才重画**（见 qq_render），静态那一帧不产生任何重绘。
 */
#define QQ_ROWS       6
#define QQ_ROW_H      15
#define QQ_Y0         20

/* 二维码视图：标题藏起来，整块让给码。上面 112px 放码，下面一行状态。 */
#define QQ_QR_AREA_H  112
#define QQ_QR_MAXSIDE 112
#define QQ_STAT_Y     114

static lv_obj_t *s_scr_qq;
static lv_obj_t *s_qq_title;
static lv_obj_t *s_qq_lbl[QQ_ROWS];
static lv_obj_t *s_qq_stat;        /* 二维码视图下面那行状态 */
static lv_obj_t *s_qq_img;         /* 二维码（RGB565，自己画的） */

static uint8_t       *s_qq_buf;    /* 画布缓冲（PSRAM） */
static lv_image_dsc_t s_qq_dsc;
static qqlogin_view_t s_qq_v;      /* UI 自己的快照 */
static qqlogin_phase_t s_qq_painted = (qqlogin_phase_t)-1;  /* 上一次画的是哪个 phase */
static bool           s_qq_qr_view;                          /* 当前屏上是不是二维码视图 */

/* 服务地址去掉 http:// 前缀 —— 12px 字体下 19 个字符正好放得下，多 7 个就溢出 */
static const char *qq_host_short(void)
{
    const char *h = qqmusic_host();
    if (strncmp(h, "http://", 7) == 0) h += 7;
    return h;
}

static void qq_paint_qr(const qrpng_t *qr)
{
    if (s_qq_buf == NULL || qr == NULL || qr->n <= 0) return;
    int n = qr->n;

    /* 整数倍放大；2 倍是底线（再小手机真扫不动），8 倍是上限（29×8 早就超屏了） */
    int scale = QQ_QR_AREA_H / n;
    if (scale < 2) scale = 2;
    if (scale > 8) scale = 8;
    while (n * scale > QQ_QR_MAXSIDE && scale > 1) scale--;
    int side = n * scale;

    uint16_t *px = (uint16_t *)s_qq_buf;
    /* 白底：这张码自己没有静区，这层白就是静区，不能省 */
    memset(s_qq_buf, 0xFF, (size_t)side * (size_t)side * 2);
    for (int j = 0; j < n; j++) {
        for (int i = 0; i < n; i++) {
            if (!qrpng_dark(qr, i, j)) continue;
            int x0 = i * scale, y0 = j * scale;
            for (int dy = 0; dy < scale; dy++) {
                uint16_t *row = px + (size_t)(y0 + dy) * (size_t)side + x0;
                for (int dx = 0; dx < scale; dx++) row[dx] = 0x0000;
            }
        }
    }

    memset(&s_qq_dsc, 0, sizeof(s_qq_dsc));
    /* ⚠️ 这里写的是**本机字节序**的 RGB565，也就是屏幕要的那个。
     *    别用 RGB565_SWAPPED —— 那是给"解码器吐大端"的 JPEG 封面用的。 */
    s_qq_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_qq_dsc.header.w  = side;
    s_qq_dsc.header.h  = side;
    s_qq_dsc.data_size = (uint32_t)side * side * 2;
    s_qq_dsc.data      = s_qq_buf;

    lv_image_set_src(s_qq_img, &s_qq_dsc);
    lv_obj_set_size(s_qq_img, side, side);
    lv_obj_set_pos(s_qq_img, (160 - side) / 2, (QQ_QR_AREA_H - side) / 2);
    /* ⚠️ src 指针没变（还是同一个 dsc），LVGL 会当成"没动过"而不重绘 ——
     *    所以换了码之后必须手动 invalidate，否则屏上还是上一张码。 */
    lv_obj_invalidate(s_qq_img);
    /* ★ 带上任务名：LVGL 不是线程安全的，而这一页的渲染**可能**被按键回调
     *   （esp_timer 任务）触发。崩溃排查时"死在哪个任务里"是关键线索。 */
    ESP_LOGI(TAG, "[%s] 二维码已上屏：%d 模块 x %d 倍 = %dx%d @(%d,%d)",
             pcTaskGetName(NULL), n, scale, side, side,
             (160 - side) / 2, (QQ_QR_AREA_H - side) / 2);
}

static void qq_render(void)
{
    qqlogin_view(&s_qq_v);

    /* "刚登录成功"只提示一次（后台会连着刷状态，不 ack 会弹个不停） */
    if (s_qq_v.fresh_login) {
        ui_toast("登录成功");
        qqlogin_ack();
    }

    bool qr_mode = s_qq_v.qr_ready &&
                   (s_qq_v.phase == QQLOGIN_QR_WAIT || s_qq_v.phase == QQLOGIN_QR_SCANNED);

    /* ★ 相位一变就记一行（带任务名）—— 崩溃现场就靠这个看"死在哪个任务里、
     *   死在哪个阶段"。平时不刷屏（只在真的变化时打）。 */
    {
        static int s_qq_last_phase = -2;
        if ((int)s_qq_v.phase != s_qq_last_phase) {
            s_qq_last_phase = (int)s_qq_v.phase;
            ESP_LOGW(TAG, "[%s] QQ登录相位 -> %d（qr_ready=%d qr_mode=%d）",
                     pcTaskGetName(NULL), (int)s_qq_v.phase,
                     (int)s_qq_v.qr_ready, (int)qr_mode);
        }
    }

    /* ---- 两个视图的显隐只在真的切换时动一次 ---- */
    if (qr_mode != s_qq_qr_view) {
        s_qq_qr_view = qr_mode;
        if (qr_mode) {
            lv_obj_add_flag(s_qq_title, LV_OBJ_FLAG_HIDDEN);   /* 标题让位给二维码的静区 */
            for (int i = 0; i < QQ_ROWS; i++) lv_obj_add_flag(s_qq_lbl[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_qq_img, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_qq_stat, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s_qq_title, LV_OBJ_FLAG_HIDDEN);
            for (int i = 0; i < QQ_ROWS; i++) lv_obj_remove_flag(s_qq_lbl[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_qq_img, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_qq_stat, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (qr_mode) {
        /* ★ 只有换了一张新码（phase 从 BUSY 进到 WAIT）才重画点阵 ——
         *   WAIT→SCANNED 是同一张码，只换下面那行字，重画纯属浪费。
         *   （29 个模块 × 4 倍 = 116×116 ≈ 13KB 全屏重绘，250ms 一次就废了。） */
        if (s_qq_painted != s_qq_v.phase) {
            if (s_qq_v.phase == QQLOGIN_QR_WAIT) qq_paint_qr(&s_qq_v.qr);
            s_qq_painted = s_qq_v.phase;
        }
        ui_set_text(s_qq_stat, s_qq_v.phase == QQLOGIN_QR_SCANNED
                                   ? "已扫码，请在手机上确认"
                                   : "用手机 QQ 音乐 App 扫码");
        return;
    }

    s_qq_painted = s_qq_v.phase;
    char buf[64];
    snprintf(buf, sizeof(buf), "状态  %s", s_qq_v.state);
    ui_set_text(s_qq_lbl[0], buf);
    if (s_qq_v.account[0]) snprintf(buf, sizeof(buf), "账号  %s", s_qq_v.account);
    else                   strlcpy(buf, "账号  无", sizeof(buf));
    ui_set_text(s_qq_lbl[1], buf);
    ui_set_text(s_qq_lbl[2], "服务地址");
    ui_set_text(s_qq_lbl[3], qq_host_short());
    ui_set_text(s_qq_lbl[4], s_qq_v.reason);
    ui_set_text(s_qq_lbl[5],
                (s_qq_v.phase == QQLOGIN_QR_BUSY) ? "正在取二维码..." : "按K3扫码登录   K4返回");
}

static void build_qq_screen(void)
{
    lv_obj_t *scr = ui_new_screen();
    s_scr_qq = scr;
    ui_page_register(UI_PAGE_QQLOGIN, scr);

    ui_put_img(scr, ASSET_BG_INFO, 0, 0);      /* 和系统信息/蓝牙页同一套底图 */
    ui_put_img(scr, ASSET_IC_NOTE, 4, 5);
    s_qq_title = ui_put_label_1line(scr, "QQ 登录", 20, 2, 134, ui_font_16, 0x1C1C1E);

    for (int i = 0; i < QQ_ROWS; i++) {
        s_qq_lbl[i] = ui_put_label_1line(scr, "", 6, QQ_Y0 + i * QQ_ROW_H, 150,
                                         ui_font_12, 0x3C3C40);
    }

    /* 二维码画布：RGB565、PSRAM。只按"最大可能边长"开一次，之后改 dsc 的 w/h。 */
    s_qq_buf = heap_caps_malloc((size_t)QQ_QR_MAXSIDE * QQ_QR_MAXSIDE * 2,
                                MALLOC_CAP_SPIRAM);
    if (s_qq_buf == NULL) {
        ESP_LOGE(TAG, "二维码画布分配失败（%u 字节）—— 扫码页会画不出码",
                 (unsigned)(QQ_QR_MAXSIDE * QQ_QR_MAXSIDE * 2));
    }
    s_qq_img = lv_image_create(scr);
    lv_obj_remove_flag(s_qq_img, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_qq_img, LV_OBJ_FLAG_HIDDEN);

    s_qq_stat = ui_put_label_1line(scr, "", 0, QQ_STAT_Y, 160, ui_font_12, 0x1C1C1E);
    lv_obj_set_style_text_align(s_qq_stat, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(s_qq_stat, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(TAG, "QQ 登录页已建好（画布 %u B，%s）",
             (unsigned)(QQ_QR_MAXSIDE * QQ_QR_MAXSIDE * 2),
             s_qq_buf ? "PSRAM 就绪" : "!!分配失败!!");
}

void ui_qqlogin_open(void)
{
    qqlogin_enter();      /* 异步查一次登录情况（结果几秒后由 tick 刷上来） */
    qq_render();          /* 先把缓存里的结论填上，别进去空一下 */
    ui_page_show(UI_PAGE_QQLOGIN);
}

void ui_qqlogin_activate(void)
{
    qqlogin_request_qr();
    qq_render();
}

/* ============================================================ 系统信息整页 —— 一行 toast 放不下这么多事实（IP + 内存 + 欠载 + …），
 * 用户反馈"根本显示不全"。7 行 × 15px，对应 bg_info 的底图。 */
#define INFO_ROWS  7
#define INFO_ROW_H 15
#define INFO_Y0    20

static lv_obj_t *s_scr_info;
static lv_obj_t *s_info_lbl[INFO_ROWS];

/* 返回值写进 buf；返回是否"有内容"。空行传 "" */
static void info_line(int i, char *buf, size_t len)
{
    player_status_t st;
    player_status_get(&st);
    uint32_t up_s = (uint32_t)(esp_timer_get_time() / 1000000);
    switch (i) {
        /* ★ 用户要求（2026-09-27）：没连上网的时候，这两行要变成"怎么配网"的说明书 ——
         *   第 0 行给热点的 IP（浏览器里输这个就能进控制台），第 1 行给热点名。
         *   以前没网时第 0 行是空的（本机 IP 没有），用户连进去了也不知道该访问哪。 */
        case 0:
            if (net_mgr_is_connected())
                snprintf(buf, len, "本机 IP  %.16s", net_mgr_ip_str());   /* ★ 用户明确要求过 */
            else
                snprintf(buf, len, "配网 IP  %.16s", net_mgr_ap_ip_str());
            break;
        case 1:
            if (net_mgr_is_connected())
                snprintf(buf, len, "WiFi  %.10s  %d dBm", net_mgr_ssid(), net_mgr_rssi());
            else
                snprintf(buf, len, "热点  %.12s", net_mgr_ap_ssid());
            break;
        case 2:
            snprintf(buf, len, "内存  %uK / PSRAM %uK",
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
            break;
        case 3:
            snprintf(buf, len, "音频  %ukHz  欠载 %d  I2C %d",
                     (unsigned)(audio_engine_sample_rate() / 1000),
                     audio_engine_underruns(), board_i2c_retries());
            break;
        case 4:
            snprintf(buf, len, "音量  %d%%  %s  亮度 %d%%",
                     player_volume_get(), player_is_muted() ? "静音" : "正常",
                     player_brightness());
            break;
        case 5:
            /* 同样要用"模式名"（QQ 后端下 player_src_name 会给出不相干的名字） */
            snprintf(buf, len, "曲目  %d/%d  %.16s", st.queue_index + 1, st.queue_count,
                     player_mode_name(player_mode()));
            break;
        case 6:
            snprintf(buf, len, "运行  %u时%u分  设置待存 %d",
                     (unsigned)(up_s / 3600), (unsigned)((up_s / 60) % 60),
                     settings_pending_count());
            break;
        default:
            buf[0] = 0;
            break;
    }
}

/* 给按键自检（app_main 的 K4_SELFTEST）用：把这一页七行原样打进串口。
 * 我看不见屏幕，"离线时第 0/1 行到底显示成什么"只能靠它验证。 */
void ui_info_debug_dump(void)
{
    char buf[64];
    for (int i = 0; i < INFO_ROWS; i++) {
        info_line(i, buf, sizeof(buf));
        ESP_LOGW(TAG, "[info %d] %s", i, buf);
    }
}

static void info_render(void)
{
    char buf[64];
    for (int i = 0; i < INFO_ROWS; i++) {
        info_line(i, buf, sizeof(buf));
        ui_set_text(s_info_lbl[i], buf);      /* 内部先比较，没变就不重绘 */
    }
}

/* ============================================================ 内存用量
 *
 * 用户要求：系统信息页【长按 K3】进来，**像播放页的进度条那样画两条横条** ——
 * 一条 PSRAM、一条 内部 SRAM。
 * （flash 用量基本不变，用户说不需要。）
 *
 * 样式直接照抄播放页那条进度条：槽透明、指示条圆角、颜色区分两块内存。
 * 为什么不画成历史曲线：用户要的就是"两条进度条"这种一眼看懂的形式，
 * 不需要时间轴。
 */
#define MEM_BAR_X   5
#define MEM_BAR_W   150
#define MEM_BAR_H   12

static lv_obj_t *s_scr_mem;
static lv_obj_t *s_mem_lbl_psram, *s_mem_lbl_sram;   /* 两行的文字 */
static lv_obj_t *s_mem_bar_psram, *s_mem_bar_sram;   /* 两条进度条 */

/* 按播放页那条进度条的样式造一条：槽不画（底图上有），只画圆角填充 */
static lv_obj_t *mem_make_bar(lv_obj_t *scr, int y, uint32_t color)
{
    lv_obj_t *b = lv_bar_create(scr);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, MEM_BAR_X, y);
    lv_obj_set_size(b, MEM_BAR_W, MEM_BAR_H);
    lv_bar_set_range(b, 0, 100);
    lv_bar_set_value(b, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, LV_PART_MAIN);          /* 槽透明 */
    lv_obj_set_style_bg_color(b, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    return b;
}

static void mem_render(void)
{
    /* PSRAM：8MB 那块（音频缓冲、LVGL、WiFi 都在这里）*/
    size_t tot = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t fre = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t use = tot - fre;
    unsigned pct = tot ? (unsigned)(use * 100 / tot) : 0;
    char buf[48];
    /* ⚠️ 这行文字要短：12px 字体下 23 个字符就会超出 150px 被截断
     *    （实测 "PSRAM  2995/8192 KB  36%" 显示不全）。
     *    现在用紧凑写法 + 拉丁字体（比中文字体的拉丁字符窄），一行放得下。*/
    snprintf(buf, sizeof(buf), "PSRAM %u/%uK %u%%",
             (unsigned)(use / 1024), (unsigned)(tot / 1024), pct);
    ui_set_text(s_mem_lbl_psram, buf);
    if (lv_bar_get_value(s_mem_bar_psram) != pct) lv_bar_set_value(s_mem_bar_psram, pct, LV_ANIM_OFF);

    /* 内部 SRAM：308KB 那块（整机最紧的资源，蓝牙当初就是被它挤死的）*/
    tot = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    fre = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    use = tot - fre;
    pct = tot ? (unsigned)(use * 100 / tot) : 0;
    snprintf(buf, sizeof(buf), "SRAM %u/%uK %u%%",
             (unsigned)(use / 1024), (unsigned)(tot / 1024), pct);
    ui_set_text(s_mem_lbl_sram, buf);
    if (lv_bar_get_value(s_mem_bar_sram) != pct) lv_bar_set_value(s_mem_bar_sram, pct, LV_ANIM_OFF);
}

static void build_mem_screen(void)
{
    lv_obj_t *scr = ui_new_screen();
    s_scr_mem = scr;
    ui_page_register(UI_PAGE_MEM, scr);

    ui_put_img(scr, ASSET_BG_INFO, 0, 0);      /* 复用系统信息页的底图，省一套素材 */
    ui_put_img(scr, ASSET_IC_NOTE, 4, 5);
    ui_put_label_1line(scr, "内存用量", 20, 2, 134, ui_font_16, 0x1C1C1E);

    /* 两块内存各一行字 + 一条进度条。PSRAM 蓝、SRAM 橙 —— 颜色区分开，
     * 不然两条长得一样，一眼分不清哪条是哪条。 */
    /* 这两行全是 ASCII（PSRAM/SRAM + 数字），所以用拉丁字体 ——
     * 比中文字体的拉丁字符窄一截，长的数字行才放得下（见 mem_render 的注释）。*/
    s_mem_lbl_psram = ui_put_label_1line(scr, "", 5, 26, 152, ui_font_lat, 0x3C3C40);
    s_mem_bar_psram = mem_make_bar(scr, 42, 0x1A73E8);
    s_mem_lbl_sram  = ui_put_label_1line(scr, "", 5, 66, 152, ui_font_lat, 0x3C3C40);
    s_mem_bar_sram  = mem_make_bar(scr, 82, 0xC77700);

    ESP_LOGI(TAG, "内存用量页已建好（PSRAM + 内部 SRAM 两条进度条）");
}

void ui_mem_open(void)
{
    mem_render();            /* 先把两条填上，别进去空一下 */
    ui_page_show(UI_PAGE_MEM);
}

/* ============================================================ 蓝牙控制页（M7）
 *
 * 这是个【独立模式】：进了这一页，按键只发给对端，本机播放器不受影响。
 * 页面上刻意**不显示本机的歌名/封面/进度** —— 用户点名要求的：
 * "对方在放什么我们根本不知道，显示了反而是误导"。
 * 底图直接复用 bg_info（顶栏 + 7 行），省一套素材。
 */
#define BT_ROWS  7
#define BT_ROW_H 15
#define BT_Y0    20

static lv_obj_t *s_scr_bt;
static lv_obj_t *s_bt_lbl[BT_ROWS];

static void bt_line(int i, char *buf, size_t len)
{
    switch (i) {
        case 0: snprintf(buf, len, "K1 下一首   K3 上一首"); break;
        case 1: snprintf(buf, len, "K2 播放 / 暂停"); break;
        case 2: snprintf(buf, len, "长按 K2 调音量"); break;
        case 3: snprintf(buf, len, "K4 回播放器"); break;
        case 4: {
            const char *p = hid_remote_peer_name();
            if (p[0]) snprintf(buf, len, "对端%s", p);
            else      snprintf(buf, len, "对端 未连");
            break;
        }
        case 5:
            snprintf(buf, len, "状态 %s", hid_remote_state_str());
            break;
        case 6:
            snprintf(buf, len, "已发送 %d 次", hid_remote_sent_count());
            break;
        default: buf[0] = 0; break;
    }
}

static void bt_render(void)
{
    char buf[64];
    for (int i = 0; i < BT_ROWS; i++) {
        bt_line(i, buf, sizeof(buf));
        ui_set_text(s_bt_lbl[i], buf);
    }
}

static void build_bt_screen(void)
{
    lv_obj_t *scr = ui_new_screen();
    s_scr_bt = scr;
    ui_page_register(UI_PAGE_BT, scr);

    ui_put_img(scr, ASSET_BG_INFO, 0, 0);
    ui_put_img(scr, ASSET_IC_BT, 4, 5);
    ui_put_label_1line(scr, "蓝牙控制", 20, 2, 134, ui_font_16, 0x1C1C1E);

    for (int i = 0; i < BT_ROWS; i++) {
        s_bt_lbl[i] = ui_put_label_1line(scr, "", 6, BT_Y0 + i * BT_ROW_H, 150,
                                        ui_font_12, 0x3C3C40);
    }
    ESP_LOGI(TAG, "蓝牙控制页已建好（%d 行；进入后按键只发给对端）", BT_ROWS);
}

static void build_info_screen(void)
{
    lv_obj_t *scr = ui_new_screen();
    s_scr_info = scr;
    ui_page_register(UI_PAGE_INFO, scr);

    ui_put_img(scr, ASSET_BG_INFO, 0, 0);
    ui_put_img(scr, ASSET_IC_NOTE, 4, 5);
    ui_put_label_1line(scr, "系统信息", 20, 2, 134, ui_font_16, 0x1C1C1E);

    for (int i = 0; i < INFO_ROWS; i++) {
        s_info_lbl[i] = ui_put_label_1line(scr, "", 6, INFO_Y0 + i * INFO_ROW_H, 150,
                                          ui_font_12, 0x3C3C40);
    }
    ESP_LOGI(TAG, "系统信息页已建好（%d 行）", INFO_ROWS);
}


static void menu_activate(int i)
{
    switch (i) {
        case 0: ui_srcmenu_open(); break;      /* 模式 → 二级菜单（最近播放… / 每日30首…） */
        case 1: ui_backmenu_open(); break;     /* ★ 新增：来源 → 二级菜单（Navidrome / QQ 音乐） */
        case 2: ui_search_open(); break;       /* 搜索（播放/暂停 在播放页有 K2）*/
        case 3: ui_vol_open(true, false); break;   /* 记住来路=菜单，K4 会回菜单 */
        case 4: ui_setmenu_open(); break;      /* 设置 → 二级菜单 */
        case 5: ui_page_show(UI_PAGE_INFO); break;
        default: break;
    }
}

void ui_menu_activate(void) { menu_activate(s_menu_sel); }

void ui_menu_move(int delta)
{
    int sel = s_menu_sel + delta;
    if (sel < 0) sel = 0;
    if (sel > MENU_N - 1) sel = MENU_N - 1;
    s_menu_sel = sel;
}

static void build_menu_screen(void)
{
    lv_obj_t *scr = ui_new_screen();
    s_scr_menu = scr;
    ui_page_register(UI_PAGE_MENU, scr);

    ui_put_img(scr, ASSET_BG_MENU, 0, 0);
    ui_put_img(scr, ASSET_IC_NOTE, 4, 5);
    ui_put_label_1line(scr, "菜单", 20, 2, 134, ui_font_16, 0x1C1C1E);

    s_menu_hl = ui_put_img(scr, ASSET_HL_MENU, 0, MENU_Y0);

    for (int i = 0; i < MENU_ROWS; i++) {
        /* 菜单行只有 17px 高，必须用 12px 字（16px 中文字的行高约 19，会顶到下一行） */
        s_menu_lbl[i] = ui_put_label_1line(scr, "", 12, MENU_Y0 + i * MENU_ROW_H + 1,
                                          136, ui_font_12, 0x1C1C1E);
    }
    /* 窗口滚到最后一项时上面会空一行，用底图的分隔线凑合看（不额外画东西） */
    ESP_LOGI(TAG, "菜单页已建好（%d 项，窗口 %d 行滚动）", MENU_N, MENU_ROWS);
}

/* ============================================================ 音量 / 亮度页 */

static lv_obj_t *s_scr_vol, *s_vol_top, *s_vol_num, *s_vol_hint, *s_vol_bar;
/* 蓝牙变体：加减/静音都发给【对方】。用户的手机音量我们读不到，
 * 所以这一页不显示数字和进度条 —— 摆一个假的值比不摆更误导。 */
static bool s_vol_remote;
/* 遥控音量页"上一次干了什么"：0=还没按过, '+'=加, '-'=减, 'm'=静音。
 * 对方的音量我们【读不到】，所以这一页不显示任何数值 —— 只给个示意（"+ -"），
 * 右上角显示刚才那个键发出去了没（按键反馈）。摆一个假的数字比不摆更误导。 */
static char s_vol_remote_last;

static void vol_render(bool force)
{
    if (s_vol_remote) {
        ui_set_text(s_vol_top, "远端音量");
        ui_set_text(s_vol_num, "+ -");            /* 示意：这是个加减音量的地方 */
        char hint[16];
        switch (s_vol_remote_last) {
            case '+': snprintf(hint, sizeof(hint), "已发 ＋"); break;
            case '-': snprintf(hint, sizeof(hint), "已发 －"); break;
            case 'm': snprintf(hint, sizeof(hint), "已静音");  break;
            default:  snprintf(hint, sizeof(hint), "发给对方"); break;
        }
        ui_set_text(s_vol_hint, hint);
        /* 进度条藏起来：任何填充位置都在暗示一个具体音量值，那是假的 */
        if (!lv_obj_has_flag(s_vol_bar, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(s_vol_bar, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    if (lv_obj_has_flag(s_vol_bar, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_remove_flag(s_vol_bar, LV_OBJ_FLAG_HIDDEN);
    }

    int v = s_vol_brightness ? player_brightness() : player_volume_get();

    char buf[16];
    snprintf(buf, sizeof(buf), "%d", v);
    ui_set_text(s_vol_num, buf);
    ui_set_text(s_vol_top, s_vol_brightness ? "亮度" : "音量");
    /* 静音提示：音量模式下看的是"硬静音"状态，不是"音量==0" */
    bool muted = !s_vol_brightness && player_is_muted();
    ui_set_text(s_vol_hint, muted ? "静音" : "");

    /* 一次跳 5%，动画顺过去更自然。面积 136×10 ≈ 2.7KB/帧。
     * 时长由 style 的 anim_duration 决定（建控件时设）。 */
    if (lv_bar_get_value(s_vol_bar) != v) lv_bar_set_value(s_vol_bar, v, LV_ANIM_ON);
}

void ui_vol_set_mode(bool brightness)
{
    s_vol_brightness = brightness;
    vol_render(true);
}

/* 离开音量/亮度页时补一次落盘（由 ui_keys.c 的 k4_navigate 调用）。
 *
 * 为什么需要：音量/亮度的改动都走 settings_set_int_lazy（去抖 2000ms），
 * 而本机是**电源开关硬断电**，没有关机流程可以补写 —— 去抖窗口内关机就丢了。
 * 用户报的"屏幕亮度重启后不保留"就是这个。
 *
 * 用户又明确要求保留去抖（防长按连调把 NVS 写爆），所以折中：
 *   **页内照旧去抖，离开这一页时立刻补写一次**。
 * 代价：如果调完亮度**停在那一页直接断电**，仍然会丢 —— 这个场景没法两全，
 * 因为硬断电时没有任何代码有机会跑。 */
void ui_vol_commit(void)
{
    if (s_vol_brightness) (void)settings_set_int("bright", player_brightness());
    else                  (void)settings_set_int("vol",    player_volume_get());
}

/* 打开音量/亮度页。把两个入口（播放页长按 K2、菜单"屏幕亮度"、蓝牙遥控页长按 K2）
 * 收成一个，顺便记住来路 —— 这样 K4 就能统一按"上一级"处理。 */
void ui_vol_open(bool brightness, bool remote)
{
    s_vol_back = ui_page_current();
    ui_vol_set_mode(brightness);
    ui_vol_set_remote(remote);
    ui_page_show(UI_PAGE_VOL);
}

bool ui_vol_is_remote(void);
void ui_vol_set_remote(bool remote)
{
    s_vol_remote = remote;
    s_vol_remote_last = 0;          /* 每次进来从"还没按过"开始 */
    vol_render(true);
}

bool ui_vol_is_remote(void) { return s_vol_remote; }

/* K1 = 加、K3 = 减（用户指定） */
void ui_vol_step(int delta)
{
    if (s_vol_remote) {
        /* 对方音量：一次按一下，长按连发（连发由按键层的 hold 节流） */
        hid_remote_send(delta > 0 ? HID_KEY_VOL_UP : HID_KEY_VOL_DOWN);
        s_vol_remote_last = delta > 0 ? '+' : '-';
        vol_render(false);
        return;
    }
    if (s_vol_brightness) {
        player_set_brightness(player_brightness() + delta);   /* 下限在 player 里夹到 5% */
    } else {
        /* 调音量时顺手取消静音（用户按"加"了，显然是想听） */
        if (delta > 0 && player_is_muted()) player_set_mute(false);
        int v = player_volume_get() + delta;
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        player_volume(v);              /* 由 player 任务落到 ES8388（I2C 写有耗时，别在 UI 任务里做） */
    }
    vol_render(false);
}

void ui_vol_toggle_mute(void)
{
    if (s_vol_remote) {
        hid_remote_send(HID_KEY_MUTE);
        s_vol_remote_last = 'm';
        vol_render(false);
        return;
    }
    if (s_vol_brightness) {
        /* 亮度模式的 K2：直接拉到最亮/最暗，等于"看得见/省眼"两档 */
        player_set_brightness(player_brightness() > 60 ? 20 : 100);
        vol_render(false);
        return;
    }
    /* ★ 真静音：ES8388 的 DAC 硬静音。之前是把音量设成 0，用户反馈"不是真静音" */
    player_set_mute(!player_is_muted());
    vol_render(false);
}

static void build_vol_screen(void)
{
    lv_obj_t *scr = ui_new_screen();
    s_scr_vol = scr;
    ui_page_register(UI_PAGE_VOL, scr);

    ui_put_img(scr, ASSET_BG_VOL, 0, 0);
    ui_put_img(scr, ASSET_IC_NOTE, 4, 5);

    s_vol_top  = ui_put_label_1line(scr, "音量", 20, 2, 90, ui_font_16, 0x1C1C1E);
    s_vol_hint = ui_put_label_1line(scr, "", 112, 4, 44, ui_font_12, 0xD93025);

    /* 卡片里的大号数字（24px 拉丁，居中） */
    s_vol_num = ui_put_label(scr, "40", 0, 34, 160, &lv_font_montserrat_24, 0x1C1C1E,
                             LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_align(s_vol_num, LV_TEXT_ALIGN_CENTER, 0);

    /* 底图 (12,74) 是槽，这里叠填充 */
    s_vol_bar = lv_bar_create(scr);
    lv_obj_remove_style_all(s_vol_bar);
    lv_obj_set_pos(s_vol_bar, 12, 74);
    lv_obj_set_size(s_vol_bar, 136, 10);
    lv_bar_set_range(s_vol_bar, 0, 100);
    lv_bar_set_value(s_vol_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_anim_duration(s_vol_bar, 140, 0);   /* LV_ANIM_ON 用的时长 */
    lv_obj_set_style_bg_opa(s_vol_bar, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_vol_bar, lv_color_hex(0x1A73E8), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_vol_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_vol_bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
}

/* ============================================================ 统一入口 */

void ui_pages_build(void)
{
    build_list_screen();
    build_menu_screen();
    build_vol_screen();
    build_info_screen();
    build_bt_screen();
    build_src_screen();              /* 「模式」二级菜单（项数随后端动态） */
    build_back_screen();             /* 「来源」二级菜单（Navidrome / QQ 音乐） */
    build_set_screen();              /* 设置二级菜单（开机续播 / 重连 WiFi）*/
    build_qq_screen();               /* QQ 登录（设置里那一项按 K3 进）*/
    /* 音质二级菜单已按用户要求删除（固定用最高：无损优先）*/
    build_mem_screen();              /* 内存用量图表（系统信息页长按 K3 进）*/             /* 音质二级菜单（5 个挡位）*/
    ui_search_build();               /* M8：搜索方式 / 输入 / 结果 三页 */
    vol_render(true);
}

/* 背光要等 board_backlight_init() 之后才能设，所以单独给一个入口（ui.c 里调） */
void ui_pages_apply_brightness(void)
{
    /* 亮度值由 player 从 NVS 恢复（player_brightness()），这里只负责施加 ——
     * 背光必须等 board_backlight_init() 之后才能设 */
    board_backlight_set(player_brightness());
    ESP_LOGI(TAG, "背光按设置设为 %d%%", player_brightness());
}

void ui_pages_enter(ui_page_t p)
{
    switch (p) {
        case UI_PAGE_LIST: {
            /* 进来就定位到正在播的那首，省得从第 1 行开始翻 */
            int play = player_queue_index();
            s_list_sel = play >= 0 ? play : 0;
            list_render(true);
            break;
        }
        case UI_PAGE_MENU:
            s_menu_dirty = true;      /* 音频时钟那项的文字可能被别处改过 */
            menu_render();
            break;
        case UI_PAGE_VOL:
            vol_render(true);
            break;
        case UI_PAGE_INFO:
            info_render();
            break;
        case UI_PAGE_BT:
            bt_render();
            break;
        case UI_PAGE_SRC:
            s_src_r_sel = -1;
            src_render();
            break;
        case UI_PAGE_BACK:
            s_back_r_sel = -1;
            back_render();
            break;
        /* 进设置页顺带查一次 QQ 登录情况 —— 那一行的后缀要显示它。
         * 只是投一条后台命令，UI 不会被网络卡住。 */
        case UI_PAGE_SETTINGS:
            qqlogin_enter();
            set_render();
            break;
        case UI_PAGE_QQLOGIN:
            qq_render();
            break;
        case UI_PAGE_SEARCH_MODE:
        case UI_PAGE_SEARCH_IN:
        case UI_PAGE_SEARCH_RES:
            ui_search_enter(p);
            break;
        default:
            break;
    }
}

/* ★ 蓝牙控制模式是【状态源】，页面是跟随者。
 *
 * 为什么这样分：网页也能切这个模式（POST /api/hid），而 web_console 按分层约定
 * 不许碰 LVGL；所以由"模式"说了算，界面来对齐。
 *
 * ⚠️ 必须由 ui.c 的 tick 【无条件】调用（不能塞进 ui_pages_tick）：
 *    ui_pages_tick() 只在"不在播放页"时才被调 —— 而开机后正好停在播放页，
 *    于是"网页切了模式但屏幕一直不动"（实测踩到的就是这个）。
 */
void ui_bt_mode_follow(void)
{
    if (hid_remote_mode() && ui_page_current() == UI_PAGE_PLAY) {
        ui_page_show(UI_PAGE_BT);
    } else if (!hid_remote_mode() && ui_page_current() == UI_PAGE_BT) {
        ui_page_show(UI_PAGE_PLAY);
        s_vol_remote = false;    /* 模式关了，音量页也回本地变体（同上，防残留） */
    }
}

void ui_pages_tick(void)
{
    /* ★ 离开 QQ 登录页要**立刻**告诉后台停轮询 —— 否则它会白白轮询五分钟，
     *   还会在用户已经回到别的页面时把 toast 弹出来。
     *   放在这里（而不是某个页面自己的 K4 处理里）是因为出口有三条：
     *   K4 短按、K4 长按回播放器、以及网页那边切页面。集中判一次最稳。 */
    {
        static bool s_qq_page_on;
        bool on = (ui_page_current() == UI_PAGE_QQLOGIN);
        if (s_qq_page_on && !on) qqlogin_leave();
        s_qq_page_on = on;
    }

    switch (ui_page_current()) {
        case UI_PAGE_LIST: list_render(false); break;
        case UI_PAGE_MENU: menu_render();      break;
        case UI_PAGE_VOL:  vol_render(false);  break;
        case UI_PAGE_INFO: info_render();      break;
        case UI_PAGE_MEM:  mem_render();       break;
        case UI_PAGE_BT:   bt_render();        break;
        case UI_PAGE_SRC:  src_render();       break;
        case UI_PAGE_BACK: back_render();      break;
        case UI_PAGE_SETTINGS: set_tick();     break;
        case UI_PAGE_QQLOGIN:  qq_render();    break;
        case UI_PAGE_SEARCH_MODE:
        case UI_PAGE_SEARCH_IN:
        case UI_PAGE_SEARCH_RES:
            ui_search_tick();
            break;
        default: break;
    }
}
