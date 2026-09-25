/*
 * ui_search —— M8 屏上搜索：方式选择 / 输入（字母格 + 拼音输入法）/ 结果
 *
 * 三个页面放一个文件，不塞进 ui_pages.c（那边已经管着列表/菜单/音量/信息/蓝牙/来源
 * 六个页面）。对外只暴露 build / open / enter / tick / key_* 几个入口。
 *
 * ## 为什么要在设备上做拼音输入法
 * 屏幕只有一个 160×128 的字母格，**打不出汉字**；而 Navidrome 服务端不支持拼音搜索
 * （2026-09-25 实测：yue/ylkbd/zjl/caijianya 全部 0 结果，官方也无此功能）。
 * 所以由设备把字母变成真汉字，再把汉字交给服务器搜 —— 搜索始终由服务器做，
 * **用户新加的歌立刻能搜到**，不存在"本地索引过时"（这正是用户否掉本地索引的理由）。
 *
 * ## 输入页版面（128px 高，一格一格抠出来的）
 *   y   0..19   顶栏：已输入内容（汉字 + 未确认的音节）
 *   y  20..35   状态行：光标在哪 / 这个键会干什么（屏小，全靠这行做提示）
 *   y  36..75   候选区：2 行 × 8 = 16 格（每格 20×20，16px 中文字）
 *   y  76..107  字母格：A-M / N-Z 两行（每行 13 格，12×16，12px 拉丁）
 *   y 108..123  动作行：`退格` / `确定`（各 80 宽）
 *
 * ## 光标模型：一条线，不是两个区
 *   0..25 = A-Z ｜ 26 = 退格 ｜ 27 = 确定 ｜ 28.. = 候选字（数量随音节变）
 * 为什么不做"字母区/候选区"两个独立光标：那样就得设计"怎么从候选区回去改字"，
 * 一不小心就是陷阱（用户进了候选区发现退格键够不着）。一条线走下来最省心 ——
 * 一直往左按就能从候选字倒回 确定 → 退格，**永远出得来**。
 *
 * ## 按键（k: 0..3 = K1..K4；K4 由 ui_keys.c 按"上一级/播放器"统一处理，这里放行）
 *   K1 / K2  光标 →/←（**K1 向右、K2 向左**，用户指定；长按连移）
 *   K3 短按  看光标在哪一格：字母=输入 / 退格=删 / 确定=去挑字或开搜 / 候选字=选它
 *   K3 长按  **开始搜索**（两种模式一样）
 *
 * ## 拼音的省事之处
 * 音节打完且再打字母也变不成更长的音节（yue/lai/bu/dong…）→ 光标**自动跳到候选字**，
 * 常用情况就是"打字母 + K3 选字"两步。还能延长的（yu → yuan/yue/yun）不跳，
 * 免得挡住用户继续打。想挑字也可以自己按 K2 走到"确定"。
 */
#include <string.h>
#include <stdio.h>
#include "ui_internal.h"
#include "player.h"
#include "assets.h"
#include "esp_log.h"

static const char *TAG = "ui.search";

/* ---------------- 版面常量 ---------------- */
#define SROW_H    17                    /* 方式选择页：沿用菜单页的行高 */
#define SROW_Y0   22

#define KEY_COLS  13                    /* 字母格：两行 13 格 */
#define KEY_W     (160 / KEY_COLS)      /* 12 */
#define KEY_H     16
#define KEY_Y0    76
#define ACT_Y0    108
#define ACT_W     80
#define CAND_COLS 8                     /* 候选区：2 行 8 格 */
#define CAND_W    (160 / CAND_COLS)     /* 20 */
#define CAND_H    20
#define CAND_Y0   36
#define CAND_N    (2 * CAND_COLS)       /* 屏上能显示 16 个候选 */

#define CELL_BACK 26                    /* 光标落在"退格"上的位置 */
#define CELL_OK   27                    /* "确定" */
#define CAND_BASE 28                    /* 第一个候选字 */

