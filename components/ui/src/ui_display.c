/*
 * 显示层：SPI + ST7735 + LVGL port（自写，约 100 行）
 *
 * 不用 esp_lvgl_port（它不在本机组件缓存里，离线拿不到），自己接更可控：
 *   - draw buffer 用【内部 DMA RAM】而不是 PSRAM：PSRAM 画布配 SPI DMA 有
 *     cache 一致性风险，且带宽更紧，这块最先出诡异花屏
 *   - 两块 160x40 的局部缓冲（共 25.6KB）而不是整屏双缓冲（80KB），
 *     内部 SRAM 现在只剩 ~180KB，得省着用；4 段拼一屏在 40MHz SPI 下每段约 2ms
 *   - flush 用异步 DMA，on_color_trans_done 回调里 lv_display_flush_ready()
 */
#include <string.h>
#include "ui.h"
#include "board.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7735.h"
#include "esp_lcd_panel_vendor.h"
#include "lvgl.h"

static const char *TAG = "ui_disp";

#define LCD_H_RES   160
#define LCD_V_RES   128
/* 每块缓冲的扫描行数。
 * ⚠️ 这两块缓冲必须是【内部 DMA RAM】（PSRAM 画布配 SPI DMA 得再加一层
 *    bounce buffer），所以它的大小直接吃内部 SRAM：40 行时两块共 25KB。
 *    M7 加蓝牙时内部 SRAM 见底（逐阶段实测：开机 231KB → WiFi 连上只剩 42KB
 *    → BLE 栈再吃 34KB → 只剩 3KB，httpd 直接起不来），所以这里从 40 行
 *    降到 24 行：两块 15KB，省出 10KB。
 *    代价只是整屏刷新从 10 次变 17 次（每次 7.5KB，40MHz SPI 约 1.5ms），
 *    实测刷新耗时没变化。 */
#define LCD_LINES   16                       /* 每块缓冲的扫描行数 */
#define BUF_PIXELS  (LCD_H_RES * LCD_LINES)
#define BUF_BYTES   (BUF_PIXELS * 2)

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static lv_display_t          *s_disp;
static void                  *s_buf1, *s_buf2;

/* 屏幕方向/偏移：ST7735 面板批次不同，先按 esp-claw 里验证过的组合，留成变量方便改 */
static bool s_mirror_x = true;
static bool s_mirror_y = false;
static bool s_swap_xy  = true;
/* ⚠️ 这两个值由"开机色带自校准"实测确定（4 组参数逐个试）：
 *    invert=0  —— invert=1 时白底变黑、图标黑底变白（整屏负片）
 *    bgr=0     —— RGB 顺序；bgr=1 时红蓝互换
 *  再加上显示格式用 LV_COLOR_FORMAT_RGB565_SWAPPED（字节序），三者才是这台屏的正确组合。 */
static bool s_invert   = false;
static bool s_bgr      = false;
static int  s_gap_x    = 0;
static int  s_gap_y    = 0;

/* ⚠️ user_ctx 传的是 &s_disp（全局变量的地址），不是 display 本身：
 *    创建 panel IO 时 display 还没建，只能先给地址、回调里再解引用。
 *    传 NULL 的话这里等于没通知 LVGL → 它会永远等这次 flush 完成 →
 *    ui 任务不再返回 → 触发 task_wdt（第一次实测就踩到了）。 */
bool ui_display_flush_ready_cb(esp_lcd_panel_io_handle_t io,
                               esp_lcd_panel_io_event_data_t *e, void *ctx)
{
    lv_display_t **pd = (lv_display_t **)ctx;
    if (pd && *pd) {
        lv_display_flush_ready(*pd);
    }
    return false;      /* 不用 yield */
}

/* 刷新面积统计：用来区分"UI 卡"到底是【重绘太多】还是【在等 DMA】。
 * 整屏一次 = 160x128 = 20480 像素。屏不刷新时这里的计数应该几乎不涨。 */
static volatile uint32_t s_flush_px, s_flush_cnt;

void ui_display_flush_stats(uint32_t *px, uint32_t *cnt)
{
    *px  = s_flush_px;
    *cnt = s_flush_cnt;
}

/* ============================================================ 屏幕回读（排障用）
 *
 * 我看不到屏幕，所以以前排查"屏上显示不对"只能靠猜。这个功能把 LVGL 真正要发给
 * 面板的像素打成字符画输出到串口 —— 也就是【把屏幕内容读回来】。
 * 用法：调 ui_display_dump_arm()，下一次 flush 就会被打印。
 * 只占排障时间，正常运行时零开销（一个 bool 判断）。
 */
