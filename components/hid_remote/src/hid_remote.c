/*
 * hid_remote —— BLE HID 遥控实现（NimBLE + esp_hid）
 *
 * 时序（照 IDF 的 esp_hid_device 示例，但砍掉扫描/双模部分）：
 *   esp_bt_controller_mem_release(CLASSIC) → esp_bt_controller_init/enable
 *   → esp_nimble_init()（内部把 NimBLE 主机任务也起好）
 *   → esp_hidd_dev_init(BLE) → 收到 ESP_HIDD_START_EVENT 后开始广播
 *
 * ⚠️ S3 上用的是 "SOC_ESP_NIMBLE_CONTROLLER" 那套（控制器和主机是同一份 NimBLE），
 *    所以示例里那个 nimble_port_run() 的 host 任务其实是死代码 —— 别照抄，
 *    起了两个主机循环只会互相打架。这里靠 esp_nimble_init() 自己起任务。
 *
 * ⚠️ 按键是"发给对端"的低带宽操作，不需要额外任务：
 *    按下用 esp_hidd_dev_input_set()，20ms 后用一次性 esp_timer 发全零释放。
 *    （主机把一直按着的位当成"长按"，不释放会出现"上一首一直跳"。）
 */
#include <string.h>
#include "hid_remote.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_bt.h"
#include "esp_hidd.h"
/* ⚠️ 这些头靠 esp_hid 的公开 REQUIRES(bt) 传递过来 —— 不用手写 nimble 的绝对路径 */
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_uuid.h"
#include "services/gap/ble_svc_gap.h"

static const char *TAG = "hid";

#define RPT_ID           1        /* Consumer Control 的 Report ID，要和 report map 一致 */
#define RPT_LEN          2        /* 9 个用法位 + 7 位填充 = 16 位 */
#define HOLD_MS          20       /* 按下多久后自动释放 */
/* ⚠️ 广播数据（legacy adv）总共只有 31 字节：flags 3 + appearance 4 +
 *    UUID16 4 = 11，留给名字的只有 20 字节。中文名"ESP-Music 遥控"是 8 个
 *    汉字 = 24 字节，直接超限 —— 实测 ble_gap_adv_set_fields 返回 rc=4
 *    (BLE_HS_EMSGSIZE)，广播数据没设上，手机就扫不到这个设备。
 *    改用短 ASCII 名（同时也避免部分主机把 UTF-8 名字显示成乱码）。 */
#define DEVICE_NAME      "ESP-Music RC"

/* Consumer Control 报告描述符。
 * 位序必须和下面的 s_bit[] 一一对应 —— 改了这里就得改那里，否则按 K1 出音量键。 */
static uint8_t s_report_map[] = {
    0x05, 0x0C,        /* Usage Page (Consumer) */
    0x09, 0x01,        /* Usage (Consumer Control) */
    0xA1, 0x01,        /* Collection (Application) */
    0x85, RPT_ID,      /*   Report ID (1) */
    0x09, 0xE9,        /*   Usage (Volume Increment)   bit0 */
    0x09, 0xEA,        /*   Usage (Volume Decrement)   bit1 */
    0x09, 0xE2,        /*   Usage (Mute)               bit2 */
    0x09, 0xCD,        /*   Usage (Play/Pause)         bit3 */
    0x09, 0xB5,        /*   Usage (Scan Next Track)    bit4 */
    0x09, 0xB6,        /*   Usage (Scan Previous)      bit5 */
    0x09, 0xB7,        /*   Usage (Stop)               bit6 */
    0x09, 0xB3,        /*   Usage (Fast Forward)       bit7 */
    0x09, 0xB4,        /*   Usage (Rewind)             byte1 bit0 */
    0x15, 0x00,        /*   Logical Minimum (0) */
    0x25, 0x01,        /*   Logical Maximum (1) */
    0x75, 0x01,        /*   Report Size (1) */
    0x95, 0x09,        /*   Report Count (9) */
    0x81, 0x02,        /*   Input (Data,Var,Abs) */
    0x95, 0x07,        /*   Report Count (7) —— 补齐到 2 字节 */
    0x81, 0x01,        /*   Input (Const) */
    0xC0               /* End Collection */
};

static esp_hid_raw_report_map_t s_report_maps[] = {
    { .data = s_report_map, .len = sizeof(s_report_map) },
};

static esp_hid_device_config_t s_dev_cfg = {
    .vendor_id         = 0x303A,     /* Espressif 的 VID（我们不是真 USB 设备，填它最不容易被系统挑刺） */
    .product_id        = 0x0001,
    .version           = 0x0100,
    .device_name       = DEVICE_NAME,
    .manufacturer_name = "ESP-Music",
    .serial_number     = "esp-music-01",
    .report_maps       = s_report_maps,
    .report_maps_len    = 1,
};

