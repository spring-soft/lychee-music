#include "usblink.h"

#include <string.h>
#include <stdio.h>
#include <stdatomic.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "net_mgr.h"
#include "qqmusic.h"
#include "subsonic.h"
#include "freertos/queue.h"

static const char *TAG = "usblink";

/* 跟烧录口同一个串口（ESP32-S3 DevKit 的 UART 口 = UART0 @115200） */
#define LINK_UART       UART_NUM_0
#define LINK_BAUD       115200
#define LINK_RX_BUF     2048
#define LINK_LINE_MAX   192

static bool              s_ready;
static SemaphoreHandle_t s_lock;
static char              s_last_reply[LINK_LINE_MAX];
static char              s_last_cmd[LINK_LINE_MAX];
static atomic_uint       s_rx_lines;
static atomic_uint       s_tx_lines;

/* 服务器 → 板子的一行（已去掉 "ESP<" 前缀） */
static void on_server_line(const char *text)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_last_reply, text, sizeof(s_last_reply));
    xSemaphoreGive(s_lock);
    atomic_fetch_add(&s_rx_lines, 1);
    ESP_LOGW(TAG, "服务器 → 板子: %s", text);
}

static void link_task(void *arg)
{
    uint8_t buf[128];
    char    line[LINK_LINE_MAX];
    int     n = 0;

    for (;;) {
        int got = uart_read_bytes(LINK_UART, buf, sizeof(buf), pdMS_TO_TICKS(200));
        for (int i = 0; i < got; i++) {
            char c = (char)buf[i];
            if (c == '\r') continue;
            if (c == '\n') {
                line[n] = 0;
                if (n > 0 && strncmp(line, "ESP<", 4) == 0) {
                    on_server_line(line + 4);
                }
                n = 0;
            } else if (n < LINK_LINE_MAX - 1) {
                line[n++] = c;
            } else {
                n = 0;              /* 太长了：丢掉，别让日志把缓冲塞满 */
            }
        }
    }
}

esp_err_t usblink_init(void)
{
    if (s_ready) return ESP_OK;
    if (s_lock == NULL) s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;

    /* ⚠️ 只装接收缓冲（tx_buffer_size=0）：
     *   这条口本来就被日志用着（esp_rom_printf 直接写 FIFO），
     *   我们只需要"读"输入；发送用 uart_write_bytes 直接写，不加自己的 TX 环。 */
    esp_err_t err = uart_driver_install(LINK_UART, LINK_RX_BUF, 0, 0, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "串口驱动装不上: %s", esp_err_to_name(err));
        return err;
    }
    err = uart_param_config(LINK_UART, &(uart_config_t){
        .baud_rate = LINK_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    });
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "串口参数配不上: %s", esp_err_to_name(err));
        return err;
    }
    /* 3KB 栈足够（只拼行 + 打日志） */
    if (xTaskCreate(link_task, "usblink", 3072, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_ready = true;
    ESP_LOGW(TAG, "有线指令通道就绪（UART0 %d 8N1，认 ESP< 开头的行）", LINK_BAUD);
    return ESP_OK;
}

bool usblink_ready(void) { return s_ready; }

esp_err_t usblink_send(const char *cmd)
{
    if (!s_ready || cmd == NULL || cmd[0] == 0) return ESP_ERR_INVALID_STATE;
    char line[LINK_LINE_MAX];
    int len = snprintf(line, sizeof(line), "ESP>%s\n", cmd);
    if (len <= 0) return ESP_ERR_INVALID_ARG;
    if (len >= (int)sizeof(line)) len = sizeof(line) - 1;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_last_cmd, cmd, sizeof(s_last_cmd));
    xSemaphoreGive(s_lock);

    int written = uart_write_bytes(LINK_UART, line, len);
    if (written != len) {
        ESP_LOGW(TAG, "发指令写串口只写了 %d/%d 字节", written, len);
        return ESP_FAIL;
    }
    atomic_fetch_add(&s_tx_lines, 1);
    ESP_LOGW(TAG, "板子 → 服务器: %s", cmd);
    return ESP_OK;
}

const char *usblink_last_reply(void)
{
    static char out[LINK_LINE_MAX];
    if (s_lock == NULL) return "";
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(out, s_last_reply, sizeof(out));
    xSemaphoreGive(s_lock);
    return out;
}

const char *usblink_last_cmd(void)
{
    static char out[LINK_LINE_MAX];
    if (s_lock == NULL) return "";
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(out, s_last_cmd, sizeof(out));
    xSemaphoreGive(s_lock);
    return out;
}

uint32_t usblink_rx_lines(void) { return atomic_load(&s_rx_lines); }
uint32_t usblink_tx_lines(void) { return atomic_load(&s_tx_lines); }

