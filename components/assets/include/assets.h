/*
 * 预渲染素材 / 静态资源访问（assets 分区 mmap，零拷贝）
 *
 * 素材由 tools/gen_assets.py 打包成单个 assets.bin，构建期用
 * esptool_py_flash_to_partition() 注册到 "assets" 分区，
 * 运行时 mmap 后按索引取指针 —— 既能直接交给 lv_image_dsc_t.data，
 * 也能直接喂给 lv_binfont_create_from_buffer()，还能当 httpd_resp_send_chunk() 的源。
 *
 * 为什么要独立成一个组件（而不是塞在 ui 里）：
 *   web_console 要发 assets 里的 index.html.gz，但方案里约定
 *   「ui / web_console / hid_remote / net_mgr 互不引用」—— 只有把它下沉成
 *   共同依赖，才能既满足分层又不重复实现一次 mmap。
 *
 * ⚠️ mmap 出来的指针不能交给 SPI DMA（flash 映射区不是 DMA 可访问的物理内存）。
 *    LVGL 会把图绘进 RAM 里的 draw buffer 再交 DMA，所以走 LVGL 就没这个问题。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "assets_index.h"   /* 生成：asset_id_t / asset_names[] */

#ifdef __cplusplus
extern "C" {
#endif

/* fmt 字段的取值 */
#define ASSET_FMT_RGB565     0   /* 位图（w/h 有效） */
#define ASSET_FMT_FONT       1   /* LVGL bin 字体（不透明 blob） */
#define ASSET_FMT_RGB565A8   2   /* 位图 + 独立 alpha 平面（图标） */
#define ASSET_FMT_WEB_GZ     3   /* 文本资源（已 gzip，交给 httpd 时带 Content-Encoding） */
#define ASSET_FMT_RAW        4   /* 通用二进制 blob（拼音表这种：自带格式、不透明） */

typedef struct {
    const uint8_t *data;
    uint32_t       size;
    uint16_t       w, h;      /* 位图才有；字体/文本为 0 */
    uint8_t        fmt;       /* ASSET_FMT_* */
} asset_t;

/* mmap assets 分区并校验 magic */
esp_err_t assets_init(void);

/* 按 id 取素材；失败返回 false（id 越界或分区是空的） */
bool assets_get(asset_id_t id, asset_t *out);

/* 按文件名取素材（web_console 用 "index.html" 这种原名，不关心枚举顺序） */
bool assets_get_by_name(const char *name, asset_t *out);

/* 调试：打印所有素材 */
void assets_dump(void);

#ifdef __cplusplus
}
#endif
