/*
 * player_bt —— 蓝牙音箱（服务器侧）实现
 *
 * 数据流和 player_remote 一样：
 *    UI/按键 → player_bt_xxx()（非阻塞，投命令）
 *            → player 任务 → player_bt_task_xxx()（阻塞 HTTP）
 *            → PSRAM 里的列表快照 → UI 定时刷新读取
 *
 * 为什么要这么绕：这些 HTTP 一次要 1~8 秒（扫描期间更要等），
 * UI 任务栈只有几 KB，直接发 HTTP 会栈溢出/卡住画面（项目里已经踩过）。
 */
#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include "sdkconfig.h"
#include "qqmusic.h"
#include "player.h"
#include "player_bt.h"

static const char *TAG = "player.bt";

#define HTTP_TIMEOUT_MS  8000
#define BODY_MAX         (16 * 1024)   /* /v1/bt/devices 只有十几条，16KB 绰绰有余 */

/* 一台音箱的快照。字符串短一点：MAC 17 字节，名字几十字节。 */
typedef struct {
    char addr[24];
    char name[48];
    bool connected;
} bt_dev_t;

static bt_dev_t         *s_devs;         /* PSRAM，PLAYER_BT_LIST_MAX 条 */
static int               s_n;
static SemaphoreHandle_t s_lock;

/* 连接结果：0=空闲 / 1=正在连 / 2=成功 / 3=失败。
 * UI 用 player_bt_connect_take() 取一次（取完清零），所以只提示一次。 */
static int  s_conn_state;
static char s_conn_name[48];

/* ============================================================ 小工具 */

static void base_url(char *out, size_t len)
{
    const char *h = qqmusic_host();
    if (h == NULL || h[0] == 0) h = CONFIG_APP_QQ_HOST;
    strlcpy(out, h, len);
    size_t n = strlen(out);
    while (n > 0 && out[n - 1] == '/') out[--n] = 0;
}

static void jstr(const cJSON *o, const char *k, char *buf, size_t len)
{
    if (buf == NULL || len == 0) return;
    buf[0] = 0;
    if (!cJSON_IsObject(o)) return;
    const cJSON *it = cJSON_GetObjectItem(o, k);
    if (cJSON_IsString(it) && it->valuestring) strlcpy(buf, it->valuestring, len);
}

/*
 * 发一个请求，body 放 PSRAM（调用方 heap_caps_free）。
 * 非 200 一律当失败 —— 服务器对"连不上音箱"明确回 502，正好能和"服务器没开"区分开。
 */
static esp_err_t http_do(esp_http_client_method_t method, const char *path,
                         char **body_out, size_t *len_out)
{
    if (body_out) *body_out = NULL;
    if (len_out) *len_out = 0;

    char base[192], url[384];
    base_url(base, sizeof(base));
    snprintf(url, sizeof(url), "%s%s", base, path);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 1024,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (cli == NULL) return ESP_FAIL;
    esp_http_client_set_method(cli, method);

    esp_err_t err = esp_http_client_open(cli, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "连不上服务器 %s: %s", url, esp_err_to_name(err));
        esp_http_client_cleanup(cli);
        return err;
    }
    int content_len = esp_http_client_fetch_headers(cli);
    int status = esp_http_client_get_status_code(cli);
    size_t cap = (content_len > 0 && (size_t)content_len < BODY_MAX) ? (size_t)content_len + 1
                                                                    : 1024;
    char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        esp_http_client_close(cli);
        esp_http_client_cleanup(cli);
        return ESP_ERR_NO_MEM;
    }
    size_t used = 0;
    while (true) {
        if (used + 1 >= cap) {
            if (cap >= BODY_MAX) break;
            size_t ncap = cap * 2 > BODY_MAX ? BODY_MAX : cap * 2;
            char *nb = heap_caps_malloc(ncap, MALLOC_CAP_SPIRAM);
            if (nb == NULL) break;
            memcpy(nb, buf, used);
            heap_caps_free(buf);
            buf = nb;
            cap = ncap;
        }
        int n = esp_http_client_read(cli, buf + used, cap - 1 - used);
        if (n <= 0) break;
        used += n;
    }
    buf[used] = 0;
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);

    if (status != 200) {
        /* 502 一般就是"音箱连不上"；把服务器给的 detail 打出来，方便定位 */
        ESP_LOGW(TAG, "服务器 %d：%s %.160s", status,
                 method == HTTP_METHOD_POST ? "POST" : "GET", buf);
        heap_caps_free(buf);
        return ESP_FAIL;
    }
    if (body_out) *body_out = buf;
    else          heap_caps_free(buf);
    if (len_out) *len_out = used;
    return ESP_OK;
}

static esp_err_t http_post(const char *path) { return http_do(HTTP_METHOD_POST, path, NULL, NULL); }

/* ============================================================ 生命周期 */

