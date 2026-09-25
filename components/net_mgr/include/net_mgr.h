/*
 * 网络管理（WiFi STA）
 *
 * 凭据优先级：NVS(namespace "net", key "ssid"/"pass") → Kconfig 默认值。
 * 之所以先 NVS：M5 的网页配网会把新凭据写进 NVS，不应被编译期默认值覆盖。
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化 NVS + netif + WiFi（STA），不阻塞等待连接 */
esp_err_t net_mgr_init(void);

/* 阻塞等待拿到 IP；返回 true = 已连接 */
bool net_mgr_wait_ip(int timeout_ms);

bool        net_mgr_is_connected(void);
const char *net_mgr_ip_str(void);
const char *net_mgr_ssid(void);
int         net_mgr_rssi(void);

/* 手动重连（菜单项）：重连次数用完后 net_mgr 会彻底放弃，
 * 路由器重启/换地方之后得有个手动拉起来的入口，不然只能重启设备 */
void net_mgr_reconnect(void);

/* ---------------- M5 网页配网（全部不重启） ---------------- */

/* 换 STA 凭据：存 NVS 后立刻切换。密码传 NULL/空串 = 沿用已存的 */
esp_err_t net_mgr_set_sta(const char *ssid, const char *pass);

/* 设备自己的热点（APSTA 模式一直开着，没网时靠它进控制台）。
 * ssid/pass 传 NULL = 沿用已存的；pass 短于 8 位时按开放热点处理 */
esp_err_t   net_mgr_ap_apply(bool on, const char *ssid, const char *pass);
bool        net_mgr_ap_is_on(void);
const char *net_mgr_ap_ssid(void);
const char *net_mgr_ap_pass(void);

/* 热点扫描：⚠️ 必须异步（同步扫要 2~4 秒，在 httpd 任务里会把网页卡死）。
 * 用法：scan_start() → 轮询 scan_busy()，变 false 后 scan_count()/scan_get() 取结果。
 * 注意扫描期间 STA 会短暂断流（音频可能闪一下） */
esp_err_t net_mgr_scan_start(void);
bool      net_mgr_scan_busy(void);
int       net_mgr_scan_count(void);
bool      net_mgr_scan_get(int i, char *ssid, size_t ssid_len, int *rssi, const char **auth);

/* 放音期间关省电（消除 ~100ms 周期的接收尖峰，实测有效） */
void net_mgr_set_ps_lowpower(bool on);

#ifdef __cplusplus
}
#endif