#define OUT_MAX   36                    /* 已确认汉字（UTF-8 最多 ~12 字） */
#define SYLL_MAX  10

#define RES_ROWS  5                     /* 结果页：和列表页一样 5 行 × 22px */
#define RES_ROW_H 22
#define RES_Y0    20

/* ============================================================ 状态 */

static bool s_pinyin_mode;              /* 当前走的是不是拼音（两种模式共用一页）*/

/* 方式选择页 */
static lv_obj_t *s_scr_smode, *s_smode_hl, *s_smode_lbl[2];
static int       s_smode_sel, s_smode_r_sel = -1;

/* 输入页 */
static lv_obj_t *s_scr_in, *s_in_top, *s_in_hint;
static lv_obj_t *s_in_hl_key, *s_in_hl_act, *s_in_hl_cand;
static lv_obj_t *s_in_cand[CAND_N];
static lv_obj_t *s_in_key[26];
static lv_obj_t *s_in_act[2];

static char     s_in_out[OUT_MAX];      /* 已确认的汉字 */
static char     s_in_syll[SYLL_MAX];    /* 未确认的音节（小写字母）*/
static uint16_t s_in_cand_list[128];    /* 当前音节的全部候选（表里可能 100+ 个）*/
static int      s_in_ncand;
static int      s_in_pos;               /* 线性光标，见文件头 */
static int      s_in_r_ncand = -1;      /* 上次渲染的候选数（-1 = 脏，要重查）*/
static int      s_in_r_pos = -1;

/* 结果页 */
static lv_obj_t *s_scr_res, *s_res_top, *s_res_hl, *s_res_note;
static lv_obj_t *s_res_lbl[RES_ROWS];
static int      s_res_sel, s_res_win;
static int      s_res_r_sel = -1, s_res_r_win = -1;
static uint32_t s_res_r_gen = 0xFFFFFFFF;

/* ============================================================ 小工具 */

static void scroll_window(int sel, int n_items, int rows, int *win)
{
    if (n_items <= 0) { *win = 0; return; }
    if (sel < *win) *win = sel;
    if (sel >= *win + rows) *win = sel - rows + 1;
    if (*win > n_items - rows) *win = n_items - rows;
    if (*win < 0) *win = 0;
}

static void hl_move(lv_obj_t *hl, int x, int y)
{
    if (lv_obj_get_x(hl) == x && lv_obj_get_y(hl) == y) return;
    lv_obj_set_pos(hl, x, y);
}

/* 码点 → UTF-8（表里全是 BMP，最多 3 字节）。表是拿字库字符集过滤过的，
 * 所以不会出现"屏上画不出来的字"。 */
static int cp_to_utf8(uint16_t cp, char *out)
{
    int n = 0;
    if (cp < 0x800) {
        out[n++] = (char)(0xC0 | (cp >> 6));
        out[n++] = (char)(0x80 | (cp & 0x3F));
    } else {
        out[n++] = (char)(0xE0 | (cp >> 12));
        out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[n++] = (char)(0x80 | (cp & 0x3F));
    }
    out[n] = 0;
    return n;
}

/* ============================================================ 方式选择页 */

static void smode_render(void)
{
    if (s_smode_r_sel == s_smode_sel) return;
    s_smode_r_sel = s_smode_sel;
    ui_anim_slide_obj(s_smode_hl, SROW_Y0 + s_smode_sel * SROW_H, 120);
}

