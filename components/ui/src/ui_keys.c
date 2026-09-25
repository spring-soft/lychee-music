/*
 * K1-K4 按键
 *
 * 用 espressif/button（esp_timer 驱动，不额外占任务），只做"事件 → 动作"的映射，
 * 动作全部转给 player / ui_pages 的接口 —— 按键层不碰音频引擎内部，也不碰 LVGL 对象。
 *
 * 键位表（与方案里确认过的语义一致）：
 *   播放页  短按  K1 下一首 | K2 播放/暂停 | K3 上一首 | K4 换来源（下一个，立刻播）
 *   播放页  长按  K1 收藏/取消 | K2 音量页 | K3 列表页 | K4 菜单
 *   列表页  短按  K1 上一行 | K2 下一行 | K3 播放选中 | K4 返回播放页
 *   列表页  长按  K1/K2 连续翻行（K3/K4 同短按）
 *   菜单页  短按  K1 上 | K2 下 | K3 确认 | K4 返回播放页
 *   音量页  短按  K1 音量− | K2 静音 | K3 音量＋ | K4 返回播放页
 *   音量页  长按  K1/K3 连续调节
 *   任意页  长按 K4 都是"回播放页"
 *
 * ⚠️ K1 在 GPIO3（strapping 脚）：开机按住它会进 JTAG 下载模式。
 *    正常使用没问题，但别在上电瞬间按着 K1。
 */
#include <stdio.h>
#include <string.h>
#include "ui_internal.h"
#include "player.h"
#include "app_core.h"
#include "board.h"
#include "hid_remote.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "iot_button.h"
#include "button_gpio.h"

static const char *TAG = "ui.keys";

#define KEY_COUNT        4
#define HOLD_REPEAT_MS   160      /* 长按连滚/连调的节流（HOLD 回调本身很密） */

static const int       k_pins[KEY_COUNT] = {
    BOARD_PIN_KEY1, BOARD_PIN_KEY2, BOARD_PIN_KEY3, BOARD_PIN_KEY4
};
static button_handle_t s_btn[KEY_COUNT];
static int64_t         s_hold_last_ms;
static uint32_t        s_evt_count;

/* ============================================================ M7 蓝牙控制模式
 *
 * 菜单第一项进入，进的是 UI_PAGE_BT 那一页（独立界面）。进去以后
 * **按键只发给对端**，本机播放器一动都不动：
 *   K1 下一首 ｜ K2 播放/暂停 ｜ K3 上一首
 *   K2 长按  音量调整（和播放器里长按 K2 一致，进去后 K1/K3 加减、K2 静音）
 *   K4       退出模式，回播放器
 * 长按 K1/K2/K3 在模式里没有别的语义，一律吞掉 —— 不吞的话"长按 K1 收藏"
 * 会一边收藏本机一边给对方发下一首，用户会以为设备坏了。
 */
static bool in_bt_mode(void) { return ui_page_current() == UI_PAGE_BT; }

static bool bt_key_short(int k)
{
    if (!in_bt_mode()) return false;

    /* K4 不在这里处理 —— 它由最前面的 k4_navigate() 统一管（顺带关掉遥控模式）。
     * ⚠️ 这里只剩 K1~K3，所以下面"没连上就吃掉"的逻辑**再也吞不到出口键**了
     * （之前就是因为吞了 K4 才退不出去）。 */
    if (!hid_remote_active()) {
        /* 其余按键：模式开着但没连上就**吃掉**，绝不掉回本机控制（那就又混在一起了） */
        ui_toast("蓝牙未连接");
        return true;
    }
    switch (k) {
        case 0: hid_remote_send(HID_KEY_NEXT);       break;
        case 1: hid_remote_send(HID_KEY_PLAY_PAUSE); break;
        case 2: hid_remote_send(HID_KEY_PREV);       break;
    }
    return true;
}

static bool bt_key_long(int k)
{
    if (!in_bt_mode()) return false;
    if (k == 3) return false;          /* K4 由 k4_navigate 统一管，这里不碰 */
    if (k == 1 && hid_remote_active()) {
        ui_vol_open(false, true);      /* 遥控音量页；K4 会回到本页（来路被记住了）*/
    }
    return true;
}