static esp_hidd_dev_t   *s_dev;
static esp_timer_handle_t s_release_timer;
static volatile bool     s_available;      /* 栈起来了 */
static volatile bool     s_connected;      /* 有对端连着 */
static volatile bool     s_mode;           /* 蓝牙控制模式（不存 NVS，见头文件） */
static volatile int      s_sent;
static char              s_peer[32];

/* 按 bit 序排放（对应 report map）：byte0 bit0..7, byte1 bit0 */
static const uint8_t s_bit[][2] = {
    [HID_KEY_VOL_UP]   = { 0x01, 0 },
    [HID_KEY_VOL_DOWN] = { 0x02, 0 },
    [HID_KEY_MUTE]     = { 0x04, 0 },
    [HID_KEY_PLAY_PAUSE] = { 0x08, 0 },
    [HID_KEY_NEXT]     = { 0x10, 0 },
    [HID_KEY_PREV]     = { 0x20, 0 },
    [HID_KEY_STOP]     = { 0x40, 0 },
    [HID_KEY_FAST_FWD] = { 0x80, 0 },
    [HID_KEY_REWIND]   = { 0x00, 0x01 },
};

static void report_send(const uint8_t *buf)
{
    if (s_dev == NULL) return;
    esp_err_t err = esp_hidd_dev_input_set(s_dev, 0, RPT_ID, (uint8_t *)buf, RPT_LEN);
    if (err != ESP_OK) ESP_LOGW(TAG, "发 HID 报告失败: %s", esp_err_to_name(err));
}

/* 20ms 后松手。用一次性 esp_timer 而不是 sleep：调用方可能是按键回调（定时器任务），
 * 在那里阻塞会拖住别的按键判定。 */
static void release_cb(void *arg)
{
    uint8_t zero[RPT_LEN] = { 0, 0 };
    report_send(zero);
}

/* ============================================================ 广播 */

static struct ble_hs_adv_fields s_fields;

/* 记下对端 MAC（纯诊断用：屏幕/网页能告诉用户"连的是哪台"）。
 * ⚠️ esp_hidd 自己注册的是【全局】GAP 事件监听器（nimble_hidd.c 里
 *    ble_gap_event_listener_register），所以这里再挂一个广告实例级的回调
 *    不会抢掉它的连接/订阅事件 —— 两边都能收到。 */
static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0
                && ble_gap_conn_find(event->connect.conn_handle, &desc) == 0) {
                snprintf(s_peer, sizeof(s_peer), "%02X:%02X:%02X:%02X:%02X:%02X",
                         desc.peer_ota_addr.val[5], desc.peer_ota_addr.val[4],
                         desc.peer_ota_addr.val[3], desc.peer_ota_addr.val[2],
                         desc.peer_ota_addr.val[1], desc.peer_ota_addr.val[0]);
                ESP_LOGW(TAG, "对端 %s（按 K1-K4 会控制对方）", s_peer);
            } else {
                ESP_LOGW(TAG, "连接失败/断开 status=%d", event->connect.status);
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            s_peer[0] = 0;
            break;
        default:
            break;
    }
    return 0;
}

/* 广播间隔（毫秒）。为什么要两档 —— 用户提了一个很实在的问题：
 * "开机不广播，那手机怎么连得上？"
 * 广播必须**一直开着**，否则手机根本发现不了这台设备；但广播是持续开销
 * （每 N 毫秒在 3 个信道各发一包，还要和 WiFi 抢 coex 时隙），
 * 一直用 30ms 的密间隔去换"可能随时要连"是浪费。
 * 折中：**待机用慢间隔（500ms，射频唤醒次数少 16 倍）仍然可被发现**，
 * 进「蓝牙控制」页时切成快间隔（30ms）—— 那时用户真的要用，连得快才重要。 */
#define ADV_ITVL_IDLE_MS   500
#define ADV_ITVL_FAST_MS   30

static bool     s_adv_on;
static uint32_t s_adv_itvl_ms;

static void adv_start(uint32_t itvl_ms)
{
    struct ble_gap_adv_params adv;
    memset(&adv, 0, sizeof(adv));
    adv.conn_mode = BLE_GAP_CONN_MODE_UND;      /* 可被连接 */
    adv.disc_mode = BLE_GAP_DISC_MODE_GEN;      /* 可被发现 */
    adv.itvl_min = BLE_GAP_ADV_ITVL_MS(itvl_ms);
    adv.itvl_max = BLE_GAP_ADV_ITVL_MS(itvl_ms + itvl_ms / 4);
    int rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &adv,
                              gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "开始广播失败 rc=%d", rc);
        s_adv_on = false;
        return;
    }
    s_adv_on = true;
    s_adv_itvl_ms = itvl_ms;
}

