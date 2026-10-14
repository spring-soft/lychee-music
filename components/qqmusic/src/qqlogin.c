/*
 * qqlogin —— 实现（设计说明见 include/qqlogin.h）
 *
 * 线程模型（三个上下文，一把锁，零 LVGL 调用）：
 *
 *   按键回调(esp_timer)  --xQueueOverwrite(长度1, 覆盖式)-->  后台任务 qqlogin
 *   UI 任务(ui_pages_tick)  --qqlogin_view() 拷快照-->  画屏
 *
 * 队列长度取 1 且用 Overwrite：要的是"最新意图"，不是把历史命令排队执行
 * （连按 K3 十下，只想登录一次，不想画十张码）。
 */
#include <stdio.h>
#include <string.h>
#include "qqlogin.h"
#include "qqmusic.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"      /* heap_caps_free：PNG 缓冲是 PSRAM 上分配的 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

static const char *TAG = "qqlogin";

#define LOGIN_TASK_STACK   6144
#define LOGIN_TASK_PRIO    4
#define POLL_INTERVAL_MS   1500                  /* 扫码状态轮询间隔 */
#define POLL_TIMEOUT_MS    (5 * 60 * 1000)       /* 上游二维码有效期约 5 分钟 */

#define CMD_STATUS 0
#define CMD_QR     1

static SemaphoreHandle_t s_lock;
static QueueHandle_t     s_cmd;
static qqlogin_view_t    s_view;
static volatile bool     s_leave;                /* 离开页面 → 轮询立刻停 */
static volatile bool     s_req_qr;               /* 轮询中又按了 K3 → 换一张码 */
static bool              s_started;

static void set_view(qqlogin_phase_t ph, const char *state, const char *reason)
{
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    s_view.phase = ph;
    if (state) strlcpy(s_view.state, state, sizeof(s_view.state));
    if (reason) strlcpy(s_view.reason, reason, sizeof(s_view.reason));
    else s_view.reason[0] = 0;
    /* 只有"码在屏上"的两种状态保留点阵，其余一律丢掉 —— 不然会留着上一张码 */
    if (ph != QQLOGIN_QR_WAIT && ph != QQLOGIN_QR_SCANNED) s_view.qr_ready = false;
    if (s_lock) xSemaphoreGive(s_lock);
}

/* ============================================================ 登录情况（IDLE 视图） */

static void do_status(bool fresh)
{
    qqmusic_health_t h;
    esp_err_t e = qqmusic_health_ex(&h);

    char state[32];
    char reason[96] = { 0 };

    /* ★ 顺序很重要：先判"通不通"，再判"凭据到底什么情况"。
     *   实测过期时服务端仍然 ok=true（VIP 接口照样能用），所以**不能拿 ok 判登录**。 */
    if (!h.reachable) {
        strlcpy(state, e == ESP_ERR_INVALID_STATE ? "没配服务地址" : "连不上服务", sizeof(state));
        strlcpy(reason, h.reason, sizeof(reason));
    } else if (!h.present) {
        strlcpy(state, "未登录", sizeof(state));
        strlcpy(reason, "按 K3 扫码登录", sizeof(reason));
    } else if (h.expired) {
        strlcpy(state, "凭据已过期", sizeof(state));
        strlcpy(reason, "按 K3 重新扫码", sizeof(reason));
    } else if (!h.valid) {
        strlcpy(state, "凭据无效", sizeof(state));
        strlcpy(reason, h.reason[0] ? h.reason : "按 K3 重新扫码", sizeof(reason));
    } else {
        strlcpy(state, "凭据有效", sizeof(state));
        strlcpy(reason, h.vip ? "VIP 已识别，可拿无损" : "未识别到 VIP", sizeof(reason));
    }

    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    s_view.phase       = QQLOGIN_IDLE;
    s_view.qr_ready    = false;
    s_view.fresh_login = fresh;
    strlcpy(s_view.state, state, sizeof(s_view.state));
    strlcpy(s_view.reason, reason, sizeof(s_view.reason));
    if (h.musicid > 0) snprintf(s_view.account, sizeof(s_view.account), "%lld", h.musicid);
    else               s_view.account[0] = 0;
    if (s_lock) xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "登录情况: %s（通=%d 有=%d 有效=%d 过期=%d VIP=%d）",
             state, (int)h.reachable, (int)h.present, (int)h.valid,
             (int)h.expired, (int)h.vip);
}

