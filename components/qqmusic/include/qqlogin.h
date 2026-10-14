/*
 * qqlogin —— 「QQ 登录」这一页的后台流程（设置 → QQ 登录）
 *
 * 用户要求（2026-10-14）：
 *   在设置二级菜单里能看到**当前 QQ 契约层服务的登录情况**，
 *   按 K3 取二维码扫码登录，确认登录后**回到显示登录情况**。
 *
 * 为什么单独一个模块 + 一条自己的任务：
 *   · 取二维码 / 轮询登录状态都是**阻塞几秒到几分钟**的事。
 *     按键回调跑在 esp_timer 任务里（iot_button 就是它调度的），在那里发 HTTP
 *     会把整机所有定时器一起卡住；UI 任务更不能碰（它一停屏幕就冻住）。
 *   · 所以这里只做三件事：**按键只投命令、后台任务干活、UI 任务读快照**。
 *     三个上下文之间只有一把锁，没有任何 LVGL 调用 —— 页面怎么画全在 ui_pages.c。
 *
 * ⚠️ 登录状态是**服务端的全局状态**（同一时刻只有一张有效的码），
 *    所以这里刻意不保存 identifier：多存一份就多一种"对不上"的可能。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "qrpng.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    QQLOGIN_IDLE = 0,     /* 显示登录情况（这就是用户要的"显示登录情况"） */
    QQLOGIN_QR_BUSY,      /* 正在取二维码 */
    QQLOGIN_QR_WAIT,      /* 二维码已就绪，等手机扫 */
    QQLOGIN_QR_SCANNED,   /* 已扫到，等手机上点确认 */
    QQLOGIN_ERROR,        /* 取码/轮询失败（reason 里是原因） */
} qqlogin_phase_t;

/* UI 任务读到的快照。由 qqlogin_view() 一次性拷贝出来 —— UI 绝不直接读内部状态。 */
typedef struct {
    qqlogin_phase_t phase;
    bool            fresh_login;   /* 刚扫码登录成功；UI 提示一次后调 qqlogin_ack() */
    char            state[32];     /* 「凭据有效」「凭据已过期」「等待扫码」… */
    char            account[32];   /* 账号（musicid）；没有则空串 */
    char            reason[96];    /* 原因 / 错误说明 */
    bool            qr_ready;      /* qr 有效 */
    qrpng_t         qr;            /* 二维码点阵（模块级，UI 自己按整数倍放大） */
} qqlogin_view_t;

/* 建锁、建队列、起后台任务。要在 qqmusic_init() 之后调用（不联网）。 */
void qqlogin_init(void);

/* 进/离开这一页。离开会**立刻**让后台停止轮询（不然它会白白轮询五分钟）。 */
void qqlogin_enter(void);
void qqlogin_leave(void);

/* K3：取一张新二维码。已在取 / 已在等扫码时也能按 —— 等扫码时会换一张新的。 */
void qqlogin_request_qr(void);

/* 读当前快照（UI 任务里调，4Hz 也没问题：一次约 600 字节的拷贝）。 */
void qqlogin_view(qqlogin_view_t *out);

/* UI 把"登录成功"提示弹过一次之后调它，避免每次 tick 都重复弹。 */
void qqlogin_ack(void);

#ifdef __cplusplus
}
#endif