static void adv_setup(void);      /* 定义在下面（广播内容），这里先声明 */

/* 按当前模式调广播参数。改参数要先停再起（NimBLE 不允许运行中改）——
 * 中间断几毫秒，已经连着的设备不受影响。 */
static void adv_apply(void)
{
    uint32_t want = s_mode ? ADV_ITVL_FAST_MS : ADV_ITVL_IDLE_MS;
    if (s_adv_on && s_adv_itvl_ms == want) return;
    if (s_adv_on) ble_gap_adv_stop();
    s_adv_on = false;
    adv_setup();
    adv_start(want);
    if (s_adv_on) {
        ESP_LOGW(TAG, "广播%s（间隔 %ums，手机随时连得上；设备名 \"%s\"）",
                 s_mode ? "加速：用户在用遥控" : "待机", (unsigned)want, DEVICE_NAME);
    }
}

static void adv_setup(void)
{
    memset(&s_fields, 0, sizeof(s_fields));
    /* 通用可发现 + 不支持经典蓝牙（S3 只有 BLE，声称支持会让部分主机走错分支） */
    s_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    /* Appearance 用 HID 键盘 */
    s_fields.appearance = 0x03C1;
    s_fields.appearance_is_present = 1;
    s_fields.tx_pwr_lvl_is_present = 1;
    s_fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    s_fields.name = (uint8_t *)DEVICE_NAME;
    s_fields.name_len = strlen(DEVICE_NAME);
    s_fields.name_is_complete = 1;

    /* 广播里必须带 HID 服务 UUID（0x1812），否则部分主机扫不到 HID 设备 */
    static ble_uuid16_t hid_uuid = BLE_UUID16_INIT(0x1812);
    s_fields.uuids16 = &hid_uuid;
    s_fields.num_uuids16 = 1;
    s_fields.uuids16_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&s_fields);
    if (rc != 0) ESP_LOGE(TAG, "设置广播内容失败 rc=%d", rc);
}

/* ============================================================ HID 事件 */

/* ⚠️ 签名是 esp_event 那套（handler_args/base/id/event_data），不是
 *    `void cb(esp_hidd_event_t, esp_hidd_event_data_t*)` —— 后者编译不过。 */
static void hidd_event_cb(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_hidd_event_t event = (esp_hidd_event_t)id;
    switch (event) {
        case ESP_HIDD_START_EVENT:
            s_available = true;
            /* ⚠️ **一直广播**（待机用慢间隔）—— 不广播的话手机根本发现不了设备。
             * 之前我按"退出功能别占性能"理解成"退出就停广播"，结果手机连不上，
             * 用户一句话点破（"进入才开蓝牙退出又关掉然后手机连不上？"）。
             * 正确做法是"一直在但待机放慢"，见 ADV_ITVL_* 的说明。 */
            ESP_LOGW(TAG, "HID 已就绪");
            adv_apply();
            break;

        case ESP_HIDD_CONNECT_EVENT:
            s_connected = true;
            s_peer[0] = 0;
            ESP_LOGW(TAG, "已连接（现在按 K1-K4 会控制对方）");
            break;

        case ESP_HIDD_DISCONNECT_EVENT:
            s_connected = false;
            s_peer[0] = 0;
            ESP_LOGW(TAG, "已断开，继续广播等它回来");
            s_adv_on = false;              /* 连接期间广播会被协议栈停掉，标记一下 */
            adv_apply();
            break;

        default:
            break;
    }
}

/* ============================================================ 初始化 */

/* NimBLE 主机任务：跑协议栈主循环，直到 nimble_port_stop()。
 *
 * ⚠️⚠️ 这个任务必须由我们显式启动（`esp_nimble_enable(nimble_host_task)`）——
 *    在 S3 上 esp_nimble_init() 只初始化数据结构，**不建任务**：
 *    `ble_transport_hs_init()` 里只有一句 ble_hs_init()。
 *    没有这个任务，主机永远不会跟控制器同步 —— 症状是"BLE 栈已起"但
 *    **永远不广播、也扫不到**，而且不报任何错（我在这上面查了几轮）。
 *    IDF 的 esp_hid_device 示例是在最后调 `esp_nimble_enable(host_task)`
 *    （我一开始搜 nimble_port_freertos_init 所以漏了它，它俩是一回事：
 *     nimble_port_freertos_init() 内部就是调 esp_nimble_enable()）。 */
