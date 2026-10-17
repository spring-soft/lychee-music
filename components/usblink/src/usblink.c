#include "usblink.h"

#include <string.h>
#include <stdio.h>
#include <stdatomic.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "esp_log.h"

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
