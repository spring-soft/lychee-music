#include <string.h>
#include "net_mgr.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "net";

#define NVS_NS_NET "net"
#define BIT_CONNECTED BIT0
#define BIT_FAIL      BIT1

static EventGroupHandle_t s_evt;
static esp_netif_t       *s_sta_netif;
static esp_netif_t       *s_ap_netif;
static bool               s_started;
static char               s_ip[16] = "-";
static char               s_ssid[64];
static bool               s_connected;
static int                s_rssi = -127;
static int                s_retry;
static bool               s_ap_on;
static char               s_ap_ssid[33];
static char               s_ap_pass[65];

/* 扫描结果（异步填，别在 httpd 任务里同步扫） */
#define SCAN_MAX 20
typedef struct { char ssid[33]; int8_t rssi; uint8_t auth; } scan_item_t;
static scan_item_t s_scan[SCAN_MAX];
static volatile int  s_scan_n;          /* 0 = 没有结果；扫描中为 -1 */
static volatile bool s_scan_busy;

/* --- 小工具：NVS 读字符串带兜底 --- */
static void nvs_get_or(nvs_handle_t h, const char *key, char *dst, size_t len, const char *def)
{
    size_t l = len;
    dst[0] = 0;
    if (nvs_get_str(h, key, dst, &l) != ESP_OK || dst[0] == 0) {
        strlcpy(dst, def ? def : "", len);
    }
}

/* NVS 优先，Kconfig 兜底 */
static void load_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t h;
    ssid[0] = 0;
    pass[0] = 0;
    if (nvs_open(NVS_NS_NET, NVS_READONLY, &h) == ESP_OK) {
        size_t l = ssid_len;
        nvs_get_str(h, "ssid", ssid, &l);
        l = pass_len;
        nvs_get_str(h, "pass", pass, &l);
        nvs_close(h);
    }
    if (ssid[0] == 0) {
        strlcpy(ssid, CONFIG_APP_WIFI_SSID, ssid_len);
        strlcpy(pass, CONFIG_APP_WIFI_PASSWORD, pass_len);
        ESP_LOGI(TAG, "NVS 里没有凭据，用编译期默认值");
    } else {
        ESP_LOGI(TAG, "从 NVS 读到 WiFi 凭据");
    }
}

static esp_err_t nvs_set_str_kv(const char *key, const char *val)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_NET, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, key, val ? val : "");
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* 设备热点：默认 SSID 用 MAC 后两字节，默认密码 esp-music。
 * 没网的时候连它就能打开控制台（网页控制台本身不设鉴权，用户要求的）。 */
#define AP_PASS_DEFAULT "esp-music"