static void build_smode_screen(void)
{
    lv_obj_t *scr = ui_new_screen();
    s_scr_smode = scr;
    ui_page_register(UI_PAGE_SEARCH_MODE, scr);

    ui_put_img(scr, ASSET_BG_MENU, 0, 0);
    ui_put_img(scr, ASSET_IC_NOTE, 4, 5);
    ui_put_label_1line(scr, "搜索方式", 20, 2, 134, ui_font_16, 0x1C1C1E);

    /* 高亮要在文字之前建（LVGL 层级 = 创建顺序），否则会盖住字 */
    s_smode_hl = ui_put_img(scr, ASSET_HL_MENU, 0, SROW_Y0);
    /* 名字压短：136px 宽、12px 字，10 个汉字就是 120px，再多就截了 */
    static const char *names[2] = { "字母直搜（英文歌名）", "拼音输入（中文歌名）" };
    for (int i = 0; i < 2; i++) {
        s_smode_lbl[i] = ui_put_label_1line(scr, names[i], 12, SROW_Y0 + i * SROW_H + 1,
                                           136, ui_font_12, 0x1C1C1E);
    }
}

void ui_search_open(void)               /* 主菜单第 3 项进来 */
{
    s_smode_sel = 0;
    s_smode_r_sel = -1;
    smode_render();
    ui_page_show(UI_PAGE_SEARCH_MODE);
}

/* ============================================================ 输入页：状态与渲染 */

static int sin_total_cells(void)
{
    return CAND_BASE + (s_pinyin_mode ? s_in_ncand : 0);
}

/* 光标格子的屏幕坐标（三种格子大小差太多，各自算）*/
static void cell_xy(int pos, int *x, int *y, int *w, int *h)
{
    if (pos < 26) {                              /* 字母 */
        *x = (pos % KEY_COLS) * KEY_W;
        *y = KEY_Y0 + (pos / KEY_COLS) * KEY_H;
        *w = KEY_W;
        *h = KEY_H;
    } else if (pos < CAND_BASE) {                /* 退格 / 确定 */
        *x = (pos - CELL_BACK) * ACT_W;
        *y = ACT_Y0;
        *w = ACT_W;
        *h = KEY_H;
    } else {                                     /* 候选字 */
        int win = 0;
        scroll_window(s_in_pos - CAND_BASE, s_in_ncand, CAND_N, &win);
        int v = (pos - CAND_BASE) - win;
        if (v < 0 || v >= CAND_N) v = 0;         /* 正常进不来，防越界 */
        *x = (v % CAND_COLS) * CAND_W;
        *y = CAND_Y0 + (v / CAND_COLS) * CAND_H;
        *w = CAND_W;
        *h = CAND_H;
    }
}

static void sin_clamp_pos(void)
{
    int total = sin_total_cells();
    if (s_in_pos >= total) s_in_pos = total - 1;
    if (s_in_pos < 0) s_in_pos = 0;
}

static void sin_invalidate_cand(void) { s_in_r_ncand = -1; }

