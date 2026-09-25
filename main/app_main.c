/*
 * ESP-Music — 纯原生音乐终端
 *
 * M0 ✅ 工具链/分区/PSRAM + 板级 bring-up（I2C/ES8388/I2S，耳朵验收通过）
 * M1 ✅ 本机音频管道（解码→环形缓冲→I2S）：MP3 精确到帧、FLAC 不崩、underrun 0
 * M2 ✅ 联网 + Navidrome 拉流（服务端转 mp3，underrun 0）
 * M3 ✅ 播放页 UI（封面/歌名/滚动/进度，启动 5.1 秒）
 * M4 ✅ 按键 + 列表页/菜单页/音量页 + 来源切换
 * M5 ✅ 网页控制台（27 个接口，1000ms 轮询 + 前端插值）
 * M6 ⏳ 最近播放/收藏落盘 + 收藏与 Navidrome 双向一致 + 开机续播（本文件当前阶段）
 *
 * 编排顺序上有两条实测过的讲究（见 [[esp-music-boot-time]]）：
 *   1) net_mgr_init() 是异步的，放在 ui_start() 之前 —— 让连 WiFi 和建界面并行；
 *   2) 开机路径上不要 ping，getRandomSongs 本身就是连通性验证。
 */
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "board.h"
#include "audio_engine.h"
#include "net_mgr.h"
#include "subsonic.h"
#include "app_core.h"
#include "player.h"
#include "settings.h"
#include "store.h"
#include "hid_remote.h"
#include "ui.h"
#include "ui_internal.h"
#include "web_console.h"

static const char *TAG = "esp-music";

/* 启动页的步骤数（进度条按它算） */
#define SPLASH_STEPS 8

/* 每一阶段的内部 SRAM 余量。
 *
 * 为什么要一路打：内部 SRAM 是这块板唯一紧的资源，而"哪一步吃掉了多少"光看
 * 起止两个数完全推不出来 —— 加 BLE 时实测到"BT 初始化前余量从 27KB 掉到 7KB"
 * 却找不到是谁吃的（WiFi？socket 数？UI？），只能靠逐阶段打点定位。
 * 正常开机也就 8 行日志，留着当基线，以后再动内存配置有对照。 */
static void log_sram(const char *step)
{
    ESP_LOGI(TAG, "  SRAM[%s] 内部 %u KB ｜ DMA %u KB ｜ PSRAM %u KB",
             step,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

static void splash(const char *text, int step)
{
    ESP_LOGI(TAG, "[启动 %d/%d] %s", step, SPLASH_STEPS, text);
    log_sram(text);
    ui_splash_set(text, step, SPLASH_STEPS);
}


static void log_chip_info(void)
{
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    ESP_LOGI(TAG, "========== ESP-Music ==========");
    ESP_LOGI(TAG, "chip %s rev v%d.%d / %d core(s) / %d MHz / flash %" PRIu32 " MB / idf %s",
             CONFIG_IDF_TARGET, ci.revision / 100, ci.revision % 100, ci.cores,
             CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, flash_size / (1024 * 1024), esp_get_idf_version());
}

static void log_memory(const char *when)
{
    ESP_LOGI(TAG, "内存[%s]: PSRAM %u/%u KB  内部SRAM %u/%u KB  DMA可用 %u KB",
             when,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_total_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL) / 1024));
}

/* ============================================================ 键盘/导航自检
 *
 * 为什么要有它：我看不见屏幕、也按不到按键，而"K4 回不回得去"这类问题
 * **靠读代码已经栽了三次**（蓝牙页吞掉 K4 / 搜索页放行后没人接 / 来源页特判）。
 * 所以干脆让固件自己把按键序列打一遍、把每一步的页面名打出来，
 * 我读串口就能验证整条导航链。
 *
 * 打开后会：等起播 → 走一遍"进菜单→蓝牙→退出→搜索→打字→搜索→逐级返回"，
 * 每一步前后都打页面名。平时必须是 0（它会在开机后自己乱按键）。
 * 和 UI_DUMP_ON_SONG / UI_CALIBRATE / BASELINE_TONE 是同一类编译开关。
 */