static void load_ap_config(void)
{
    nvs_handle_t h;
    char def_ssid[33];
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(def_ssid, sizeof(def_ssid), "ESP-Music-%02X%02X", mac[4], mac[5]);

    int ap_on = 1;
    if (nvs_open(NVS_NS_NET, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_or(h, "ap_ssid", s_ap_ssid, sizeof(s_ap_ssid), def_ssid);
        nvs_get_or(h, "ap_pass", s_ap_pass, sizeof(s_ap_pass), AP_PASS_DEFAULT);
        int8_t v = 1;
        if (nvs_get_i8(h, "ap_on", &v) == ESP_OK) ap_on = v;
        nvs_close(h);
    } else {
        strlcpy(s_ap_ssid, def_ssid, sizeof(s_ap_ssid));
        strlcpy(s_ap_pass, AP_PASS_DEFAULT, sizeof(s_ap_pass));
    }
    s_ap_on = (ap_on != 0);
}

static esp_err_t ap_apply(void)
{
    if (s_ap_netif == NULL) return ESP_ERR_INVALID_STATE;
    wifi_config_t ac = {0};
    strlcpy((char *)ac.ap.ssid, s_ap_ssid, sizeof(ac.ap.ssid));
    ac.ap.ssid_len = strlen(s_ap_ssid);
    ac.ap.channel  = 1;                     /* 跟着 STA 信道走的话得设 0；固定 1 更省心 */
    ac.ap.max_connection = 2;
    if (strlen(s_ap_pass) >= 8) {
        strlcpy((char *)ac.ap.password, s_ap_pass, sizeof(ac.ap.password));
        ac.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ac.ap.authmode = WIFI_AUTH_OPEN;    /* 密码短于 8 位 WPA2 不接受，干脆开放 */
    }
    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &ac);
    ESP_LOGW(TAG, "设备热点: %s SSID=\"%s\" 密码%s",
             s_ap_on ? "开" : "关", s_ap_ssid,
             ac.ap.authmode == WIFI_AUTH_OPEN ? "（无）" : "已设");
    return err;
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
            s_connected = false;
            s_ip[0] = 0;
            if (s_ap_on) {
                /* 只是掉了一下就又回来了（很常见），先补一刀省得扫的时候断太久 */
                ap_apply();
            }
            if (s_retry++ < 8) {
                ESP_LOGW(TAG, "断开（reason=%d），重连第 %d 次", d->reason, s_retry);
                esp_wifi_connect();
            } else {
                ESP_LOGE(TAG, "重连多次仍失败（reason=%d），停止重试", d->reason);
                xEventGroupSetBits(s_evt, BIT_FAIL);
            }
            break;
        }
        case WIFI_EVENT_SCAN_DONE: {
            uint16_t n = SCAN_MAX;
            wifi_ap_record_t *recs = heap_caps_malloc(sizeof(wifi_ap_record_t) * SCAN_MAX, MALLOC_CAP_SPIRAM);
            if (recs == NULL) { s_scan_busy = false; break; }
            /* ⚠️ 第二个参数是"最多要几条"，返回后会被改成实际条数 */
            if (esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
                if (n > SCAN_MAX) n = SCAN_MAX;
                /* ★ 按 SSID 去重，同名只留信号最强的那条。
                 *   实测用户家里是多个 AP 用同一个 SSID（mesh/中继），
                 *   不去重的话网页上会列出一串一模一样的名字（实测 9 条里 7 条同名），
                 *   用户根本没法选。 */
                int kept = 0;
                for (int i = 0; i < n; i++) {
                    const char *ssid = (char *)recs[i].ssid;
                    if (ssid[0] == 0) continue;          /* 隐藏 SSID 跳过，选了也没法连 */
                    int at = -1;
                    for (int k = 0; k < kept; k++) {
                        if (!strcmp(s_scan[k].ssid, ssid)) { at = k; break; }
                    }
                    if (at < 0) {
                        at = kept++;
                        strlcpy(s_scan[at].ssid, ssid, sizeof(s_scan[at].ssid));
                        s_scan[at].rssi = recs[i].rssi;
                        s_scan[at].auth = recs[i].authmode;
                    } else if (recs[i].rssi > s_scan[at].rssi) {
                        s_scan[at].rssi = recs[i].rssi;   /* 同名的更强的覆盖弱的 */
                        s_scan[at].auth = recs[i].authmode;
                    }
                }
                s_scan_n = kept;
                ESP_LOGI(TAG, "扫描完成：%u 个热点 → 按 SSID 去重后 %d 个", n, kept);
            } else {
                s_scan_n = 0;
                ESP_LOGW(TAG, "取扫描结果失败");
            }
            heap_caps_free(recs);
            s_scan_busy = false;
            break;
        }
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true;
        s_retry = 0;
        ESP_LOGI(TAG, "拿到 IP: %s", s_ip);
        /* 放音前就把省电关掉：默认 MIN_MODEM 会在 beacon 间打盹，
         * 造成 ~100ms 周期的接收延迟尖峰 → 音频断供（老实现实测的坑） */
        esp_wifi_set_ps(WIFI_PS_NONE);
        xEventGroupSetBits(s_evt, BIT_CONNECTED);
    }
}

