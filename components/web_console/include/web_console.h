/*
 * 网页控制台（ESP-IDF httpd + assets 分区里的单页 HTML）
 *
 * 设计取舍（都是用户确认过的）：
 *   - **不设鉴权**（用户要求："反正我在局域网里面"）。所以这个服务只该开在可信网络里。
 *   - 页面是单文件、零外部引用，gzip 后放 assets 分区，mmap 直接发给 socket（零拷贝）。
 *     AP 模式下手机没有到 Navidrome 的路由，所以封面必须由设备**代理**（/api/cover），
 *     顺带也不把 Navidrome 凭据暴露给浏览器。
 *   - 状态推送用 **1 秒轮询**，不做 SSE/WebSocket：设备是单核在放音频，
 *     长连接的心跳/重连反而更容易干扰播放，且手机休眠回来不用重连。
 *   - ⚠️ 任何耗时操作（WiFi 扫描、连 Navidrome 校验）都不能阻塞在 httpd 任务里：
 *     扫描走 net_mgr 的异步接口，Navidrome 校验虽然同步但有 10 秒超时且只发生在
 *     用户点"保存/测试"时（此时本来也没有音频请求）。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 起 httpd（80 端口）。要在 net_mgr_init / ui_start / player_init 之后调，
 * 因为它要读这些模块的状态。 */
esp_err_t web_console_start(void);

#ifdef __cplusplus
}
#endif
