/*
 * usblink —— 板子与服务器之间的**有线串口指令通道**（走烧录口那根 USB 线）。
 *
 * ============================================================================
 * 为什么做这个
 * ============================================================================
 * 场景：没有可用 WiFi 的时候，得有人把局面救回来 —— 让服务器开个热点，
 * 手机连上去就能访问控制台配网。可是没网的时候板子和服务器也互相够不着。
 * 板子插在服务器上的那根 USB 线（CH343 串口）就是最后一条可用通道。
 *
 * ============================================================================
 * 协议（一行一条 UTF-8，\n 结尾）
 * ============================================================================
 *   服务器 → 板子   ESP<指令...
 *   板子 → 服务器   ESP>指令...
 *
 * 例：
 *   服务器: ESP<OK PONG      （对 PING 的回执）
 *   板子  : ESP>HOTSPOT      （让服务器开热点）
 *
 * ⚠️ 这条串口**同时是日志口**（UART0 控制台）。所以：
 *   · 我们只认以 "ESP<" 开头的**输入行**，其它输入忽略；
 *   · 我们自己发出去的行一定带 "ESP>" 前缀，服务器那边同样按前缀过滤。
 *   这样日志和指令混在一条线上也不会互相误解。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 启动指令通道（在 UART0 上装接收驱动 + 起任务）。可重复调用。 */
esp_err_t usblink_init(void);

/* 串口驱动装好了没有 */
bool usblink_ready(void);

/* 给服务器发一条指令（自动补 "ESP>" 前缀和换行）。 */
esp_err_t usblink_send(const char *cmd);

/* 最近一次服务器回执（已去掉 ESP< 前缀；空串 = 还没有） */
const char *usblink_last_reply(void);

/* 最近一次我们发出去的指令（给网页显示"上次点了什么/结果"） */
const char *usblink_last_cmd(void);

uint32_t usblink_rx_lines(void);   /* 收到多少条来自服务器的行 */
uint32_t usblink_tx_lines(void);   /* 发出去多少条指令 */

/* ============================================================ 一键配网（流程跑在设备上）
 *
 * ★ 为什么不能放在网页里跑：ESP 一连上新 WiFi，**AP 会切信道、断 1~3 秒**，
 *   网页的 fetch 立刻全断（Failed to fetch）—— 流程就死在半路了，刷新也没用。
 *   所以流程做成**设备上的后台任务**：页面只负责"启动"和"看进度"，
 *   AP 断了、页面刷新了都无所谓，任务照跑；刷新后一查状态就能看到进度。
 */
esp_err_t usblink_provision_start(const char *ssid, const char *pass);
/* 读当前进度：返回 true = 正在跑；pct/message 是给网页显示的内容 */
bool      usblink_provision_status(int *pct, char *message, size_t len);

#ifdef __cplusplus
}
#endif