/* ============================================================ 一键配网状态机 */
static struct {
    volatile int  running;        /* 0=空闲 1=跑着 */
    volatile int  pct;
    char          msg[160];
    char          ssid[33];
    char          pass[65];
    volatile int  step;           /* 1=连ESP 2=通知服务器 3=等服务器 4=校准 */
} s_pv;

static void pv_set(int pct, const char *fmt, ...)
{
    va_list ap;
    s_pv.pct = pct;
    va_start(ap, fmt);
    vsnprintf(s_pv.msg, sizeof(s_pv.msg), fmt, ap);
    va_end(ap);
    ESP_LOGW(TAG, "[配网 %d%%] %s", pct, s_pv.msg);
}

static void provision_task(void *arg)
{
    s_pv.step = 1;
    pv_set(5, "开始：让 ESP 连上「%s」", s_pv.ssid);
    esp_err_t err = net_mgr_set_sta(s_pv.ssid, s_pv.pass[0] ? s_pv.pass : NULL);
    if (err != ESP_OK) { pv_set(-1, "ESP 连接失败：%s", esp_err_to_name(err)); goto done; }

    /* 等 ESP 拿到 IP（最多 30 秒）——这一步 AP 会瞬断，正常 */
    bool ok = false;
    for (int i = 0; i < 60; i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
        const char *ip = net_mgr_ip_str();
        if (ip && ip[0]) { pv_set(40, "ESP 已连上，IP=%s", ip); ok = true; break; }
        if ((i % 4) == 0) pv_set(15, "ESP 连接中… %d 秒（AP 会瞬断，属正常）", i / 2);
    }
    if (!ok) { pv_set(-1, "ESP 30 秒内没连上（密码错或信号弱）"); goto done; }

    /* 通过串口让服务器连同一个网络 */
    s_pv.step = 2;
    pv_set(55, "正在让服务器连「%s」…", s_pv.ssid);
    char cmd[160];
    snprintf(cmd, sizeof(cmd), "WIFI %s %s", s_pv.ssid, s_pv.pass);
    if (usblink_send(cmd) != ESP_OK) { pv_set(-1, "串口发送失败（数据线插好了吗）"); goto done; }

    /* 等服务器回执里的 ip=（最多 150 秒） */
    s_pv.step = 3;
    char srv_ip[32] = {0};
    for (int i = 0; i < 150; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        const char *r = usblink_last_reply();
        const char *p = r ? strstr(r, "ip=") : NULL;
        if (p) {
            p += 3;
            int n = 0;
            while (p[n] && p[n] != ' ' && n < (int)sizeof(srv_ip) - 1) { srv_ip[n] = p[n]; n++; }
            srv_ip[n] = 0;
        }
        if (srv_ip[0]) { pv_set(85, "服务器已连上，IP=%s", srv_ip); break; }
        if (r && strncmp(r, "ERR", 3) == 0) { pv_set(-1, "服务器连不上：%s", r); goto done; }
        if ((i % 5) == 0) pv_set(60 + (i / 10), "服务器配网中… %d 秒", i);
    }
    if (!srv_ip[0]) { pv_set(-1, "服务器 150 秒内没回 IP（看回执）"); goto done; }

    /* 校准服务器地址（QQ API :3210 / 音乐流 :4533） */
    s_pv.step = 4;
    pv_set(95, "校准服务器地址…");
    char qhost[96], nhost[96];
    snprintf(qhost, sizeof(qhost), "http://%s:3210", srv_ip);
    snprintf(nhost, sizeof(nhost), "http://%s:4533", srv_ip);
    qqmusic_set_config(qhost, true);
    subsonic_set_config(nhost, NULL, NULL, NULL, 0);
    pv_set(100, "配网完成：ESP=%s 服务器=%s", net_mgr_ip_str(), srv_ip);

done:
    if (s_pv.pct < 0) s_pv.pct = 0;
    s_pv.running = 0;
    vTaskDelete(NULL);
}

esp_err_t usblink_provision_start(const char *ssid, const char *pass)
{
    if (ssid == NULL || ssid[0] == 0) return ESP_ERR_INVALID_ARG;
    if (s_pv.running) return ESP_ERR_INVALID_STATE;
    strlcpy(s_pv.ssid, ssid, sizeof(s_pv.ssid));
    strlcpy(s_pv.pass, pass ? pass : "", sizeof(s_pv.pass));
    s_pv.pct = 0;
    s_pv.running = 1;
    if (xTaskCreate(provision_task, "provision", 6144, NULL, 4, NULL) != pdPASS) {
        s_pv.running = 0;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool usblink_provision_status(int *pct, char *message, size_t len)
{
    if (pct) *pct = s_pv.pct;
    if (message) strlcpy(message, s_pv.msg, len);
    return s_pv.running != 0;
}