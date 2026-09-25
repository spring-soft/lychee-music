/*
 * UI 组件内部共享（ui.c / ui_pages.c / ui_keys.c 之间用，别对外暴露）
 *
 * 三条"避免整屏重绘"的铁律在这里以工具函数的形式固定下来：
 *   ui_new_screen()   —— 屏幕根对象一定关掉滚动/滚动条（否则边上有滑块）
 *   ui_plain_cont()   —— 动态控件的父对象一定无填充/无圆角/无阴影
 *   ui_set_text()     —— 文本先比较再写（省掉无谓的 invalidate）
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "lvgl.h"
#include "assets.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_PAGE_PLAY = 0,
    UI_PAGE_LIST,
    UI_PAGE_MENU,
    UI_PAGE_VOL,
    UI_PAGE_INFO,      /* 系统信息整页（一行 toast 装不下，用户要求能看到本机 IP） */
    UI_PAGE_BT,        /* M7 蓝牙控制模式（独立模式：进去以后按键只管对方，不显示本机歌名） */
    UI_PAGE_SRC,       /* 来源二级菜单（主菜单里"来源"那一项按 K3 进来的） */
    UI_PAGE_SEARCH_MODE,  /* M8：搜索方式选择（字母直搜 / 拼音输入）*/
    UI_PAGE_SEARCH_IN,    /* M8：输入页（字母格 + 拼音候选字，两种模式共用一页）*/
    UI_PAGE_SEARCH_RES,   /* M8：搜索结果列表 */
    UI_PAGE_COUNT,
} ui_page_t;

/* ---------------- ui.c 提供的工具 ---------------- */
lv_obj_t       *ui_new_screen(void);
lv_image_dsc_t *ui_img_dsc(asset_id_t id);
lv_obj_t       *ui_put_img(lv_obj_t *parent, asset_id_t id, int x, int y);
lv_obj_t       *ui_put_label(lv_obj_t *parent, const char *txt, int x, int y, int w,
                             const lv_font_t *font, uint32_t color,
                             lv_label_long_mode_t mode);
lv_obj_t       *ui_plain_cont(lv_obj_t *parent, int x, int y, int w, int h);
/* 真正的单行省略（DOTS 模式本身会折行，见 ui.c 的说明）—— 列表行/菜单行/提示条用这个 */
lv_obj_t       *ui_put_label_1line(lv_obj_t *parent, const char *txt, int x, int y, int w,
                                   const lv_font_t *font, uint32_t color);
void            ui_set_text(lv_obj_t *lbl, const char *txt);
void            ui_fmt_mmss(uint32_t ms, char *buf, size_t len);

extern lv_font_t       *ui_font_16;     /* 16px 中文（顶栏/列表行） */
extern lv_font_t       *ui_font_12;     /* 12px 中文（菜单行/提示） */
extern const lv_font_t *ui_font_lat;    /* 12px 拉丁（时间） */

/* 把字库里没有的字符换成 '?'（否则上屏是一片空白，见 ui.c 的说明） */
void ui_sanitize_text(const char *in, char *out, size_t out_len, const lv_font_t *font);
/* 开机自检：UI 文案用到的字符字库有没有（列表由 tools/gen_ui_text.py 生成）*/
void ui_font_audit_text(void);

/* 把对象纵向滑到目标位置（列表/菜单的高亮用）。ui.c 实现。 */
void ui_anim_slide_obj(lv_obj_t *o, int y, uint32_t ms);

/* ---------------- 页面注册与切换（ui.c） ---------------- */
void      ui_page_register(ui_page_t p, lv_obj_t *scr);
void      ui_page_show(ui_page_t p);
ui_page_t ui_page_current(void);
bool      ui_splash_running(void);

/* 顶部浮层提示（1.8 秒自动消失）。可从任意任务调（按键回调里调它很常见） */
void ui_toast(const char *text);

/* ---------------- ui_pages.c 实现的三个次级页面 ---------------- */
void ui_pages_build(void);
void ui_pages_apply_brightness(void);  /* 背光要等 board_backlight_init() 之后才能设 */
void ui_pages_enter(ui_page_t p);   /* 切进页面时强制刷新一次 */
void ui_pages_tick(void);           /* 当前页面可见时的周期刷新（UI 任务里调） */
/* 让页面跟随"蓝牙控制模式"（模式是状态源，网页也能改它）。
 * ⚠️ 必须由 ui.c 无条件调用 —— 别塞进 ui_pages_tick()，那个在播放页不跑。 */
void ui_bt_mode_follow(void);

/* ---------------- 各页面内的按键动作（ui_keys.c 调） ---------------- */
void ui_list_move(int delta);
void ui_list_play_selected(void);
void ui_menu_move(int delta);
void ui_menu_activate(void);
void ui_srcmenu_open(void);         /* 来源二级菜单：主菜单按 K3 进来 */
void ui_srcmenu_move(int delta);    /* K1 / K2 */
void ui_srcmenu_activate(void);     /* K3：确定并退回主菜单 */
void ui_vol_set_mode(bool brightness);
void ui_vol_open(bool brightness, bool remote);  /* 打开音量页并记住来路（K4 回上一级）*/
ui_page_t ui_page_parent(ui_page_t p);           /* K4 短按的"上一级" */
void ui_vol_set_remote(bool remote); /* 音量页的"蓝牙"变体：加减的是【对方】的音量 */
bool ui_vol_is_remote(void);
void ui_vol_step(int delta);        /* 单位 = % */
void ui_vol_toggle_mute(void);

/* ---------------- ui_pinyin.c（M8 拼音输入法的查表，表由 tools/gen_pinyin.py 生成） ----------------
 * 只读 assets 里的表、无状态、mmap 零拷贝。只在 UI 任务里调用。 */
esp_err_t pinyin_init(void);
bool      pinyin_ready(void);
/* 取某音节的候选汉字（码点）。out=NULL 或 max<=0 时只返回候选【个数】。 */
int       pinyin_candidates(const char *syllable, uint16_t *out, int max);
bool      pinyin_is_syllable(const char *s);   /* 是不是一个完整音节 */
bool      pinyin_is_prefix(const char *s);     /* 是不是某个音节的前缀（能不能继续输入）*/
bool      pinyin_can_extend(const char *s);    /* 有更长的音节以它开头（yu → yuan/yue/yun）*/

/* ---------------- ui_search.c（M8 屏上搜索的三个页面） ----------------
 * 单独一个文件，ui_pages.c / ui_keys.c 各转调一下，别把逻辑摊回那边去。
 * K4（上一级 / 回播放器）不在这里处理 —— 那是 ui_keys.c 的统一规则。 */
void ui_search_build(void);
void ui_search_open(void);           /* 主菜单进来：打开"搜索方式"页 */
void ui_search_enter(ui_page_t p);   /* 切进页面时重置+刷新（ui_pages_enter 转调）*/
void ui_search_tick(void);           /* 周期刷新（ui_pages_tick 转调）*/
bool ui_search_key_short(int k);     /* 返回 true = 这个键我处理了 */
bool ui_search_key_long(int k);
bool ui_search_key_hold(int k);

/* ---------------- ui_keys.c ---------------- */
void ui_keys_init(void);
/* 注入按键事件（k: 0..3 = K1..K4；kind: 0=短按 1=长按 2=保持）。
 * 只给开机自检用 —— 走的是和真实按键完全相同的分发路径。 */
void ui_keys_inject(int k, int kind);
/* 诊断：把当前页面名和搜索输入页的内部状态打到日志 */
const char *ui_page_name(ui_page_t p);
void ui_search_debug_dump(void);

#ifdef __cplusplus
}
#endif