/* ============================================================ K4：全机唯一的出口
 *
 * ⚠️⚠️ 这个函数存在的理由：**同一个坑我踩了三次** ——
 *   ① 蓝牙控制页"没连上就把按键吃掉"那段把 K4 一起吞了 → 用户卡在遥控页出不来
 *   ② 搜索页我特意让 K4"放行"，但 key_short 里没写搜索页的 case → 掉到 default
 *      **什么都不做** → 用户搜完歌按 K4 回不去
 *   ③ 更早还有"来源页长按 K4 单独特判"（和其它页规则不一致）
 * 全是"规则散在每个页面里，改一处漏一处"。所以现在只在这里判一次：
 *   - 短按 K4 → 回 ui_page_parent(当前页)
 *   - 长按 K4 → 直接回播放器
 *   - **播放页例外**（最外层界面，用户确认保持原样：短按换来源、长按进菜单）
 *   - 离开蓝牙控制页 = 顺带关掉遥控模式（不关的话 ui_bt_mode_follow 会把页面又拉回去）
 * 各页面自己的 K1/K2/K3 处理里**不用再管 K4**。
 */
static bool k4_navigate(int k, bool long_press)
{
    if (k != 3) return false;
    ui_page_t cur = ui_page_current();
    if (cur == UI_PAGE_PLAY) return false;      /* 播放页自己处理（见文件头的键位表）*/

    if (cur == UI_PAGE_BT) hid_remote_set_mode(false);
    if (long_press) ui_page_show(UI_PAGE_PLAY);
    else            ui_page_show(ui_page_parent(cur));
    return true;
}

/* ============================================================ 动作映射 */

static void key_short(int k)
{
    if (k4_navigate(k, false)) return;          /* K4 只在这一处判（见上面说明）*/
    if (bt_key_short(k)) return;
    if (ui_search_key_short(k)) return;
    switch (ui_page_current()) {
        case UI_PAGE_PLAY:
            switch (k) {
                case 0: player_next();   break;
                case 1: player_toggle();  break;
                case 2: player_prev();   break;
                case 3: {                        /* 换来源（下一个），立刻播 */
                    player_src_t nx = (player_src_t)((player_source() + 1) % PLAYER_SRC_COUNT);
                    if (nx == PLAYER_SRC_RECENT && player_recent_count() == 0) {
                        nx = (player_src_t)((nx + 1) % PLAYER_SRC_COUNT);   /* 还没播过任何歌 */
                    }
                    char buf[48];
                    snprintf(buf, sizeof(buf), "来源: %s", player_src_name(nx));
                    ui_toast(buf);
                    player_set_source(nx, true);
                    break;
                }
            }
            break;

        case UI_PAGE_LIST:
            switch (k) {
                case 0: ui_list_move(-1); break;
                case 1: ui_list_move(+1); break;
                case 2: ui_list_play_selected(); break;
                default: break;              /* K1/K2/K3 之外的键（含 K4）由 k4_navigate 管 */
            }
            break;

        case UI_PAGE_MENU:
            switch (k) {
                case 0: ui_menu_move(-1); break;
                case 1: ui_menu_move(+1); break;
                case 2: ui_menu_activate(); break;
                default: break;              /* K4 由 k4_navigate 管 */
            }
            break;

        case UI_PAGE_SRC:                       /* 来源二级菜单 */
            switch (k) {
                case 0: ui_srcmenu_move(-1); break;
                case 1: ui_srcmenu_move(+1); break;
                case 2: ui_srcmenu_activate(); break;   /* 确定并退回主菜单 */
                default: break;              /* K4 由 k4_navigate 管 */
            }
            break;

        case UI_PAGE_VOL:
            switch (k) {
                case 0: ui_vol_step(+5); break;       /* K1 = 加（用户指定） */
                case 1: ui_vol_toggle_mute(); break;
                case 2: ui_vol_step(-5); break;       /* K3 = 减 */
                default: break;              /* K4 由 k4_navigate 管 */
            }
            break;

        case UI_PAGE_INFO:
            /* 纯展示页：别的键不做事（免得"随手按一下"就把这页顶掉）。
             * K4 回上一级由 k4_navigate 管。 */
            break;

        default: break;
    }
}

