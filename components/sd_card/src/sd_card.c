/*
 * sd_card 实现 —— 见 sd_card.h 的硬件与"懒挂载"说明。
 *
 * 依赖 IDF 自带的组件，不引入任何托管组件：
 *   esp_driver_sdspi  → driver/sdspi_host.h（sdspi_host_init_device 等）
 *   sdmmc             → sdmmc_cmd.h（sdmmc_card_t）
 *   fatfs             → vfs/esp_vfs_fat.h（esp_vfs_fat_sdspi_mount / info / unmount）
 * main/idf_component.yml 与 dependencies.lock 都不需要改。
 */
#include <inttypes.h>
#include "sd_card.h"
#include "board.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"

static const char *TAG = "sd_card";

#if BOARD_HAS_TF

static sdmmc_card_t *s_card;
static bool          s_mounted;
static int           s_mount_count;
static esp_err_t     s_last_err = ESP_OK;

esp_err_t sd_card_init(void)
{
    /* CD 引脚：输入 + 上拉（卡座触点对地闭合 = 有卡，所以低电平表示有卡）。
     * ⚠️ 这里刻意【不用】GPIO 中断：热插拔的自动挂载/卸载这一轮不做
     *    （列为真机后续项），这里只提供一个"值不值得去挂载"的快速判断。 */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_PIN_TF_CD,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "卡检测引脚(GPIO%d)配置失败: %s", BOARD_PIN_TF_CD, esp_err_to_name(err));
        s_last_err = err;
        return err;
    }
    ESP_LOGI(TAG, "TF 卡就绪：CS=%d MISO=%d CD=%d ｜ 当前%s卡",
             BOARD_PIN_TF_CS, BOARD_PIN_TF_MISO, BOARD_PIN_TF_CD,
             sd_card_present() ? "有" : "无");
    return ESP_OK;
}

bool sd_card_present(void)
{
    return gpio_get_level(BOARD_PIN_TF_CD) == 0;
}

esp_err_t sd_card_mount(void)
{
    if (s_mounted) return ESP_OK;

    if (!sd_card_present()) {
        ESP_LOGW(TAG, "没检测到 TF 卡（CD=GPIO%d 为高）", BOARD_PIN_TF_CD);
        s_last_err = ESP_ERR_NOT_FOUND;
        return s_last_err;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();   /* .slot = SPI2_HOST，与屏幕同一条总线 */
    host.max_freq_khz = 20000;                  /* 20MHz：SPI 模式的稳妥速度 */

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = SPI2_HOST;                   /* 显式写明：就是屏幕那条 */
    slot.gpio_cs = BOARD_PIN_TF_CS;             /* ⚠️ 默认是 GPIO13，必须改 */
    slot.gpio_cd = BOARD_PIN_TF_CD;

    esp_vfs_fat_mount_config_t mcfg = {
        /* ⚠️⚠️ 永远 false。卡上是用户的音乐，挂不上只能报错 —— 这一行是整份代码里
         *    最不能改的一行（改成 true 就是"一次误挂 = 用户整卡数据没了"）。 */
        .format_if_mount_failed = false,
        .max_files              = 4,
        .allocation_unit_size   = 16 * 1024,
        .disk_status_check_enable = false,
    };

    esp_err_t err = esp_vfs_fat_sdspi_mount(SD_ROOT, &host, &slot, &mcfg, &s_card);
    if (err != ESP_OK) {
        /* 常见原因都写进日志，免得现场只能看到一串 errno：
         *   ESP_ERR_INVALID_STATE 总线没初始化（理论上不该发生，屏幕先建好了）
         *   ESP_ERR_NOT_FOUND / TIMEOUT 接触不良、卡是 exFAT/NTFS
         *   ESP_FAIL 卡被写保护或文件系统损坏（我们不会去修，只报错） */
        ESP_LOGE(TAG, "挂载 %s 失败: %s（总线没初始化 / 没卡 / 接触不良 / 不是 FAT 格式）",
                 SD_ROOT, esp_err_to_name(err));
        s_card = NULL;
        s_last_err = err;
        return err;
    }

    s_mounted = true;
    s_mount_count++;
    s_last_err = ESP_OK;

    uint64_t total = 0, free_b = 0;
    if (esp_vfs_fat_info(SD_ROOT, &total, &free_b) == ESP_OK) {
        ESP_LOGW(TAG, "TF 卡已挂载到 %s：%u MB 可用 / %u MB 总计",
                 SD_ROOT, (unsigned)(free_b / (1024 * 1024)), (unsigned)(total / (1024 * 1024)));
    } else {
        ESP_LOGW(TAG, "TF 卡已挂载到 %s（容量信息读不到）", SD_ROOT);
    }
    return ESP_OK;
}

esp_err_t sd_card_unmount(void)
{
    if (!s_mounted || s_card == NULL) return ESP_OK;

    esp_err_t err = esp_vfs_fat_sdcard_unmount(SD_ROOT, s_card);
    s_card    = NULL;
    s_mounted = false;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "卸载 %s 失败: %s", SD_ROOT, esp_err_to_name(err));
        s_last_err = err;
        return err;
    }
    /* 卸载的意义就是把 SDSPI/SDMMC 的内部缓冲还给内部堆 —— 在线模式下
     * 内部 SRAM 只有 10~15KB，这份内存得还回来。 */
    ESP_LOGI(TAG, "TF 卡已卸载（%s），驱动内存已归还", SD_ROOT);
    return ESP_OK;
}

bool      sd_card_mounted(void)     { return s_mounted; }
int       sd_card_mount_count(void) { return s_mount_count; }
esp_err_t sd_card_last_err(void)    { return s_last_err; }

#else   /* !BOARD_HAS_TF —— 面包板变体：全部编成安全的空实现 */

esp_err_t sd_card_init(void)
{
    /* 刻意打日志：免得"离线模式怎么没歌"时怀疑是卡驱动坏了，
     * 实际是板型开关没开（面包板本来就没有卡座）。 */
    ESP_LOGI(TAG, "本板型无 TF 卡（BOARD_HAS_TF=0），跳过初始化");
    return ESP_OK;
}
bool      sd_card_present(void)     { return false; }
esp_err_t sd_card_mount(void)       { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t sd_card_unmount(void)     { return ESP_OK; }
bool      sd_card_mounted(void)     { return false; }
int       sd_card_mount_count(void) { return 0; }
esp_err_t sd_card_last_err(void)    { return ESP_ERR_NOT_SUPPORTED; }

#endif  /* BOARD_HAS_TF */