static void nimble_host_task(void *param)
{
    ESP_LOGI(TAG, "NimBLE 主机任务启动");
    nimble_port_run();                 /* 只有 nimble_port_stop() 能让它返回 */
    nimble_port_freertos_deinit();
    vTaskDelete(NULL);
}

esp_err_t hid_remote_init(void)
{
    /* 经典蓝牙在这块芯片上不存在，释放掉能省一点（S3 上是空操作，但无害） */
    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_bt_controller_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "控制器初始化失败: %s（蓝牙遥控不可用，其余功能不受影响）",
                 esp_err_to_name(err));
        return err;
    }
    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "控制器启用失败: %s", esp_err_to_name(err));
        esp_bt_controller_deinit();
        return err;
    }

    err = esp_nimble_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE 初始化失败: %s（多半是内部 SRAM 不够，见 sdkconfig.forced）",
                 esp_err_to_name(err));
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        return err;
    }

    /* Just Works 配对：手机点一下就配上，不要 PIN。
     * （有屏幕的话 DISP_ONLY + 6 位 passkey 更安全，但用户要的是"好连"。） */
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;                 /* 记住配对，下次自动连 */
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    /* GATT 里的 Device Name 特征（有些主机会先读它，再决定显示的名字） */
    ble_svc_gap_device_name_set(DEVICE_NAME);

    err = esp_hidd_dev_init(&s_dev_cfg, ESP_HID_TRANSPORT_BLE, hidd_event_cb, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_hidd_dev_init 失败: %s", esp_err_to_name(err));
        return err;
    }

    /* ⚠️⚠️ 必须在 esp_hidd_dev_init() 【之后】才启动主机任务：
     *    esp_hidd 是在 dev_init 的最后一行把 ble_hs_cfg.sync_cb 设成
     *    nimble_host_synced()（START 事件就是从那儿发的）。
     *    主机任务一旦跑起来就会去同步，同步完成时调 sync_cb —— 所以
     *    "先设好回调、再启动任务"这个顺序不能反，反了 START 事件就永远不会来。
     *    （这个顺序也正是 IDF 示例的顺序：esp_hidd_dev_init() → esp_nimble_enable()。） */
    esp_nimble_enable(nimble_host_task);

    const esp_timer_create_args_t rel = {
        .callback = release_cb, .name = "hid_rel",
    };
    if (esp_timer_create(&rel, &s_release_timer) != ESP_OK) s_release_timer = NULL;

    ESP_LOGW(TAG, "BLE 栈已起（内部 SRAM 剩 %u KB）—— 等主机同步后开始广播",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    return ESP_OK;
}

/* ============================================================ 公开接口 */

bool hid_remote_available(void) { return s_available; }
/* 事件标志 + esp_hidd 自己的判断：漏掉一次 DISCONNECT 事件也不会误发按键给对方 */
bool hid_remote_connected(void)
{
    return s_connected && s_dev != NULL && esp_hidd_dev_connected(s_dev);
}
bool hid_remote_mode(void) { return s_mode; }

void hid_remote_set_mode(bool on)
{
    if (s_mode == on) return;
    s_mode = on;
    ESP_LOGW(TAG, "蓝牙控制模式 %s", on ? "开（按键只发给对端，不再显示本机歌名）"
                                        : "关（回到播放器，按键控制本机）");
    /* 进出遥控模式 = 切换广播快慢（不是开关广播 —— 关了手机就连不上，
     * 见 ADV_ITVL_* 的说明）。 */
    if (s_available && !hid_remote_connected()) adv_apply();
}

bool hid_remote_active(void)
{
    return s_available && s_mode && hid_remote_connected();
}

void hid_remote_send(hid_key_t k)
{
    if (!hid_remote_active() || k == HID_KEY_NONE) return;
    uint8_t buf[RPT_LEN] = { s_bit[k][0], s_bit[k][1] };
    report_send(buf);
    s_sent++;
    /* 松手：不释放的话对端会当成一直按着（"上一首"会连跳） */
    if (s_release_timer) {
        esp_timer_stop(s_release_timer);
        esp_timer_start_once(s_release_timer, HOLD_MS * 1000);
    }
}

const char *hid_remote_state_str(void)
{
    if (!s_available) return "不可用";
    if (hid_remote_connected()) return "已连接";
    return "未连接";
}

const char *hid_remote_peer_name(void) { return s_peer; }

int hid_remote_sent_count(void) { return s_sent; }