/* 平时 0。打开后开机自动把按键序列打一遍并逐页打印（见下面注释）——
 * 改完按键/页面逻辑想自己验证时，把它改成 1、烧一次、读串口，比让用户按一遍快得多。
 * ⚠️ 开着的时候别忘关：它会在开机后自己乱按键。 */
#define K4_SELFTEST 0

#if K4_SELFTEST
static void key_selftest_task(void *arg)
{
    /* 等启动页收起（起播）—— 启动页期间 ui_page_show 会被吞掉 */
    while (ui_splash_running()) vTaskDelay(pdMS_TO_TICKS(200));
    vTaskDelay(pdMS_TO_TICKS(1500));

    static const struct { int k; int kind; const char *what; } steps[] = {
        { 3, 1, "长按K4 进菜单" },
        { 2, 0, "K3 确认(蓝牙控制)" },
        { 3, 0, "短按K4 退出蓝牙控制" },      /* ← 用户报过：没连蓝牙时退不出去 */
        { 3, 1, "长按K4 再进菜单" },
        { 1, 0, "K2 下移到 来源" },
        { 1, 0, "K2 下移到 搜索" },
        { 2, 0, "K3 进搜索方式" },
        { 2, 0, "K3 选(字母直搜)进输入页" },
        { 0, 0, "K1 向右(光标 D)" },
        { 0, 0, "K1 向右(光标 E)" },
        { 2, 0, "K3 输入字母" },
        { 2, 1, "长按K3 开始搜索" },
        { 3, 0, "短按K4 回输入页" },          /* ← 用户报过：搜完回不去 */
        { 3, 0, "短按K4 回搜索方式" },
        { 3, 0, "短按K4 回菜单" },
        { 3, 0, "短按K4 回播放页" },
        { 3, 1, "长按K4(在播放页=进菜单)" },
        { 3, 1, "长按K4 回播放器" },
    };
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        ui_page_t before = ui_page_current();
        ESP_LOGW(TAG, "[自检 %2u] %s ｜ 按键前=%s", (unsigned)(i + 1), steps[i].what,
                 ui_page_name(before));
        ui_keys_inject(steps[i].k, steps[i].kind);
        vTaskDelay(pdMS_TO_TICKS(1200));       /* 等页面切换/网络回来 */
        ESP_LOGW(TAG, "[自检 %2u]   -> 按键后=%s  uid=%d", (unsigned)(i + 1),
                 ui_page_name(ui_page_current()), (int)ui_page_current());
        if (ui_page_current() == UI_PAGE_SEARCH_IN) ui_search_debug_dump();
    }
    ESP_LOGW(TAG, "[自检] 全部走完");
    vTaskDelete(NULL);
}
#endif

/* M0 的基线测试音。⚠️ 实测它阻塞 2.8 秒，把网络初始化往后推了 3 秒，
 * 而且每次开机都"嘟"一声 —— 默认关掉，需要排查音频链路时再打开。 */
#define BASELINE_TONE 0

static void baseline_tone(void)
{
#if BASELINE_TONE
    ESP_LOGW(TAG, "[baseline] 1 秒静音 → 1.5 秒 440Hz");
    board_play_tone(1000, 440.0f, 0, false);
    board_play_tone(1500, 440.0f, 12000, false);
    board_play_tone(300, 0, 0, false);
#endif
}