static void key_long(int k)
{
    if (k4_navigate(k, true)) return;           /* 长按 K4 = 回播放器（除非在播放页）*/
    if (bt_key_long(k)) return;
    if (ui_search_key_long(k)) return;
    if (ui_page_current() != UI_PAGE_PLAY) return;   /* 播放页以外的长按没有其它语义 */
    switch (k) {
        case 0: {                                  /* 收藏 / 取消收藏 */
            player_status_t st;
            player_status_get(&st);
            if (!st.song_id[0]) { ui_toast("还没有在放歌"); break; }
            player_star(!st.starred);
            /* 别加 "★"：字库只到 GB2312 + 中文标点，U+2605 会显示成方框 */
            ui_toast(st.starred ? "已取消收藏" : "已收藏");
            break;
        }
        case 1: ui_vol_open(false, false); break;   /* remote=false 顺便清掉残留标志 */
        case 2: ui_page_show(UI_PAGE_LIST); break;
        case 3: ui_page_show(UI_PAGE_MENU); break;
    }
}

/* 长按保持：只有"连滚/连调"用得上，节流到 160ms 一次 */
static void key_hold(int k)
{
    int64_t now = esp_timer_get_time() / 1000;
    if (now - s_hold_last_ms < HOLD_REPEAT_MS) return;
    s_hold_last_ms = now;

    if (in_bt_mode()) return;      /* 遥控页没有"连发"语义（不会想连跳十首） */
    if (ui_search_key_hold(k)) return;   /* 输入页 K1/K2 连移光标 */

    switch (ui_page_current()) {
        case UI_PAGE_LIST:
            if (k == 0) ui_list_move(-1);
            else if (k == 1) ui_list_move(+1);
            break;
        case UI_PAGE_VOL:
            if (k == 0) ui_vol_step(+5);          /* K1 长按连加 */
            else if (k == 2) ui_vol_step(-5);     /* K3 长按连减 */
            break;
        default: break;
    }
}

/* ============================================================ 事件回调 */

static void cb_short(void *handle, void *usr) { s_evt_count++; key_short((int)(intptr_t)usr); }
static void cb_long(void *handle, void *usr)  { s_evt_count++; key_long((int)(intptr_t)usr); }
static void cb_hold(void *handle, void *usr)  { s_evt_count++; key_hold((int)(intptr_t)usr); }

void ui_keys_init(void)
{
    for (int i = 0; i < KEY_COUNT; i++) {
        button_config_t cfg = {
            /* 700ms 判长按：太短会在"想连按两下下一首"时误触发收藏 */
            .long_press_time  = 700,
            .short_press_time = 60,       /* 消抖 */
        };
        button_gpio_config_t gpio = {
            .gpio_num     = k_pins[i],
            .active_level = 0,            /* 低电平按下（板上有内部上拉） */
            .enable_power_save = false,
            .disable_pull      = false,
        };
        esp_err_t err = iot_button_new_gpio_device(&cfg, &gpio, &s_btn[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "K%d (GPIO%d) 创建失败: %s", i + 1, k_pins[i], esp_err_to_name(err));
            s_btn[i] = NULL;
            continue;
        }
        void *arg = (void *)(intptr_t)i;
        iot_button_register_cb(s_btn[i], BUTTON_SINGLE_CLICK,      NULL, cb_short, arg);
        iot_button_register_cb(s_btn[i], BUTTON_LONG_PRESS_START,  NULL, cb_long,  arg);
        iot_button_register_cb(s_btn[i], BUTTON_LONG_PRESS_HOLD,   NULL, cb_hold,  arg);
    }
    ESP_LOGI(TAG, "K1-K4 就绪（GPIO %d/%d/%d/%d，长按 700ms，连滚 %dms）",
             k_pins[0], k_pins[1], k_pins[2], k_pins[3], HOLD_REPEAT_MS);
}

/* 注入一个按键事件（只给开机自检用，见 K4_SELFTEST）。
 * 走的是和真实按键**完全相同**的分发函数 —— 否则自检就失去意义了。 */
void ui_keys_inject(int k, int kind)
{
    if (k < 0 || k > 3) return;
    s_evt_count++;
    if (kind == 0)      key_short(k);
    else if (kind == 1) key_long(k);
    else                key_hold(k);
}
