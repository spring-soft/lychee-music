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
#include "ui.h"
#include "ui_internal.h"
#include "assets.h"
#include "player.h"
#include "app_core.h"
#include "audio_engine.h"
#include "board.h"
#include "net_mgr.h"
#include "settings.h"
#include "hid_remote.h"
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

    /* 顶栏：来源 + "3/30"（%.16s 限宽：GCC 会按 const char* 的最坏长度算截断，
     * 不加会被 -Werror=format-truncation 拦下来） */
    char buf[64];
    if (count > 0) {
        snprintf(buf, sizeof(buf), "%.16s %d/%d", player_src_name(player_source()),
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
#define MENU_N     6                       /* 菜单总项数（现在正好一屏，不再滚动） */

static lv_obj_t *s_scr_menu;
static lv_obj_t *s_menu_hl;
static lv_obj_t *s_menu_lbl[MENU_ROWS];
static int       s_menu_sel;
static int       s_menu_r_sel = -1;
static int       s_menu_win;
static bool      s_menu_dirty = true;

/* 蓝牙遥控的短状态（菜单第 8 项的动态后缀）。
 * ⚠️ 用中文而不是 "BT ON/CONN" —— 字库是 GB2312 子集，ASCII 走 Montserrat
 *    兜底没问题，但中英混排在这个小字号上更难看；而且用户看的是中文。 */
static const char *bt_state_short(void)
{
    if (!hid_remote_available()) return "不可用";
    if (hid_remote_mode())       return hid_remote_connected() ? "控制中" : "未连";
    return hid_remote_connected() ? "已连" : "待连";
}

static const char *menu_name(int i)
{
    /* 第 1、2 项是动态的（蓝牙状态 / 当前来源），文本在这两个静态缓冲里 */
    static char s_menu_bt_lbl[32];
    static char s_menu_src_lbl[48];
    switch (i) {
        /* ★ 用户要求：蓝牙控制【独立成一项、放第一个】—— 它是个模式，不是
         *   混在播放器设置里的一个开关，所以摆在最上面最好找。 */
        case 0:
            snprintf(s_menu_bt_lbl, sizeof(s_menu_bt_lbl), "蓝牙控制 %s", bt_state_short());
            return s_menu_bt_lbl;
        /* ★ 用户要求：来源只占【一项】，冒号后面直接显示当前来源；
         *   按 K3 进二级菜单选（K1/K2 选、K3 确定退出）。 */
        case 1:
            snprintf(s_menu_src_lbl, sizeof(s_menu_src_lbl), "来源: %s",
                     player_src_name(player_source()));
            return s_menu_src_lbl;
        case 2: return "搜索";                 /* ★ 取代原来的"播放 / 暂停" */
        case 3: return "屏幕亮度";
        case 4: return "重连 WiFi";
        case 5: return "系统信息";
        default: return "";
    }
}

static void menu_render(void)
{
    /* 前两项的文字是动态的（蓝牙状态、当前来源）。它们一变就把菜单标脏，
     * 否则要等到用户翻页才刷新 —— 用户会以为"换了来源但屏幕上没反应"。 */
    {
        static char prev[64];
        char now[64];
        snprintf(now, sizeof(now), "%s|%s", bt_state_short(), player_src_name(player_source()));
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
        case UI_PAGE_SEARCH_MODE: return UI_PAGE_MENU;
        case UI_PAGE_SEARCH_IN:   return UI_PAGE_SEARCH_MODE;   /* 用户要的：回输入页要先过方式选择 */
        case UI_PAGE_SEARCH_RES:  return UI_PAGE_SEARCH_IN;     /* 用户要的：方便改词重搜 */
        case UI_PAGE_LIST:        return UI_PAGE_PLAY;
        case UI_PAGE_INFO:        return UI_PAGE_MENU;
        case UI_PAGE_VOL:         return s_vol_back;
        case UI_PAGE_BT:          return UI_PAGE_PLAY;          /* 遥控页的"上一级"= 退出模式 */
        default:                  return UI_PAGE_PLAY;
    }
}

/* ============================================================ 来源二级菜单
 *
 * 用户要求：主菜单里"来源"只占一项（冒号后面显示当前来源），
 * 按 K3 进这一页，K1/K2 选、K3 确定并退出、K4 返回。
 * 底图/高亮条直接复用菜单页的（同一套视觉，不额外做素材）。
 */
#define SRC_N  3

static lv_obj_t *s_scr_src, *s_src_hl, *s_src_lbl[SRC_N];
static int       s_src_sel;
static int       s_src_r_sel = -1;

static void src_render(void)
{
    for (int i = 0; i < SRC_N; i++) {
        char buf[48];
        bool cur = (player_source() == (player_src_t)i);
        bool empty = (i == PLAYER_SRC_RECENT && player_recent_count() == 0);
        /* 标出"现在是哪个"，来源为空也标出来 —— 不然用户会奇怪为什么选了没反应 */
        snprintf(buf, sizeof(buf), "%s%s%s", player_src_name((player_src_t)i),
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
    ui_put_label_1line(scr, "来源", 20, 2, 134, ui_font_16, 0x1C1C1E);

    s_src_hl = ui_put_img(scr, ASSET_HL_MENU, 0, MENU_Y0);
    for (int i = 0; i < SRC_N; i++) {
        s_src_lbl[i] = ui_put_label_1line(scr, "", 12, MENU_Y0 + i * MENU_ROW_H + 1,
                                         136, ui_font_12, 0x1C1C1E);
    }
    ESP_LOGI(TAG, "来源二级菜单已建好（%d 项：K1/K2 选、K3 确定、K4 返回）", SRC_N);
}

void ui_srcmenu_open(void)
{
    /* 光标默认停在【当前来源】上，用户一进来就知道自己在哪 */
    s_src_sel = (int)player_source();
    if (s_src_sel < 0 || s_src_sel >= SRC_N) s_src_sel = 0;
    s_src_r_sel = -1;
    ui_page_show(UI_PAGE_SRC);
}

void ui_srcmenu_move(int delta)
{
    s_src_sel = (s_src_sel + delta + SRC_N) % SRC_N;
}

void ui_srcmenu_activate(void)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "正在获取: %.16s", player_src_name((player_src_t)s_src_sel));
    ui_toast(buf);
    player_set_source((player_src_t)s_src_sel, true);   /* 立刻播第一首 */
    s_menu_dirty = true;      /* 主菜单那行"来源: X"要跟着变 */
    ui_page_show(UI_PAGE_MENU);                          /* 确定并退出（回上级菜单） */
}

/* 系统信息整页 —— 一行 toast 放不下这么多事实（IP + 内存 + 欠载 + …），
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
        case 0:   /* ★ 用户明确要求：系统信息里要能看到本机 IP */
            snprintf(buf, len, "本机 IP  %.16s", net_mgr_ip_str());
            break;
        case 1:
            if (net_mgr_is_connected())
                snprintf(buf, len, "WiFi  %.10s  %d dBm", net_mgr_ssid(), net_mgr_rssi());
            else
                snprintf(buf, len, "WiFi  未连接");
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
            snprintf(buf, len, "曲目  %d/%d  %s", st.queue_index + 1, st.queue_count,
                     player_src_name(player_source()));
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

static void info_render(void)
{
    char buf[64];
    for (int i = 0; i < INFO_ROWS; i++) {
        info_line(i, buf, sizeof(buf));
        ui_set_text(s_info_lbl[i], buf);      /* 内部先比较，没变就不重绘 */
    }
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


static void menu_pick_source(player_src_t src)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "正在获取: %.16s", player_src_name(src));
    ui_toast(buf);
    player_set_source(src, true);      /* 立刻播第一首 */
    ui_page_show(UI_PAGE_PLAY);
}

static void menu_activate(int i)
{
    switch (i) {
        case 0:                                  /* ★ 蓝牙控制：进入独立模式 */
            if (!hid_remote_available()) {
                ui_toast("蓝牙不可用");
            } else {
                hid_remote_set_mode(true);
                ui_page_show(UI_PAGE_BT);        /* 立刻切，不等 tick */
                ui_toast(hid_remote_connected() ? "按键只发给对方" : "还没连上对端");
            }
            s_menu_dirty = true;
            break;
        case 1: ui_srcmenu_open(); break;        /* 来源 → 二级菜单 */
        case 2: ui_search_open(); break;       /* 搜索（播放/暂停 在播放页有 K2）*/
        case 3: ui_vol_open(true, false); break;   /* 记住来路=菜单，K4 会回菜单 */
        case 4: net_mgr_reconnect(); ui_toast("正在重连 WiFi..."); break;
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
    build_src_screen();
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
    switch (ui_page_current()) {
        case UI_PAGE_LIST: list_render(false); break;
        case UI_PAGE_MENU: menu_render();      break;
        case UI_PAGE_VOL:  vol_render(false);  break;
        case UI_PAGE_INFO: info_render();      break;
        case UI_PAGE_BT:   bt_render();        break;
        case UI_PAGE_SRC:  src_render();       break;
        case UI_PAGE_SEARCH_MODE:
        case UI_PAGE_SEARCH_IN:
        case UI_PAGE_SEARCH_RES:
            ui_search_tick();
            break;
        default: break;
    }
}