/* 播放一个源并逐秒打印位置，直到播完/出错/超时（M1 的回归测试用，保留） */
static void play_and_watch(ae_source_t *src, const char *what, int timeout_s)
{
    if (src == NULL) {
        ESP_LOGE(TAG, "创建源失败: %s", what);
        return;
    }
    ESP_LOGW(TAG, "===== 播放: %s =====", what);
    audio_engine_play(src);

    uint32_t wall_t0 = (uint32_t)(esp_timer_get_time() / 1000);
    int sec = 0;
    while (audio_engine_state() != AE_STATE_FINISHED &&
           audio_engine_state() != AE_STATE_ERROR) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        sec++;
        uint32_t wall = (uint32_t)(esp_timer_get_time() / 1000) - wall_t0;
        ESP_LOGI(TAG, "[pos] 墙钟 %" PRIu32 "ms | 位置 %" PRIu32 "ms | 已解码 %" PRIu32 "ms | "
                      "ring_in %uKB ring_pcm %uKB | %s | underrun %d",
                 wall, audio_engine_position_ms(), audio_engine_decoded_ms(),
                 (unsigned)(audio_engine_ring_used_in() / 1024),
                 (unsigned)(audio_engine_ring_used_pcm() / 1024),
                 audio_engine_state_str(), audio_engine_underruns());
        if (sec > timeout_s) {
            ESP_LOGE(TAG, "超时 %d 秒未播完，中断", timeout_s);
            break;
        }
    }
    ESP_LOGW(TAG, "===== 结束: %s（格式 %s，采样率 %" PRIu32 "，underrun %d，ring_in 高水位 %uKB）=====",
             audio_engine_state_str(), audio_engine_format_str(),
             audio_engine_sample_rate(), audio_engine_underruns(),
             (unsigned)(audio_engine_ring_in_high_water() / 1024));

    audio_engine_stop();
    vTaskDelay(pdMS_TO_TICKS(300));
}

/* 网络不通时的退路：放内嵌测试音频，证明"解码→I2S→喇叭"这段是好的 */
static void m1_embedded_test(void)
{
    size_t len = 0;
    const uint8_t *mp3 = ae_embed_test_audio(false, &len);
    play_and_watch(ae_source_memory_new(mp3, len), "内嵌 MP3", 25);
}

/* 本机 python3 -m http.server 回归（可选，配了 URL 才跑） */
static void m2_local_http_test(void)
{
    const char *mp3_url = CONFIG_APP_M2_TEST_URL;
    if (mp3_url[0] == 0) return;
    ae_source_t *s = ae_source_http_new(mp3_url);
    play_and_watch(s, "本机 HTTP MP3", 40);
    ae_source_http_free(s);
}

/* 出声就收起启动页 —— 这就是"启动完成"那一刻 */
static void splash_watch_task(void *arg)
{
    uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000);
    while ((uint32_t)(esp_timer_get_time() / 1000) - t0 < 20000) {
        if (audio_engine_state() == AE_STATE_PLAYING) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "启动完成，用时 %" PRIu32 " ms", (uint32_t)(esp_timer_get_time() / 1000) - t0);
    ui_splash_finish();
    vTaskDelete(NULL);
}