esp_err_t net_mgr_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要重建 (%s)", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_evt = xEventGroupCreate();

    char pass[64];
    load_credentials(s_ssid, sizeof(s_ssid), pass, sizeof(pass));

    s_sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_sta_netif, CONFIG_APP_WIFI_HOSTNAME);
    /* 热点也一起建：初次连不上网时唯一能进控制台的入口。
     * 一开始就 APSTA，避免运行中切模式（切模式会重启 WiFi、断音频）。 */
    s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                       on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                       on_wifi_event, NULL, NULL));

    load_ap_config();
    esp_wifi_set_mode(s_ap_on ? WIFI_MODE_APSTA : WIFI_MODE_STA);
    ap_apply();

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, s_ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    /* 放宽到 WPA/WPA2 混合：实测只要求 WPA2 时会 AUTH_FAIL(reason=202) 重试好几次 */
    wc.sta.threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    wc.sta.pmf_cfg.capable = true;
    /* ★★ 必须显式指定 —— 默认（全 0）是 WIFI_FAST_SCAN，**扫到第一个同名 AP 就连**，
     *    而 sort_method（按信号排序）只在 ALL_CHANNEL_SCAN 下才生效。
     *    家里有多台同名路由/ mesh 节点时，设备就会随机连上一个弱的。
     *    实测事故（2026-09-25）：设备连到 -70 dBm 的弱节点，而同一个 SSID 旁边就有
     *    -22 dBm 的强节点 → 取流速率从 23KB/s 掉到 9KB/s（需要 24）→ 播放每几秒卡一下。
     *    代价：扫完所有信道再连，开机多花约 1 秒 —— 换一个正确的接入点很值。 */
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);
    s_started = true;

    ESP_LOGI(TAG, "STA 已启动，SSID=\"%s\" 密码长度=%u", s_ssid, (unsigned)strlen(pass));
    if (s_ssid[0] == 0) {
        ESP_LOGW(TAG, "没有 WiFi 凭据（NVS 和 Kconfig 都为空）—— 连设备热点配网");
    }
    return ESP_OK;
}

bool net_mgr_wait_ip(int timeout_ms)
{
    if (!s_started) return false;
    EventBits_t b = xEventGroupWaitBits(s_evt, BIT_CONNECTED | BIT_FAIL, pdFALSE, pdFALSE,
                                        pdMS_TO_TICKS(timeout_ms));
    if (b & BIT_CONNECTED) {
        wifi_ap_record_t ap = {0};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            s_rssi = ap.rssi;
            ESP_LOGI(TAG, "已连接: SSID=\"%s\" IP=%s RSSI=%d dBm 信道=%d",
                     (char *)ap.ssid, s_ip, ap.rssi, ap.primary);
        }
        return true;
    }
    ESP_LOGE(TAG, "等 IP 超时（%d ms）", timeout_ms);
    return false;
}

bool        net_mgr_is_connected(void) { return s_connected; }
const char *net_mgr_ip_str(void) { return s_ip; }
const char *net_mgr_ssid(void) { return s_ssid; }
int         net_mgr_rssi(void) { return s_rssi; }