esp_err_t player_bt_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) return ESP_FAIL;
    }
    if (s_devs == NULL) {
        s_devs = heap_caps_calloc(PLAYER_BT_LIST_MAX, sizeof(bt_dev_t), MALLOC_CAP_SPIRAM);
        if (s_devs == NULL) {
            ESP_LOGE(TAG, "蓝牙列表分配失败（%u 字节，PSRAM 不够？）",
                     (unsigned)(PLAYER_BT_LIST_MAX * sizeof(bt_dev_t)));
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

/* ============================================================ 列表快照（给 UI） */

int player_bt_count(void)
{
    if (s_lock == NULL) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_n;
    xSemaphoreGive(s_lock);
    return n;
}

bool player_bt_get(int i, char *addr, size_t alen, char *name, size_t nlen, bool *connected)
{
    if (s_lock == NULL) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = (i >= 0 && i < s_n);
    if (ok) {
        if (addr && alen) strlcpy(addr, s_devs[i].addr, alen);
        if (name && nlen) strlcpy(name, s_devs[i].name, nlen);
        if (connected) *connected = s_devs[i].connected;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

bool player_bt_connect_take(bool *ok, char *name, size_t nlen)
{
    if (s_lock == NULL) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool have = (s_conn_state == 2 || s_conn_state == 3);
    if (have) {
        if (ok) *ok = (s_conn_state == 2);
        if (name && nlen) strlcpy(name, s_conn_name, nlen);
        s_conn_state = 0;      /* 只报一次 */
        s_conn_name[0] = 0;
    }
    xSemaphoreGive(s_lock);
    return have;
}

/* ============================================================ 任务侧：扫描 / 刷新 / 连接 */

void player_bt_task_scan(void)
{
    ESP_LOGW(TAG, "让服务器开始扫描蓝牙设备（8 秒）");
    if (http_post("/v1/bt/scan?seconds=8") != ESP_OK) {
        ESP_LOGW(TAG, "扫描请求失败 —— 列表保持上一次的结果");
    }
}

/* 解析 /v1/bt/devices。只收"有名字"的：没有名字的基本是耳机/手环之类，
 * 混进列表只会让用户多翻几行，而且选错了也连不上。
 * 返回条数，写进 out（调用方保证有 PLAYER_BT_LIST_MAX 容量）。 */
static int parse_devices(const char *json, bt_dev_t *out, int max)
{
    cJSON *r = cJSON_Parse(json);
    if (r == NULL) return 0;

    int n = 0;
    const cJSON *items = cJSON_GetObjectItem(r, "items");
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, items) {
        if (n >= max) break;
        char name[48], addr[24];
        jstr(it, "addr", addr, sizeof(addr));
        jstr(it, "name", name, sizeof(name));
        if (addr[0] == 0 || name[0] == 0) continue;
        bt_dev_t *d = &out[n++];
        memset(d, 0, sizeof(*d));
        strlcpy(d->addr, addr, sizeof(d->addr));
        strlcpy(d->name, name, sizeof(d->name));
        d->connected = cJSON_IsTrue(cJSON_GetObjectItem(it, "connected"));
    }
    cJSON_Delete(r);
    return n;
}

void player_bt_task_refresh(void)
{
    char *body = NULL;
    size_t len = 0;
    if (http_do(HTTP_METHOD_GET, "/v1/bt/devices", &body, &len) != ESP_OK) {
        /* 连不上就保留旧列表（比清空好，用户还能看到上次那台） */
        return;
    }

    bt_dev_t *tmp = heap_caps_calloc(PLAYER_BT_LIST_MAX, sizeof(bt_dev_t), MALLOC_CAP_SPIRAM);
    if (tmp == NULL) {
        heap_caps_free(body);
        ESP_LOGE(TAG, "刷新列表：临时缓冲分配失败");
        return;
    }
    int n = parse_devices(body, tmp, PLAYER_BT_LIST_MAX);
    heap_caps_free(body);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int old = s_n;
    if (n > 0) memcpy(s_devs, tmp, (size_t)n * sizeof(bt_dev_t));
    s_n = n;
    xSemaphoreGive(s_lock);
    heap_caps_free(tmp);

    if (n != old) ESP_LOGI(TAG, "蓝牙列表：%d 台（原来 %d 台）", n, old);
}

void player_bt_task_connect(const char *addr)
{
    if (addr == NULL || addr[0] == 0) return;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_conn_state = 1;          /* 正在连 */
    s_conn_name[0] = 0;
    xSemaphoreGive(s_lock);

    /* MAC 里只有冒号和十六进制，直接放 query 是安全的（不需要 URL 编码） */
    char path[128];
    snprintf(path, sizeof(path), "/v1/bt/connect?addr=%.17s", addr);
    ESP_LOGW(TAG, "连接蓝牙音箱 %s", addr);
    esp_err_t err = http_post(path);

    /* 成功时把列表里的名字找出来给 UI 提示（找不到就用 MAC） */
    bool ok = (err == ESP_OK);
    char name[48];
    strlcpy(name, addr, sizeof(name));
    if (ok) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int i = 0; i < s_n; i++) {
            if (strcmp(s_devs[i].addr, addr) == 0) {
                strlcpy(name, s_devs[i].name, sizeof(name));
                s_devs[i].connected = true;   /* 先乐观标记，下一轮刷新以服务器为准 */
                break;
            }
        }
        xSemaphoreGive(s_lock);
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_conn_state = ok ? 2 : 3;
    strlcpy(s_conn_name, name, sizeof(s_conn_name));
    xSemaphoreGive(s_lock);

    if (ok) ESP_LOGW(TAG, "已连接蓝牙音箱 %s", name);
    else    ESP_LOGE(TAG, "连接蓝牙音箱 %s 失败（服务器回 502 通常是音箱没开/不在配对模式）", addr);
}

/* ============================================================ 切服务器音频目标 */

esp_err_t player_bt_audio_target(bool bt)
{
    char path[64];
    snprintf(path, sizeof(path), "/v1/player/audio?target=%s", bt ? "bt" : "dac");
    esp_err_t err = http_post(path);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "切服务器音频到 %s 失败", bt ? "蓝牙" : "USB DAC");
    } else {
        ESP_LOGW(TAG, "服务器音频已切到 %s", bt ? "蓝牙" : "USB DAC");
    }
    return err;
}