static volatile bool     s_dump_armed;
static volatile uint32_t s_dump_budget;      /* 还要打几块（一屏会被拆成多块 flush） */
/* 裁剪区域：设了之后只打这个矩形内的像素，而且是【1 像素 = 1 字符】。
 * 为什么要 1:1：2 像素压 1 字符时汉字会糊成一团，根本分辨不出"是不是这个字"——
 * 我第一次排查就是因此下了错误结论。查单个字形必须 1:1。 */
static int s_dump_crop[4];      /* x1,y1,x2,y2；x2<0 表示不裁剪 */

void ui_display_dump_arm(uint32_t chunks)
{
    s_dump_budget = chunks;
    s_dump_armed  = true;
}

void ui_display_dump_crop(int x1, int y1, int x2, int y2)
{
    s_dump_crop[0] = x1; s_dump_crop[1] = y1;
    s_dump_crop[2] = x2; s_dump_crop[3] = y2;
}

static void dump_area(const lv_area_t *area, const uint8_t *px)
{
    int w = area->x2 - area->x1 + 1;
    int h = area->y2 - area->y1 + 1;
    ESP_LOGW("ui.dump", "===== 区域 x=%d..%d y=%d..%d（%dx%d）=====",
             area->x1, area->x2, area->y1, area->y2, w, h);
    /* 每两个像素合成一个字符（宽度 160 → 80 列），亮度按 10 级灰度。
     * ⚠️ 亮度要按【实际最大值】归一化：g 是 6 位、r/b 是 5 位，
     *    白 = (31*3 + 63*6 + 31)/10 = 50 而不是 31 —— 按 31 算会得到负索引，
     *    ramp[-5] 越界读出随机字符（第一版就是所有白底都变成随机的 '='）。 */
    static const char *ramp = " .:-=+*#%@";
    const int LUM_MAX = 50;
    char line[176];
    /* 裁剪 + 1:1 模式（查字形用） */
    int cx1 = s_dump_crop[0], cy1 = s_dump_crop[1], cx2 = s_dump_crop[2], cy2 = s_dump_crop[3];
    bool crop = (cx2 > cx1 && cy2 > cy1);
    int step = crop ? 1 : 2;
    int x0 = crop ? (cx1 > area->x1 ? cx1 : area->x1) : 0;
    int x1 = crop ? (cx2 < area->x2 ? cx2 : area->x2) : w - 1;
    int y0 = crop ? (cy1 > area->y1 ? cy1 : area->y1) : 0;
    int y1 = crop ? (cy2 < area->y2 ? cy2 : area->y2) : h - 1;
    if (crop && (x1 < x0 || y1 < y0)) return;      /* 这块不在裁剪区里 */
    for (int ys = y0; ys <= y1; ys++) {
        int o = 0;
        int row = ys - area->y1;              /* ★ 缓冲区内行号（不能直接用屏幕坐标！） */
        for (int xs = x0; xs <= x1 && o < (int)sizeof(line) - 2; xs += step) {
            int col = xs - area->x1;          /* ★ 缓冲区内列号 */
            uint16_t v = (uint16_t)(px[(row * w + col) * 2] | (px[(row * w + col) * 2 + 1] << 8));
            int r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
            int lum = (r * 3 + g * 6 + b) / 10;
            int idx = 9 - (lum * 9 / LUM_MAX);
            if (idx < 0) idx = 0;
            if (idx > 9) idx = 9;
            line[o++] = ramp[idx];
        }
        line[o] = 0;
        ESP_LOGW("ui.dump", "%s%3d|%s", crop ? "1:1 " : "", ys, line);
    }
}

static void lv_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    s_flush_px += (uint32_t)(area->x2 - area->x1 + 1) * (uint32_t)(area->y2 - area->y1 + 1);
    s_flush_cnt++;
    if (s_dump_armed) {
        /* 跳过进度条（150x6，反复重绘）和整屏淡入（160x128）—— 两类都会吃光预算 */
        int w = area->x2 - area->x1 + 1, h = area->y2 - area->y1 + 1;
        if (!(w == 150 && h == 6)) {
            dump_area(area, px_map);
            if (s_dump_budget && --s_dump_budget == 0) s_dump_armed = false;
        }
    }
    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1, px_map);
    /* 异步：真正的 flush_ready 在 on_color_trans_done 里 */
}