void net_mgr_set_ps_lowpower(bool on)
{
    esp_wifi_set_ps(on ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
    ESP_LOGD(TAG, "WiFi 省电: %s", on ? "开(MIN_MODEM)" : "关(PS_NONE)");
}

void net_mgr_reconnect(void)
{
    if (!s_started) return;
    /* 自动重连在 8 次失败后就放弃了（换地方、路由器重启都会触发），
     * 那时候没有这个入口就只能重启设备 */
    s_retry = 0;
    xEventGroupClearBits(s_evt, BIT_FAIL);
    ESP_LOGW(TAG, "手动重连（SSID=\"%s\"）", s_ssid);
    esp_wifi_disconnect();
    esp_wifi_connect();
}

/* ============================================================ M5 网页配网 */

esp_err_t net_mgr_set_sta(const char *ssid, const char *pass)
{
    if (!s_started || ssid == NULL || ssid[0] == 0) return ESP_ERR_INVALID_ARG;
    if (strlen(ssid) > 32) return ESP_ERR_INVALID_ARG;

    /* 先落盘再切换：切过去万一失败，重启后还是连新网络 */
    nvs_set_str_kv("ssid", ssid);
    if (pass && pass[0]) nvs_set_str_kv("pass", pass);

    char stored_pass[65] = {0};
    nvs_handle_t h;
    if (nvs_open(NVS_NS_NET, NVS_READONLY, &h) == ESP_OK) {
        size_t l = sizeof(stored_pass);
        nvs_get_str(h, "pass", stored_pass, &l);
        nvs_close(h);
    }
    strlcpy(s_ssid, ssid, sizeof(s_ssid));

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, stored_pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    wc.sta.pmf_cfg.capable = true;
    /* 同上：按信号选最强的那个同名 AP（mesh 环境下不加这个会连上弱的那个）*/
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    s_retry = 0;
    s_connected = false;
    xEventGroupClearBits(s_evt, BIT_CONNECTED | BIT_FAIL);
    esp_wifi_disconnect();
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    esp_err_t err = esp_wifi_connect();
    ESP_LOGW(TAG, "切换到新 WiFi: SSID=\"%s\"（密码 %u 位，不重启）",
             ssid, (unsigned)strlen(stored_pass));
    return err;
}

bool        net_mgr_ap_is_on(void)   { return s_ap_on; }
const char *net_mgr_ap_ssid(void)    { return s_ap_ssid; }
const char *net_mgr_ap_pass(void)    { return s_ap_pass; }

esp_err_t net_mgr_ap_apply(bool on, const char *ssid, const char *pass)
{
    if (!s_started) return ESP_ERR_INVALID_STATE;
    if (ssid && ssid[0]) {
        strlcpy(s_ap_ssid, ssid, sizeof(s_ap_ssid));
        nvs_set_str_kv("ap_ssid", s_ap_ssid);
    }
    if (pass) {
        strlcpy(s_ap_pass, pass, sizeof(s_ap_pass));
        nvs_set_str_kv("ap_pass", s_ap_pass);
    }
    bool was = s_ap_on;
    s_ap_on = on;
    nvs_set_str_kv("ap_on", on ? "1" : "0");
    /* ⚠️ 开关热点要切 WiFi 模式，这会重启 WiFi —— 所以只在真的变了时才切 */
    if (was != on) {
        ESP_LOGW(TAG, "热点%s：切换 WiFi 模式（STA 会短暂断开）", on ? "开" : "关");
        esp_wifi_set_mode(on ? WIFI_MODE_APSTA : WIFI_MODE_STA);
        s_retry = 0;
    }
    return ap_apply();
}

esp_err_t net_mgr_scan_start(void)
{
    if (!s_started) return ESP_ERR_INVALID_STATE;
    if (s_scan_busy) return ESP_ERR_INVALID_STATE;     /* 已经在扫了，别叠 */
    s_scan_busy = true;
    s_scan_n = -1;
    /* ⚠️ 这里【必须】用非阻塞形式（第二参数 false）：同步扫要 2~4 秒，
     *    发生在 httpd 任务里会直接把网页请求卡死，方案里专门标了这条。 */
    esp_err_t err = esp_wifi_scan_start(NULL, false);
    if (err != ESP_OK) {
        s_scan_busy = false;
        s_scan_n = 0;
        ESP_LOGE(TAG, "发起扫描失败: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "开始扫描热点（异步）");
    return ESP_OK;
}

bool net_mgr_scan_busy(void) { return s_scan_busy; }

int net_mgr_scan_count(void)
{
    return (s_scan_busy || s_scan_n < 0) ? 0 : s_scan_n;
}

bool net_mgr_scan_get(int i, char *ssid, size_t ssid_len, int *rssi, const char **auth)
{
    if (s_scan_busy || i < 0 || i >= s_scan_n) return false;
    if (ssid) strlcpy(ssid, s_scan[i].ssid, ssid_len);
    if (rssi) *rssi = s_scan[i].rssi;
    if (auth) {
        switch (s_scan[i].auth) {
            case WIFI_AUTH_OPEN:            *auth = "OPEN";   break;
            case WIFI_AUTH_WEP:             *auth = "WEP";    break;
            case WIFI_AUTH_WPA_PSK:         *auth = "WPA";    break;
            case WIFI_AUTH_WPA2_PSK:        *auth = "WPA2";   break;
            case WIFI_AUTH_WPA_WPA2_PSK:    *auth = "WPA/WPA2"; break;
            case WIFI_AUTH_WPA3_PSK:        *auth = "WPA3";   break;
            case WIFI_AUTH_WPA2_WPA3_PSK:   *auth = "WPA2/WPA3"; break;
            default:                        *auth = "?";      break;
        }
    }
    return true;
}
