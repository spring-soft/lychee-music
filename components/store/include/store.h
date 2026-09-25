/*
 * store —— cache 分区（FATFS + 磨损均衡）的唯一入口
 *
 * 为什么要单独一个组件：
 *   1. 挂载 FATFS 要处理"第一次上电分区是空的 → 必须格式化"这件事，
 *      逻辑只该有一份；挂载晚了（播放开始后）会撞上内部 SRAM 已经见底。
 *   2. 掉电安全只能在一处保证：**先写 .TMP，写完再改名**。分散到各调用点
 *      迟早有人直接写原始文件，掉一次电就留下半个 JSON。
 *
 * ⚠️ FATFS 关着长文件名（CONFIG_FATFS_LFN_NONE=y）：实测 ff.c 的 create_name
 *    在 `i >= ni` 时直接返回 FR_INVALID_NAME —— **超过 8 字符的名字是创建【失败】，
 *    不是被截断**。所以文件名必须严格 8.3（名字 ≤8、扩展名 ≤3），统一用大写短名：
 *      RECENT.RC / STAR.SC / PEND.OP / RESUME.RS / COVER/1A2B3C4D.565
 *    （关长文件名是刻意的：开着要多一套 LFN 缓冲，而内部 SRAM 已经很紧。）
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 挂载点。所有接口的相对路径都相对它 */
#define STORE_BASE "/fs"

/* 单次写入上限（防呆：分区 6MB，但没有哪个文件该超过这个数） */
#define STORE_MAX_WRITE (512 * 1024)

/* 挂载 cache 分区（挂不上会尝试格式化，只该在开机做一次）。
 * 失败不是致命的：所有接口会返回 ESP_ERR_INVALID_STATE，上层必须容忍。 */
esp_err_t store_init(void);
bool      store_ready(void);

/* 读整个文件。cap 装不下时返回 ESP_ERR_INVALID_SIZE —— **不静默截断**：
 * 半个 JSON 解析失败会让上层以为"本来就没有数据"，比直接报错更糟。 */
esp_err_t store_read(const char *rel, void *buf, size_t cap, size_t *len_out);

/* 原子写整个文件（写 .TMP → rename）。掉电只会留下旧的或新的，不会留下半个。 */
esp_err_t store_write(const char *rel, const void *buf, size_t len);

esp_err_t store_remove(const char *rel);

/* ---- 目录（封面缓存用） ---- */

/* 目录里的【文件】个数（子目录不算） */
int store_dir_count(const char *dir_rel);

/* 目录里超过 max 个时调到只剩 keep 个，返回删掉的个数（-1 = 出错）。
 *
 * ⚠️ 刻意【不】按 mtime 淘汰。设备没有 RTC 也没有 SNTP，`time()` 返回的是
 *    "开机以来的秒数"，于是**每次重启时钟都回到 0** —— 上一轮写的文件看起来
 *    比刚写的还新，按 mtime 淘汰会先删掉最新的那批。改用目录顺序（FAT 的
 *    f_open 复用最前面的空闲目录项，所以越靠前越旧）做 FIFO 淘汰。
 *    对缓存来说够用：偶尔误删一个热门封面，最多是多请求一次。 */
int store_evict(const char *dir_rel, int keep);

/* 剩余空间（KB），诊断用 */
uint64_t store_free_kb(void);

/* ---- 诊断（/api/debug 与串口日志都用它） ---- */
int store_write_count(void);
int store_fail_count(void);
int store_last_errno(void);

#ifdef __cplusplus
}
#endif
