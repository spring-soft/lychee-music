#include <string.h>
#include "settings.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "settings";

#define NVS_NS_CFG    "cfg"
#define LAZY_MAX      8                       /* 同时待落盘的键数上限 */
#define LAZY_DELAY_MS 2000                    /* 静置多久才落盘 */

typedef struct {
    char    key[SETTINGS_MAX_KEY];
    int     val;
    bool    used;
    bool    dirty;
    int64_t due_ms;
} lazy_t;

static SemaphoreHandle_t s_lock;
static lazy_t            s_lazy[LAZY_MAX];
static int               s_commits;
static bool              s_ready;

esp_err_t settings_init(void)
{
    if (s_ready) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;

    /* nvs_flash_init() 由 net_mgr_init() 负责（它先跑）。这里只开命名空间。 */
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_CFG, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "打开 NVS 命名空间 \"%s\" 失败: %s", NVS_NS_CFG, esp_err_to_name(err));
        return err;
    }
    nvs_close(h);
    s_ready = true;
    ESP_LOGI(TAG, "就绪（命名空间 \"%s\"，去抖 %d ms）", NVS_NS_CFG, LAZY_DELAY_MS);
    return ESP_OK;
}

int settings_get_int(const char *key, int def)
{
    if (!s_ready) return def;
    nvs_handle_t h;
    if (nvs_open(NVS_NS_CFG, NVS_READONLY, &h) != ESP_OK) return def;
    int32_t v = def;
    if (nvs_get_i32(h, key, &v) != ESP_OK) v = def;
    nvs_close(h);
    return (int)v;
}

esp_err_t settings_set_int(const char *key, int val)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_CFG, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_i32(h, key, (int32_t)val);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_commits++;
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "%s = %d 已保存", key, val);
    } else {
        ESP_LOGE(TAG, "%s = %d 保存失败: %s", key, val, esp_err_to_name(err));
    }
    return err;
}

void settings_set_int_lazy(const char *key, int val)
{
    if (!s_ready || key == NULL || strlen(key) >= SETTINGS_MAX_KEY) return;
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    lazy_t *slot = NULL;
    for (int i = 0; i < LAZY_MAX; i++) {
        if (s_lazy[i].used && !strcmp(s_lazy[i].key, key)) { slot = &s_lazy[i]; break; }
    }
    if (slot == NULL) {
        for (int i = 0; i < LAZY_MAX; i++) {
            if (!s_lazy[i].used) { slot = &s_lazy[i]; break; }
        }
    }
    if (slot == NULL) {
        /* 槽位用完了：少见（只有几个键会频繁改），退化成立即落盘而不是丢数据 */
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "去抖槽位满，%s 改为立即落盘", key);
        settings_set_int(key, val);
        return;
    }
    strlcpy(slot->key, key, sizeof(slot->key));
    slot->val    = val;
    slot->used   = true;
    slot->dirty  = true;
    slot->due_ms = esp_timer_get_time() / 1000 + LAZY_DELAY_MS;
    xSemaphoreGive(s_lock);
}

void settings_tick(void)
{
    if (!s_ready || s_lock == NULL) return;
    int64_t now = esp_timer_get_time() / 1000;

    /* 先把到点的收集出来（不要持锁做 NVS 写，那会阻塞别的东西） */
    struct { char key[SETTINGS_MAX_KEY]; int val; } flush[LAZY_MAX];
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < LAZY_MAX; i++) {
        if (s_lazy[i].used && s_lazy[i].dirty && now >= s_lazy[i].due_ms) {
            strlcpy(flush[n].key, s_lazy[i].key, sizeof(flush[n].key));
            flush[n].val = s_lazy[i].val;
            s_lazy[i].dirty = false;
            n++;
        }
    }
    xSemaphoreGive(s_lock);

    for (int i = 0; i < n; i++) {
        settings_set_int(flush[i].key, flush[i].val);
    }
}

/* ---- 字符串 ---- */

bool settings_get_str(const char *key, char *out, size_t len, const char *def)
{
    out[0] = 0;
    if (!s_ready) {
        strlcpy(out, def ? def : "", len);
        return false;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS_CFG, NVS_READONLY, &h) != ESP_OK) {
        strlcpy(out, def ? def : "", len);
        return false;
    }
    size_t l = len;
    bool ok = (nvs_get_str(h, key, out, &l) == ESP_OK && out[0]);
    nvs_close(h);
    if (!ok) strlcpy(out, def ? def : "", len);
    return ok;
}

esp_err_t settings_set_str(const char *key, const char *val)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_CFG, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, key, val ? val : "");
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_commits++;
        xSemaphoreGive(s_lock);
        /* ⚠️ 别把值打进日志：这个接口会存 WiFi 密码和 Navidrome 密码 */
        ESP_LOGI(TAG, "%s 已保存（%u 字节）", key, (unsigned)(val ? strlen(val) : 0));
    } else {
        ESP_LOGE(TAG, "%s 保存失败: %s", key, esp_err_to_name(err));
    }
    return err;
}

int settings_commit_count(void) { return s_commits; }

int settings_pending_count(void)
{
    if (!s_ready || s_lock == NULL) return 0;
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < LAZY_MAX; i++) {
        if (s_lazy[i].used && s_lazy[i].dirty) n++;
    }
    xSemaphoreGive(s_lock);
    return n;
}
