#include <string.h>
#include "assets.h"
#include "esp_log.h"
#include "esp_partition.h"

static const char *TAG = "assets";

#define ASSET_MAGIC   0x53554D41u   /* "AMUS" 小端 */
#define HDR_SZ        16
#define ENTRY_SZ      44

typedef struct __attribute__((packed)) {
    char     name[28];
    uint32_t off;
    uint32_t size;
    uint16_t w, h;
    uint8_t  fmt;
    uint8_t  resv[3];
} asset_entry_t;

static const uint8_t *s_base;
static uint16_t       s_count;
static uint32_t       s_total;

static void read_entry(int id, asset_entry_t *e)
{
    memcpy(e, s_base + HDR_SZ + (size_t)id * ENTRY_SZ, ENTRY_SZ);
}

esp_err_t assets_init(void)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "assets");
    if (part == NULL) {
        ESP_LOGE(TAG, "找不到 assets 分区（检查 partitions.csv）");
        return ESP_ERR_NOT_FOUND;
    }

    const void *ptr = NULL;
    esp_partition_mmap_handle_t h;
    /* 整块映射（2MB）。mmap 要求 offset/size 64KB 对齐 —— 分区表里就是对齐的 */
    esp_err_t err = esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &ptr, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mmap 失败: %s", esp_err_to_name(err));
        return err;
    }
    s_base = (const uint8_t *)ptr;

    uint32_t magic;
    uint16_t ver, count;
    memcpy(&magic, s_base, 4);
    memcpy(&ver, s_base + 4, 2);
    memcpy(&count, s_base + 6, 2);
    memcpy(&s_total, s_base + 8, 4);

    if (magic != ASSET_MAGIC) {
        ESP_LOGE(TAG, "assets 分区没有素材（magic=0x%08" PRIx32 "，期望 0x%08" PRIx32 "）—— "
                      "是不是没烧 assets.bin？", magic, ASSET_MAGIC);
        s_base = NULL;
        return ESP_ERR_INVALID_STATE;
    }
    s_count = count;
    ESP_LOGI(TAG, "assets 就绪: v%u, %u 个素材, 数据 %u KB @ %p（mmap，零拷贝）",
             ver, count, (unsigned)(s_total / 1024), s_base);
    return ESP_OK;
}

bool assets_get(asset_id_t id, asset_t *out)
{
    if (s_base == NULL || id >= s_count) {
        return false;
    }
    asset_entry_t e;
    read_entry((int)id, &e);
    out->data = s_base + HDR_SZ + (size_t)s_count * ENTRY_SZ + e.off;
    out->size = e.size;
    out->w    = e.w;
    out->h    = e.h;
    out->fmt  = e.fmt;
    return true;
}

bool assets_get_by_name(const char *name, asset_t *out)
{
    if (s_base == NULL || name == NULL) return false;
    for (int i = 0; i < s_count; i++) {
        asset_entry_t e;
        read_entry(i, &e);
        if (strncmp(e.name, name, sizeof(e.name)) == 0) {
            out->data = s_base + HDR_SZ + (size_t)s_count * ENTRY_SZ + e.off;
            out->size = e.size;
            out->w    = e.w;
            out->h    = e.h;
            out->fmt  = e.fmt;
            return true;
        }
    }
    ESP_LOGW(TAG, "没有名为 \"%s\" 的素材", name);
    return false;
}

void assets_dump(void)
{
    for (int i = 0; i < s_count; i++) {
        asset_t a;
        if (!assets_get((asset_id_t)i, &a)) continue;
        ESP_LOGI(TAG, "  [%2d] %-26s fmt=%u %7u B", i, asset_names[i], a.fmt, (unsigned)a.size);
    }
}