/* ============================================================ 等扫码

 * 返回：0 = 登录成功（凭据已落盘）  1 = 离开页面
 *       2 = 用户又按了 K3，要换一张码  3 = 失败（已经 set_view 报过原因了） */
static int poll_login(void)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)POLL_TIMEOUT_MS * 1000;
    int64_t next     = 0;

    for (;;) {
        if (s_leave) return 1;
        if (s_req_qr) return 2;

        int64_t now = esp_timer_get_time();
        if (now >= deadline) {
            set_view(QQLOGIN_ERROR, "二维码已过期", "再按 K3 重新获取");
            return 3;
        }

        if (now >= next) {
            next = now + (int64_t)POLL_INTERVAL_MS * 1000;
            char ev[24] = { 0 };
            char er[96] = { 0 };
            bool saved  = false;

            if (qqmusic_login_status(ev, sizeof(ev), &saved, er, sizeof(er)) == ESP_OK) {
                ESP_LOGI(TAG, "扫码状态: %s（已落盘=%d）", ev, (int)saved);
                if (strcmp(ev, "DONE") == 0)    return 0;
                if (strcmp(ev, "CONF") == 0)    set_view(QQLOGIN_QR_SCANNED, "已扫码，请在手机上确认", NULL);
                else if (strcmp(ev, "TIMEOUT") == 0) {
                    set_view(QQLOGIN_ERROR, "二维码已过期", "再按 K3 重新获取");
                    return 3;
                } else if (strcmp(ev, "REFUSE") == 0) {
                    set_view(QQLOGIN_ERROR, "手机上取消了登录", "再按 K3 重试");
                    return 3;
                } else if (strcmp(ev, "ERROR") == 0) {
                    set_view(QQLOGIN_ERROR, "服务端监听扫码失败", er);
                    return 3;
                } else {
                    /* WAITING / SCAN：还没扫到 */
                    set_view(QQLOGIN_QR_WAIT, "等待扫码", "用手机 QQ 音乐 App 扫");
                }
            } else {
                /* 单次查询失败不当致命：网络抖一下很正常，下一轮再试 */
                ESP_LOGW(TAG, "查扫码状态失败，%d ms 后重试", POLL_INTERVAL_MS);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/* ============================================================ 取码 → 等扫码 → 收尾 */
static void do_qr(void)
{
    s_req_qr = false;

    for (;;) {
        if (s_leave) { set_view(QQLOGIN_IDLE, NULL, NULL); return; }

        set_view(QQLOGIN_QR_BUSY, "正在获取二维码…", "要等几秒");
        ESP_LOGI(TAG, "开始取二维码");

        uint8_t *png = NULL;
        size_t   n   = 0;
        esp_err_t e  = qqmusic_login_qr_png(&png, &n);

        qrpng_t qr;
        if (e == ESP_OK) {
            e = qrpng_decode(png, n, &qr);
            heap_caps_free(png);
        }

        if (e != ESP_OK) {
            const char *why = esp_err_to_name(e);
            switch (e) {
            case ESP_ERR_NOT_SUPPORTED:    why = "二维码图格式不认识"; break;
            case ESP_ERR_NOT_FOUND:        why = "图里找不到二维码";   break;
            case ESP_ERR_INVALID_RESPONSE: why = "服务返回的图坏了";   break;
            case ESP_ERR_NO_MEM:           why = "内存不够";           break;
            default: break;
            }
            ESP_LOGW(TAG, "取二维码失败: %s", why);
            set_view(QQLOGIN_ERROR, "获取二维码失败", why);
            return;
        }

        /* 把点阵打进日志 —— 屏幕经常不可信（黑屏/没插 FPC），
         * 这是 PC 侧唯一能复核"这张码到底扫不扫得动"的途径。 */
        qrpng_dump(&qr);
        set_view(QQLOGIN_QR_WAIT, "等待扫码", "用手机 QQ 音乐 App 扫");
        if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
        s_view.qr = qr;
        s_view.qr_ready = true;
        if (s_lock) xSemaphoreGive(s_lock);

        int rc = poll_login();
        if (rc == 0) {
            ESP_LOGW(TAG, "扫码登录成功");
            do_status(true);          /* ← 用户要的：确认登录后回到显示登录情况 */
            return;
        }
        if (rc == 1) { set_view(QQLOGIN_IDLE, NULL, NULL); return; }   /* 离开页面 */
        if (rc == 3) return;                                            /* 已报过原因 */
        /* rc == 2：换一张码，转回去重取 */
        s_req_qr = false;
    }
}

static void login_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "登录任务已启动");
    for (;;) {
        uint8_t cmd = 0;
        if (xQueueReceive(s_cmd, &cmd, portMAX_DELAY) != pdTRUE) continue;
        if (cmd == CMD_QR) do_qr();
        else               do_status(false);
    }
}

/* ============================================================ 对外接口 */

void qqlogin_init(void)
{
    if (s_started) return;

    s_lock = xSemaphoreCreateMutex();
    s_cmd  = xQueueCreate(1, sizeof(uint8_t));
    if (s_lock == NULL || s_cmd == NULL) {
        ESP_LOGE(TAG, "初始化失败：锁或命令队列没建起来");
        return;
    }
    memset(&s_view, 0, sizeof(s_view));
    s_view.phase = QQLOGIN_IDLE;
    strlcpy(s_view.state, "点 K3 扫码登录", sizeof(s_view.state));

    if (xTaskCreate(login_task, "qqlogin", LOGIN_TASK_STACK, NULL,
                    LOGIN_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "登录任务创建失败");
        return;
    }
    s_started = true;
    ESP_LOGI(TAG, "扫码登录就绪（服务 %s）", qqmusic_host());
}

void qqlogin_enter(void)
{
    if (!s_started) return;
    s_leave  = false;
    s_req_qr = false;
    uint8_t c = CMD_STATUS;
    xQueueOverwrite(s_cmd, &c);
}

void qqlogin_leave(void)
{
    if (!s_started) return;
    s_leave  = true;
    s_req_qr = false;
}

void qqlogin_request_qr(void)
{
    if (!s_started) return;

    qqlogin_phase_t ph;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ph = s_view.phase;
    xSemaphoreGive(s_lock);

    if (ph == QQLOGIN_QR_BUSY) return;      /* 正在取，别重复触发 */

    if (ph == QQLOGIN_QR_WAIT || ph == QQLOGIN_QR_SCANNED) {
        /* 后台正在轮询这一张：让它换一张（码会过期，用户也可能就是想重来） */
        s_req_qr = true;
        return;
    }

    s_leave = false;
    set_view(QQLOGIN_QR_BUSY, "正在获取二维码…", "要等几秒");
    uint8_t c = CMD_QR;
    xQueueOverwrite(s_cmd, &c);
}

void qqlogin_view(qqlogin_view_t *out)
{
    if (out == NULL) return;
    if (!s_started) {
        memset(out, 0, sizeof(*out));
        strlcpy(out->state, "登录模块没起来", sizeof(out->state));
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_view;                 /* 一次整体拷贝，之后 UI 随便用 */
    xSemaphoreGive(s_lock);
}

void qqlogin_ack(void)
{
    if (!s_started) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_view.fresh_login = false;
    xSemaphoreGive(s_lock);
}