/* 重新查表 + 把 16 个候选格子填上（音节变了、光标移进候选区时调）*/
static void sin_refresh_candidates(void)
{
    s_in_ncand = s_pinyin_mode
               ? pinyin_candidates(s_in_syll, s_in_cand_list,
                                   (int)(sizeof(s_in_cand_list) / 2))
               : 0;

    int win = 0;
    scroll_window(s_in_pos >= CAND_BASE ? s_in_pos - CAND_BASE : 0,
                  s_in_ncand, CAND_N, &win);

    for (int i = 0; i < CAND_N; i++) {
        int idx = win + i;
        if (idx >= s_in_ncand) {
            lv_obj_add_flag(s_in_cand[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        char b[8];
        cp_to_utf8(s_in_cand_list[idx], b);
        ui_set_text(s_in_cand[i], b);
        lv_obj_remove_flag(s_in_cand[i], LV_OBJ_FLAG_HIDDEN);
    }
    s_in_r_ncand = s_in_ncand;
    /* 候选数一变，候选区那个高亮框的落点也可能跟着变（窗口滚了），
     * 所以顺手把"已渲染位置"作废，让下一帧重算 */
    s_in_r_pos = -1;
}

static void sin_render_hl(void)
{
    bool in_cand = (s_in_pos >= CAND_BASE);
    bool in_act  = (s_in_pos >= CELL_BACK && s_in_pos < CAND_BASE);

    if (in_cand) lv_obj_add_flag(s_in_hl_key, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_remove_flag(s_in_hl_key, LV_OBJ_FLAG_HIDDEN);
    if (in_act)  lv_obj_remove_flag(s_in_hl_act, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_add_flag(s_in_hl_act, LV_OBJ_FLAG_HIDDEN);
    if (in_cand) lv_obj_remove_flag(s_in_hl_cand, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_add_flag(s_in_hl_cand, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *hl = in_cand ? s_in_hl_cand : (in_act ? s_in_hl_act : s_in_hl_key);
    int x, y, w, h;
    cell_xy(s_in_pos, &x, &y, &w, &h);
    if (lv_obj_get_width(hl) != w - 2 || lv_obj_get_height(hl) != h - 2) {
        lv_obj_set_size(hl, w - 2, h - 2);
    }
    hl_move(hl, x + 1, y + 1);
    s_in_r_pos = s_in_pos;
}

static void sin_render(void)
{
    char buf[80];

    /* 顶栏：已确认的汉字 + 正在打的音节 */
    if (s_in_out[0] || s_in_syll[0]) {
        snprintf(buf, sizeof(buf), "搜索 %.30s%.8s", s_in_out, s_in_syll);
    } else {
        snprintf(buf, sizeof(buf), "搜索 ...");
    }
    ui_set_text(s_in_top, buf);

    /* 状态行：说清楚"现在按 K3 会干什么"（屏太小，没地方放键位图）*/
    if (player_search_busy()) {
        ui_set_text(s_in_hint, "搜索中...");
    } else if (!s_pinyin_mode) {
        snprintf(buf, sizeof(buf), "字母 %s（长按K3搜）", s_in_out);
        ui_set_text(s_in_hint, buf);
    } else if (s_in_pos >= CAND_BASE) {
        snprintf(buf, sizeof(buf), "选字 %d/%d  K3选它",
                 s_in_pos - CAND_BASE + 1, s_in_ncand);
        ui_set_text(s_in_hint, buf);
    } else if (s_in_syll[0]) {
        snprintf(buf, sizeof(buf), "拼音 %s  %s", s_in_syll,
                 s_in_ncand ? "K2 去挑字" : "继续打字母");
        ui_set_text(s_in_hint, buf);
    } else {
        ui_set_text(s_in_hint, "打拼音再挑字（长按K3直接搜）");
    }

    if (s_in_r_ncand != s_in_ncand) sin_refresh_candidates();
    sin_clamp_pos();
    if (s_in_r_pos != s_in_pos) sin_render_hl();
}

/* 追加一个字母；返回是否被接受。
 * ⚠️ 字母格给的是大写 A-Z，而拼音表里全是小写（"yue"/"hang"）——
 *    不转小写的话 pinyin_is_prefix() 全不匹配，**一个字母都打不进去**。 */
static bool sin_type_letter(char c)
{
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');

    if (!s_pinyin_mode) {                 /* 字母直搜：没有音节概念 */
        size_t n = strlen(s_in_out);
        if (n + 1 < OUT_MAX) { s_in_out[n] = c; s_in_out[n + 1] = 0; }
        return true;
    }

    char next[SYLL_MAX];
    snprintf(next, sizeof(next), "%s%c", s_in_syll, c);
    /* 必须还是某个音节的前缀，否则拒绝 —— 不把输入带进死路 */
    if (!pinyin_is_prefix(next)) return false;

    strlcpy(s_in_syll, next, sizeof(s_in_syll));
    sin_invalidate_cand();

    /* 音节已完整、且再打字母也变不成更长的音节（yue/lai/bu/dong…）→
     * 光标自动跳到候选字上。常用的中文歌名基本都在这一类。 */
    if (pinyin_is_syllable(s_in_syll) && !pinyin_can_extend(s_in_syll)) {
        int ncand = pinyin_candidates(s_in_syll, s_in_cand_list,
                                      (int)(sizeof(s_in_cand_list) / 2));
        s_in_ncand = ncand;
        if (ncand > 0) s_in_pos = CAND_BASE;
    }
    return true;
}

static void sin_backspace(void)
{
    if (!s_pinyin_mode) {
        size_t n = strlen(s_in_out);
        if (n) s_in_out[n - 1] = 0;
        return;
    }
    if (s_in_syll[0]) {
        s_in_syll[strlen(s_in_syll) - 1] = 0;
        sin_invalidate_cand();
        return;
    }
    /* 音节空了就退一个已确认汉字（UTF-8：往前退到上一个非续字节）*/
    size_t n = strlen(s_in_out);
    while (n > 0 && ((unsigned char)s_in_out[n - 1] & 0xC0) == 0x80) n--;
    if (n > 0) s_in_out[n - 1] = 0;
}

/* 选定当前高亮的候选字 */
static void sin_pick_candidate(void)
{
    int c = s_in_pos - CAND_BASE;
    if (c < 0 || c >= s_in_ncand) return;

    char u[8];
    int n = cp_to_utf8(s_in_cand_list[c], u);
    size_t len = strlen(s_in_out);
    if (len + (size_t)n < OUT_MAX) memcpy(s_in_out + len, u, (size_t)n + 1);

    /* 选完回字母格，接着打下一个音节 */
    s_in_syll[0] = 0;
    s_in_pos = 0;
    sin_invalidate_cand();
    sin_refresh_candidates();
}

/* "确定"格：把光标送到候选字 */
static void sin_confirm_syllable(void)
{
    if (!s_pinyin_mode || s_in_syll[0] == 0 || !pinyin_is_syllable(s_in_syll)) {
        ui_toast("先打完一个音节");
        return;
    }
    sin_refresh_candidates();
    if (s_in_ncand <= 0) { ui_toast("这个音节没有候选字"); return; }
    s_in_pos = CAND_BASE;
}

/* 拼出查询串：拼音模式 = 已确认汉字 + 未确认音节；字母模式 = 已输入字母 */
static void sin_build_query(char *out, size_t cap)
{
    if (s_pinyin_mode) snprintf(out, cap, "%s%s", s_in_out, s_in_syll);
    else               snprintf(out, cap, "%s", s_in_out);
}

static void sin_start_search(void)
{
    char q[64];
    sin_build_query(q, sizeof(q));
    if (q[0] == 0) { ui_toast("还没输入"); return; }

    player_search(q);                 /* 非阻塞：网络在 player 任务里做 */
    s_res_sel = 0;
    s_res_win = 0;
    s_res_r_gen = 0xFFFFFFFF;
    s_res_r_sel = -1;
    ui_page_show(UI_PAGE_SEARCH_RES);
}

static void ui_search_mode_move(int d)
{
    s_smode_sel = (s_smode_sel + d + 2) % 2;
    smode_render();
}

static void ui_search_mode_enter(void)
{
    s_pinyin_mode = (s_smode_sel == 1);
    /* 换模式就清干净：字母直搜的字母和拼音的音节不是一回事，留着只会让人困惑 */
    s_in_out[0]  = 0;
    s_in_syll[0] = 0;
    s_in_ncand   = 0;
    s_in_pos     = 0;
    sin_invalidate_cand();
    ui_page_show(UI_PAGE_SEARCH_IN);
}

/* ============================================================ 结果页 */

static void res_render(bool force)
{
    int n = player_search_count();
    int win = 0;
    scroll_window(s_res_sel, n, RES_ROWS, &win);

    if (force || s_res_r_gen != player_search_gen() || win != s_res_r_win) {
        s_res_r_gen = player_search_gen();
        s_res_r_win = win;
        for (int i = 0; i < RES_ROWS; i++) {
            int idx = win + i;
            sub_song_t s;
            if (idx < n && player_search_get(idx, &s)) {
                /* 标题来自服务器，必须过 sanitize：字库缺字会让整行变空白 */
                char tbuf[168];
                ui_sanitize_text(s.title[0] ? s.title : "(无标题)", tbuf, sizeof(tbuf),
                                 ui_font_16);
                ui_set_text(s_res_lbl[i], tbuf);
                lv_obj_remove_flag(s_res_lbl[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                ui_set_text(s_res_lbl[i], "");
                lv_obj_add_flag(s_res_lbl[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    char buf[64];
    if (player_search_busy()) {
        snprintf(buf, sizeof(buf), "搜索中...");
    } else if (n > 0) {
        snprintf(buf, sizeof(buf), "%.16s  %d/%d", player_search_query(), s_res_sel + 1, n);
    } else if (player_search_err() != ESP_OK) {
        snprintf(buf, sizeof(buf), "搜索失败（网络？）");
    } else {
        snprintf(buf, sizeof(buf), "%.16s  没找到", player_search_query());
    }
    ui_set_text(s_res_top, buf);

    if (n == 0) {
        ui_set_text(s_res_note,
                    player_search_busy() ? "正在搜索..."
                  : (player_search_err() != ESP_OK ? "搜索失败：检查 Navidrome 连接"
                                                   : "没找到这首歌，回上一级改个词"));
        lv_obj_remove_flag(s_res_note, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_res_hl, LV_OBJ_FLAG_HIDDEN);
        s_res_r_sel = -1;
        return;
    }
    lv_obj_add_flag(s_res_note, LV_OBJ_FLAG_HIDDEN);
    if (s_res_r_sel != s_res_sel || force) {
        s_res_r_sel = s_res_sel;
        ui_anim_slide_obj(s_res_hl, RES_Y0 + (s_res_sel - win) * RES_ROW_H, 120);
        lv_obj_remove_flag(s_res_hl, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ============================================================ 建屏 */

static void build_res_screen(void)
{
    lv_obj_t *scr = ui_new_screen();
    s_scr_res = scr;
    ui_page_register(UI_PAGE_SEARCH_RES, scr);

    ui_put_img(scr, ASSET_BG_LIST, 0, 0);
    ui_put_img(scr, ASSET_IC_NOTE, 4, 5);
    s_res_top = ui_put_label_1line(scr, "", 20, 2, 134, ui_font_16, 0x1C1C1E);

    /* ⚠️ 高亮必须在行文字【之前】建，否则会盖住字 */
    s_res_hl = ui_put_img(scr, ASSET_HL_ROW, 0, RES_Y0);
    for (int i = 0; i < RES_ROWS; i++) {
        s_res_lbl[i] = ui_put_label_1line(scr, "", 10, RES_Y0 + i * RES_ROW_H + 1, 132,
                                         ui_font_16, 0x1C1C1E);
    }
    s_res_note = ui_put_label_1line(scr, "", 10, 64, 142, ui_font_12, 0x3C3C40);
    lv_obj_add_flag(s_res_note, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI(TAG, "搜索结果页已建好（%d 行）", RES_ROWS);
}

static void build_in_screen(void)
{
    lv_obj_t *scr = ui_new_screen();
    s_scr_in = scr;
    ui_page_register(UI_PAGE_SEARCH_IN, scr);

    ui_put_img(scr, ASSET_BG_INFO, 0, 0);
    /* assets 里没有放大镜，顶栏图标也用音符 */
    ui_put_img(scr, ASSET_IC_NOTE, 4, 5);
    s_in_top  = ui_put_label_1line(scr, "搜索 ...", 20, 2, 134, ui_font_16, 0x1C1C1E);
    s_in_hint = ui_put_label_1line(scr, "", 4, 21, 152, ui_font_12, 0x3C3C40);

    /* 高亮框（都在文字之前建）。三种大小：字母 12×16 / 动作 80×16 / 候选 20×20，
     * 差太多，用一个去缩放不值得 —— 建三个，只显示对的那个。 */
    s_in_hl_cand = ui_plain_cont(scr, 0, CAND_Y0, CAND_W, CAND_H);
    s_in_hl_key  = ui_plain_cont(scr, 0, KEY_Y0, KEY_W, KEY_H);
    s_in_hl_act  = ui_plain_cont(scr, 0, ACT_Y0, ACT_W, KEY_H);
    lv_obj_t *hls[3] = { s_in_hl_cand, s_in_hl_key, s_in_hl_act };
    for (int i = 0; i < 3; i++) {
        lv_obj_set_style_bg_color(hls[i], lv_color_hex(0xC8D8F0), 0);
        lv_obj_set_style_bg_opa(hls[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(hls[i], 1, 0);
        lv_obj_set_style_border_color(hls[i], lv_color_hex(0x4A78C0), 0);
        lv_obj_add_flag(hls[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* 候选区 16 格 */
    for (int i = 0; i < CAND_N; i++) {
        int c = i % CAND_COLS, r = i / CAND_COLS;
        s_in_cand[i] = ui_put_label(scr, "", c * CAND_W, CAND_Y0 + r * CAND_H + 1,
                                    CAND_W, ui_font_16, 0x1C1C1E, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_style_text_align(s_in_cand[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_add_flag(s_in_cand[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* 字母格 A-M / N-Z。用 12px 拉丁（ui_font_lat）——
     * 中文字库是刻意排除 ASCII 的，字母靠它的 fallback（见 ui.c 的字库说明）。 */
    for (int i = 0; i < 26; i++) {
        int c = i % KEY_COLS, r = i / KEY_COLS;
        char b[2] = { (char)('A' + i), 0 };
        s_in_key[i] = ui_put_label(scr, b, c * KEY_W, KEY_Y0 + r * KEY_H + 1, KEY_W,
                                   ui_font_lat, 0x1C1C1E, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_style_text_align(s_in_key[i], LV_TEXT_ALIGN_CENTER, 0);
    }

    /* 动作行：退格 / 确定 */
    static const char *acts[2] = { "退格", "确定" };
    for (int i = 0; i < 2; i++) {
        s_in_act[i] = ui_put_label_1line(scr, acts[i], i * ACT_W, ACT_Y0 + 1, ACT_W,
                                        ui_font_12, 0x1C1C1E);
        lv_obj_set_style_text_align(s_in_act[i], LV_TEXT_ALIGN_CENTER, 0);
    }
    ESP_LOGI(TAG, "搜索输入页已建好（线性光标 0..%d：A-Z + 退格 + 确定 + 候选）",
             CAND_BASE - 1);
}

/* ============================================================ 按键 */

bool ui_search_key_short(int k)
{
    /* ⚠️ K4 **必须放行**：它的语义（短按回上一级、长按回播放器）由 ui_keys.c
     * 按统一的页面层级处理，这里一旦吞掉就再也回不去了。 */
    if (k == 3) return false;

    switch (ui_page_current()) {
        case UI_PAGE_SEARCH_MODE:
            if (k == 0)      ui_search_mode_move(-1);
            else if (k == 1) ui_search_mode_move(+1);
            else if (k == 2) ui_search_mode_enter();
            return true;

        case UI_PAGE_SEARCH_IN:
            /* ⚠️ 用户指定：**K1 = 向右、K2 = 向左**（和列表/菜单页的"上/下"方向相反，
             * 这里是横向光标，用户要的就是这个手感）。 */
            if (k == 0 || k == 1) {
                s_in_pos += (k == 0) ? 1 : -1;
                sin_clamp_pos();
                if (s_in_pos >= CAND_BASE) sin_refresh_candidates();
                return true;
            }
            /* K3：看光标停在哪一格 */
            if (s_in_pos >= CAND_BASE)        sin_pick_candidate();
            else if (s_in_pos < 26) {
                if (!sin_type_letter((char)('A' + s_in_pos))) ui_toast("拼不出这个音节");
            } else if (s_in_pos == CELL_BACK) {
                sin_backspace();
                sin_clamp_pos();
            } else if (s_pinyin_mode)         sin_confirm_syllable();
            else                              sin_start_search();
            return true;

        case UI_PAGE_SEARCH_RES: {
            int n = player_search_count();
            if (n <= 0) return true;
            if (k == 0 || k == 1) {
                s_res_sel += (k == 0) ? -1 : 1;
                if (s_res_sel < 0) s_res_sel = 0;
                if (s_res_sel >= n) s_res_sel = n - 1;
            } else if (k == 2) {
                sub_song_t s;
                if (player_search_get(s_res_sel, &s)) {
                    player_play_song(&s);      /* 元数据已在手，省一次 getSong 往返 */
                    char b[64];
                    snprintf(b, sizeof(b), "播放 %.40s", s.title);
                    ui_toast(b);
                }
            }
            return true;
        }

        default: return false;
    }
}

bool ui_search_key_long(int k)
{
    if (k == 3) return false;                 /* K4 交给统一规则 */
    if (ui_page_current() == UI_PAGE_SEARCH_IN && k == 2) {
        sin_start_search();                   /* 长按 K3 = 开始搜索 */
        return true;
    }
    return false;
}

bool ui_search_key_hold(int k)
{
    if (ui_page_current() != UI_PAGE_SEARCH_IN) return false;
    /* K1/K2 长按连移光标：最多 28+16 格，一下一下按太累。
     * K3 不能连发 —— 它的长按是"开始搜索"，连发会搜出一串空词。 */
    if (k == 0 || k == 1) {
        s_in_pos += (k == 0) ? 1 : -1;      /* K1 向右、K2 向左（同短按）*/
        sin_clamp_pos();
        if (s_in_pos >= CAND_BASE) sin_refresh_candidates();
        return true;
    }
    return k != 3;
}

/* ============================================================ 对外接口 */

void ui_search_build(void)
{
    build_smode_screen();
    build_in_screen();
    build_res_screen();
}

void ui_search_enter(ui_page_t p)
{
    switch (p) {
        case UI_PAGE_SEARCH_MODE:
            s_smode_r_sel = -1;
            smode_render();
            break;
        case UI_PAGE_SEARCH_IN:
            sin_clamp_pos();
            s_in_r_pos = -1;
            sin_render();
            break;
        case UI_PAGE_SEARCH_RES:
            s_res_r_gen = 0xFFFFFFFF;
            s_res_r_sel = -1;
            res_render(true);
            break;
        default: break;
    }
}

void ui_search_tick(void)
{
    switch (ui_page_current()) {
        case UI_PAGE_SEARCH_MODE: smode_render();    break;
        case UI_PAGE_SEARCH_IN:   sin_render();      break;
        case UI_PAGE_SEARCH_RES:  res_render(false); break;
        default: break;
    }
}

/* 诊断：把输入页的内部状态打一行（自检和排障用）*/
void ui_search_debug_dump(void)
{
    uint16_t c[3] = {0};
    int n = pinyin_candidates(s_in_syll, c, 3);
    char cb[32] = {0};
    for (int i = 0; i < n && i < 3; i++) {
        char u[8];
        cp_to_utf8(c[i], u);
        strlcat(cb, u, sizeof(cb));
    }
    ESP_LOGW(TAG, "  输入页状态: 模式=%s 已输入=\"%s\" 音节=\"%s\" 光标=%d "
                  "候选=%d 前几个=[%s]",
             s_pinyin_mode ? "拼音" : "字母", s_in_out, s_in_syll, s_in_pos, s_in_ncand, cb);
}