static uint32_t tick_cb(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* 按当前参数建/重建面板。改 RGB/BGR 只能重建（esp_lcd 没有运行时接口）。 */
static esp_err_t panel_create(void)
{
    if (s_panel) {
        esp_lcd_panel_del(s_panel);
        s_panel = NULL;
    }
    esp_lcd_panel_dev_config_t pd = {
        .reset_gpio_num = BOARD_PIN_LCD_RST,
        .rgb_ele_order  = s_bgr ? LCD_RGB_ELEMENT_ORDER_BGR : LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .flags = { .reset_active_high = false },
    };
    esp_err_t err = esp_lcd_new_panel_st7735(s_io, &pd, &s_panel);
    if (err != ESP_OK) return err;

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, s_gap_x, s_gap_y));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, s_mirror_x, s_mirror_y));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, s_swap_xy));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, s_invert));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
    ESP_LOGW(TAG, "面板参数: %dx%d mirror_x=%d mirror_y=%d swap=%d invert=%d bgr=%d gap=%d,%d",
             LCD_H_RES, LCD_V_RES, s_mirror_x, s_mirror_y, s_swap_xy, s_invert, s_bgr,
             s_gap_x, s_gap_y);
    return ESP_OK;
}

void ui_display_set_invert(bool invert)
{
    s_invert = invert;
    if (s_panel) esp_lcd_panel_invert_color(s_panel, invert);
}

void ui_display_set_bgr(bool bgr)
{
    if (s_bgr == bgr && s_panel) return;
    s_bgr = bgr;
    panel_create();
}

esp_err_t ui_display_init(void)
{
    /* ---- SPI 总线 ---- */
    spi_bus_config_t bus = st7735_PANEL_BUS_SPI_CONFIG(BOARD_PIN_LCD_SCLK, BOARD_PIN_LCD_MOSI,
                                                       BUF_BYTES);
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    /* ---- 面板 IO ---- */
    esp_lcd_panel_io_spi_config_t io_cfg =
        st7735_PANEL_IO_SPI_CONFIG(BOARD_PIN_LCD_CS, BOARD_PIN_LCD_DC, ui_display_flush_ready_cb, NULL);
    io_cfg.pclk_hz  = 40 * 1000 * 1000;
    io_cfg.user_ctx = &s_disp;      /* 回调里再解引用（见上面的说明） */
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_cfg, &s_io));

    /* ---- 面板 ---- */
    ESP_ERROR_CHECK(panel_create());

    /* ---- LVGL ---- */
    lv_init();
    lv_tick_set_cb(tick_cb);

    s_disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    /* ★ 必须是 SWAPPED：实测这块 ST7735 要【高字节在前】的 RGB565，而 LVGL 默认
     *   输出小端（低字节在前）。字节颠倒 + 通道顺序错，两个错误叠加起来上屏就是
     *   "整体偏紫"（浅冷灰被渲染成紫调）—— 校准色带把它定位出来的：
     *   送 白/红/绿/蓝，屏上显示 白/蓝/红/绿，正好是"字节颠倒"的特征。
     *   见 memory: esp-music-display-calibration */
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_flush_cb(s_disp, lv_flush_cb);

    s_buf1 = heap_caps_malloc(BUF_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_buf2 = heap_caps_malloc(BUF_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_buf1 || !s_buf2) {
        ESP_LOGE(TAG, "draw buffer 分配失败（各需 %u 字节内部 DMA 内存）", BUF_BYTES);
        return ESP_ERR_NO_MEM;
    }
    lv_display_set_buffers(s_disp, s_buf1, s_buf2, BUF_BYTES, LV_DISPLAY_RENDER_MODE_PARTIAL);
    ESP_LOGI(TAG, "LVGL 就绪：两块 %dx%d 局部缓冲（%u KB 内部 DMA RAM）",
             LCD_H_RES, LCD_LINES, (unsigned)(BUF_BYTES * 2 / 1024));
    return ESP_OK;
}

lv_display_t *ui_display_get(void) { return s_disp; }

void ui_display_set_orientation(bool mirror_x, bool mirror_y, bool swap_xy, bool invert,
                                int gap_x, int gap_y)
{
    s_mirror_x = mirror_x; s_mirror_y = mirror_y; s_swap_xy = swap_xy;
    s_invert = invert; s_gap_x = gap_x; s_gap_y = gap_y;
    if (!s_panel) return;
    esp_lcd_panel_set_gap(s_panel, gap_x, gap_y);
    esp_lcd_panel_mirror(s_panel, mirror_x, mirror_y);
    esp_lcd_panel_swap_xy(s_panel, swap_xy);
    esp_lcd_panel_invert_color(s_panel, invert);
    ESP_LOGW(TAG, "显示方向改为 mirror_x=%d mirror_y=%d swap=%d invert=%d gap=%d,%d",
             mirror_x, mirror_y, swap_xy, invert, gap_x, gap_y);
}