void app_main(void)
{
    log_chip_info();
    log_memory("boot");
    player_status_init();

    splash("初始化硬件...", 1);
    if (board_selftest() != 0 || !board_is_ready()) {
        ESP_LOGE(TAG, "板级自检失败，停止后续测试");
        goto heartbeat;
    }
    splash("加载音频引擎...", 2);
    if (audio_engine_init() != ESP_OK) {
        ESP_LOGE(TAG, "音频引擎初始化失败");
        goto heartbeat;
    }

    /* ---- 先把网络初始化挂上（它是异步的）：让 WiFi 扫描/连接和 UI 准备并行，
     *      实测能省 1.5~2 秒开机时间。顺序很重要，别挪回下面。 ---- */
    bool net_ok = (net_mgr_init() == ESP_OK);

    /* ---- M3: 屏幕（4 个页面 + 按键都在这里建好） ---- */
    ESP_LOGW(TAG, "===== 显示 + 素材 + 中文字库 + 按键 =====");
    splash("加载界面...", 3);
    if (ui_start() != ESP_OK) {
        ESP_LOGE(TAG, "UI 启动失败");
    }

    baseline_tone();

    if (!net_ok) {
        ESP_LOGE(TAG, "网络初始化失败，回退到内嵌音频测试");
        ui_splash_finish();
        m1_embedded_test();
        goto heartbeat;
    }
    splash("连接 WiFi...", 4);
    bool up = net_mgr_wait_ip(25000);
    ESP_LOGW(TAG, "网络: %s  IP=%s  RSSI=%d dBm", up ? "已连接" : "未连接",
             net_mgr_ip_str(), net_mgr_rssi());
    if (!up) {
        ESP_LOGE(TAG, "WiFi 没连上 —— 查 SSID/密码、以及是不是 2.4GHz 网络（S3 不支持 5GHz）");
        ui_splash_finish();        /* 没网也别把启动页挂在那儿，用户会以为死机 */
        m1_embedded_test();
        goto heartbeat;
    }

    splash("获取歌单...", 5);
    m2_local_http_test();          /* 本机测试服务器回归（可选） */

    /* ---- M6: cache 分区（最近播放/收藏落盘）----
     * ⚠️ 必须排在 player_init 之前：player_init 要从盘上恢复最近播放/收藏。
     *    第一次上电分区里没有文件系统，这里会格式化（一次性，约 1 秒）。 */
    splash("准备存储...", 6);
    store_init();

    /* ---- M7: 蓝牙遥控（K1-K4 当 HID 外设控制手机/电脑）----
     * ⚠️ 必须排在这里：
     *   1) WiFi 先于 BT（共存要求，net_mgr_init 已经跑过了）；
     *   2) 播放【之前】—— BLE 要 40KB 量级的内部 SRAM，起播后只剩 10~15KB
     *      （见 [[esp-music-m6-persistence]]）。能不能跑起来靠 sdkconfig.forced
     *      把 NimBLE 的内存挪到 PSRAM，不是靠这里省出来的。
     * ⚠️ 失败非致命：只让遥控不可用，播放/UI/网页完全不受影响。 */
    splash("蓝牙遥控...", 7);
    if (hid_remote_init() != ESP_OK) {
        ESP_LOGW(TAG, "蓝牙遥控起不来（不影响播放，菜单里会显示\"不可用\"）");
    }

    /* ---- M4: 起播（之后按键/网页都通过 player 命令层控制）---- */
    if (subsonic_init() != ESP_OK || !subsonic_configured()) {
        ESP_LOGE(TAG, "没配 Navidrome 音乐源（host 为空）");
        ui_splash_finish();
        m1_embedded_test();
        goto heartbeat;
    }
    /* 设置（音量/亮度/来源…）——必须在 player_init / ui_start 之前准备好，
     * 它们开机就要读 NVS 里的上次设置。nvs_flash 已由 net_mgr_init 初始化。 */
    settings_init();
    if (player_init() != ESP_OK) {
        ESP_LOGE(TAG, "player 初始化失败");
        goto heartbeat;
    }
    splash("开始播放...", 8);
    /* ---- M5: 网页控制台 ---- */
    web_console_start();
    /* 沿用上次的来源，**并接着上次的位置放**（player_init 从 NVS/cache 恢复了
     * 来源、最近播放、续播位置）。取不到歌会自动退回随机 30 首。 */
    player_resume();
    /* ⚠️ 这个任务负责"起播后收起启动页"。创建失败的话启动页会**永远挂着**
     * （用户看到的就是死机），而 xTaskCreate 失败是静默的 —— 所以必须检查。 */
#if K4_SELFTEST
    xTaskCreate(key_selftest_task, "keytest", 4096, NULL, 3, NULL);
#endif
    if (xTaskCreate(splash_watch_task, "splash_w", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "启动页守护任务创建失败 —— 直接收起启动页，别让它挂在那儿");
        ui_splash_finish();
    }
    log_memory("after");

heartbeat:
    {
        uint32_t tick = 0;
        while (1) {
            player_status_t st;
            player_status_get(&st);
            ESP_LOGI(TAG, "heartbeat %" PRIu32 "  uptime=%lldms  %s [%d/%d] \"%s\" "
                          "pos=%" PRIu32 "/%" PRIu32 "ms  欠载 %d  SRAM %uKB PSRAM %uKB  net=%s(%s)",
                     ++tick, esp_timer_get_time() / 1000,
                     player_src_name(player_source()),
                     st.queue_index + 1, st.queue_count,
                     st.title[0] ? st.title : "-",
                     audio_engine_position_ms(), st.duration_ms,
                     audio_engine_underruns(),
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                     net_mgr_is_connected() ? "up" : "down", net_mgr_ip_str());
            vTaskDelay(pdMS_TO_TICKS(10000));
        }
    }
}
