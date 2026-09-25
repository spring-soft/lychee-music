/*
 * store —— cache 分区（FATFS + 磨损均衡）实现
 *
 * 掉电安全：所有写入都是 "写 <名>.TMP → rename 成正式名"。FATFS 的 rename
 * 在同一目录内只是改目录项，是原子的，所以任何时刻掉电，正式文件要么是旧的
 * 完整内容、要么是新的完整内容，不会出现"写了一半的 JSON"。
 */
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#include "store.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "store";

#define PART_LABEL "cache"
/* 同时打开的文件：封面/JSON 都是一次一个，4 个绰绰有余。
 * 每个 FIL 在 FF_MAX_SS=4096 下约 4KB（PER_FILE_CACHE），但
 * CONFIG_FATFS_ALLOC_PREFER_EXTRAM=y 会把它们放 PSRAM —— 内部 SRAM 只剩几 KB。 */
#define STORE_MAX_FILES 4

static wl_handle_t       s_wl = WL_INVALID_HANDLE;
static bool              s_ready;
static SemaphoreHandle_t s_lock;
static int               s_writes;
static int               s_fails;
static int               s_last_errno;

/* ---------------------------------------------------------------- 路径 */

/* 校验一段名字是严格的 8.3（FATFS 关着长文件名，超长是"创建失败"不是截断） */
static bool name_is_83(const char *n)
{
    size_t len = strlen(n);
    if (len == 0 || len > 12) return false;          /* 8 + '.' + 3 */
    const char *dot = strrchr(n, '.');
    size_t body = dot ? (size_t)(dot - n) : len;
    size_t ext  = dot ? strlen(dot + 1) : 0;
    if (body == 0 || body > 8) return false;
    if (dot && (ext == 0 || ext > 3)) return false;
    for (const char *p = n; *p; p++) {
        if (*p == '.') continue;
        /* FATFS 在非 LFN 分支里明确拒绝这些字符 */
        if (!((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= 'a' && *p <= 'z') || *p == '_' || *p == '-')) return false;
    }
    return true;
}

/* "/fs/COVER/1A2B.565"。逐段校验，任一段不合规就拒绝（早失败好过调试
 * "为什么这个封面永远存不下来"）。 */
static bool build_path(char *out, size_t cap, const char *rel)
{
    if (rel == NULL || rel[0] == 0 || rel[0] == '/') return false;
    if (strlen(rel) + sizeof(STORE_BASE) + 1 > cap) return false;

    char tmp[80];
    strlcpy(tmp, rel, sizeof(tmp));
    for (char *seg = tmp, *nx = strchr(tmp, '/'); ; seg = nx + 1, nx = strchr(seg, '/')) {
        if (nx) *nx = 0;
        if (!name_is_83(seg)) {
            ESP_LOGE(TAG, "路径段 \"%s\" 不是合法 8.3 名（长文件名已关闭）", seg);
            return false;
        }
        if (nx == NULL) break;
    }
    snprintf(out, cap, STORE_BASE "/%s", rel);
    return true;
}

/* 临时名：同目录、同主名、扩展名换成 TMP（保证 rename 在同目录内，原子） */
static bool build_tmp(char *out, size_t cap, const char *rel)
{
    char path[80];
    if (!build_path(path, sizeof(path), rel)) return false;

    char *slash = strrchr(path, '/');
    char *base  = slash ? slash + 1 : path;          /* 只取主名 */
    char *dot   = strrchr(base, '.');
    if (dot) *dot = 0;                               /* 砍掉原扩展名 */
    if (snprintf(out, cap, "%s.TMP", path) >= (int)cap) return false;
    /* ⚠️ 只校验主名那一小段。目录段 build_path 已经校验过了 ——
     *    早先这里拿"整个相对路径"去校验，'/' 被判成非法字符，于是
     *    **所有子目录里的文件（封面缓存）写入都静默返回 INVALID_ARG**。
     *    教训：宁可多写一行日志，也别让这种失败无声无息。 */
    return name_is_83(base);
}

/* ---------------------------------------------------------------- 挂载 */

esp_err_t store_init(void)
{
    if (s_ready) return ESP_OK;
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) return ESP_ERR_NO_MEM;
    }

    esp_vfs_fat_mount_config_t cfg = {
        /* 第一次上电（或者分区是我们自己烧的空白）时分区里没有文件系统，
         * 必须能自动格式化，否则整台设备的缓存功能永远起不来。 */
        .format_if_mount_failed = true,
        .max_files              = STORE_MAX_FILES,
        .allocation_unit_size   = 0,
        .disk_status_check_enable = false,
        .use_one_fat            = false,
    };

    ESP_LOGW(TAG, "挂载 cache 分区到 " STORE_BASE " …");
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(STORE_BASE, PART_LABEL, &cfg, &s_wl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "挂载失败: %s —— 缓存功能关闭（最近播放/收藏不会落盘）",
                 esp_err_to_name(err));
        return err;
    }
    s_ready = true;

    /* 封面缓存目录（已存在时 mkdir 返回 EEXIST，正常） */
    if (mkdir(STORE_BASE "/COVER", 0777) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "建 COVER 目录失败（errno %d）—— 封面不落盘", errno);
    }

    ESP_LOGW(TAG, "cache 已挂载：%u KB 可用，%d 个封面缓存，写 %d 次",
             (unsigned)store_free_kb(), store_dir_count("COVER"), s_writes);
    return ESP_OK;
}

bool store_ready(void) { return s_ready; }

/* ---------------------------------------------------------------- 读 */

esp_err_t store_read(const char *rel, void *buf, size_t cap, size_t *len_out)
{
    if (len_out) *len_out = 0;
    if (!s_ready || buf == NULL) return ESP_ERR_INVALID_STATE;

    char path[80];
    if (!build_path(path, sizeof(path), rel)) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;                    /* 第一次运行很正常 */
    }
    size_t off = 0;
    esp_err_t err = ESP_OK;
    for (;;) {
        if (off == cap) {                            /* 缓冲区满了：探一下还有没有 */
            char probe;
            if (read(fd, &probe, 1) > 0) err = ESP_ERR_INVALID_SIZE;
            break;
        }
        ssize_t n = read(fd, (char *)buf + off, cap - off);
        if (n < 0) { s_last_errno = errno; err = ESP_FAIL; break; }
        if (n == 0) break;                           /* EOF */
        off += (size_t)n;
    }
    close(fd);
    xSemaphoreGive(s_lock);

    if (err == ESP_ERR_INVALID_SIZE) {
        ESP_LOGE(TAG, "%s 超过缓冲 %u 字节，丢弃（宁可报错也不交给上层半个 JSON）",
                 rel, (unsigned)cap);
        return err;
    }
    if (err == ESP_OK && len_out) *len_out = off;
    return err;
}

/* ---------------------------------------------------------------- 写 */

esp_err_t store_write(const char *rel, const void *buf, size_t len)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (len > STORE_MAX_WRITE) return ESP_ERR_INVALID_SIZE;

    char path[80], tmp[80];
    /* 这两条失败必须出声 —— 它们以前是静默返回，害得"封面缓存永远是 0 个"查了半天。
     * build_path/build_tmp 内部已经打了具体是哪一段不合规。 */
    if (!build_path(path, sizeof(path), rel)) {
        ESP_LOGE(TAG, "写 %s：路径不合法", rel);
        return ESP_ERR_INVALID_ARG;
    }
    if (!build_tmp(tmp, sizeof(tmp), rel)) {
        ESP_LOGE(TAG, "写 %s：临时名不合法", rel);
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        s_last_errno = errno;
        err = ESP_FAIL;
    } else {
        size_t off = 0;
        while (off < len) {
            ssize_t n = write(fd, (const char *)buf + off, len - off);
            if (n <= 0) { s_last_errno = errno; err = ESP_FAIL; break; }
            off += (size_t)n;
        }
        /* VFS 的 close 才真正 f_close（把目录项里的大小/时间刷下去），必须检查 */
        close(fd);
        errno = 0;
        if (err == ESP_OK && rename(tmp, path) != 0) {
            /* ⚠️ FATFS 的 rename 【不覆盖】已存在的目标：ff.c 的 f_rename 走
             *    follow_path(path_new) 发现同名就直接返回 FR_EXIST（errno 17）。
             *    不处理的话只有开机后第一次写得进去，之后每次都被拒
             *    —— 实测就是这么发现的（`写 STAR.SC 失败（errno 17）`）。
             *    所以先删目标再改名。中间有个极短的空窗（既无旧文件也无新文件），
             *    但**永远不会留下半个文件** —— 半个 JSON 才是真读不回来的那种损坏。 */
            if (errno == EEXIST) {
                unlink(path);
                if (rename(tmp, path) != 0) { s_last_errno = errno; err = ESP_FAIL; }
            } else {
                s_last_errno = errno;
                err = ESP_FAIL;
            }
        }
        if (err != ESP_OK) unlink(tmp);              /* 别留垃圾 TMP */
    }
    if (err == ESP_OK) s_writes++; else s_fails++;
    xSemaphoreGive(s_lock);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写 %s 失败（errno %d，累计 %d 次）", rel, s_last_errno, s_fails);
    }
    return err;
}

esp_err_t store_remove(const char *rel)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    char path[80];
    if (!build_path(path, sizeof(path), rel)) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int r = unlink(path);
    int e = errno;
    xSemaphoreGive(s_lock);
    if (r != 0) { s_last_errno = e; return ESP_ERR_NOT_FOUND; }
    return ESP_OK;
}

/* ---------------------------------------------------------------- 目录 */

int store_dir_count(const char *dir_rel)
{
    if (!s_ready) return 0;
    char dir[80];
    if (!build_path(dir, sizeof(dir), dir_rel)) return 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    DIR *d = opendir(dir);
    int n = 0;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_type == DT_DIR) continue;        /* 含 . / ..（FATFS 标 DT_DIR） */
            n++;
        }
        closedir(d);
    }
    xSemaphoreGive(s_lock);
    return n;
}

int store_evict(const char *dir_rel, int keep)
{
    if (!s_ready || keep < 0) return -1;
    char dir[80];
    if (!build_path(dir, sizeof(dir), dir_rel)) return -1;

    int n = store_dir_count(dir_rel);
    if (n <= keep) return 0;

    int deleted = 0, seen = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_type == DT_DIR) continue;
            /* 目录顺序 ≈ 写入先后（f_open 复用最靠前的空闲项），从前往后删作为 FIFO 淘汰 */
            if (++seen > n - keep) break;
            /* ⚠️ d_name 是 char[256]，直接用 %s 会被 -Werror=format-truncation 拦下。
             *    我们的名字都被校验成 8.3（≤12 字符），所以限宽既安全又说明意图。 */
            char full[80];
            int m = snprintf(full, sizeof(full), "%.60s/%.12s", dir, e->d_name);
            if (m < 0 || m >= (int)sizeof(full)) continue;
            if (unlink(full) == 0) deleted++;
        }
        closedir(d);
    }
    xSemaphoreGive(s_lock);

    ESP_LOGW(TAG, "%s 目录 %d 个 → 淘汰 %d 个（剩 %d）", dir_rel, n, deleted, n - deleted);
    return deleted;
}

uint64_t store_free_kb(void)
{
    if (!s_ready) return 0;
    uint64_t total = 0, free_b = 0;
    if (esp_vfs_fat_info(STORE_BASE, &total, &free_b) != ESP_OK) return 0;
    return free_b / 1024;
}

int store_write_count(void) { return s_writes; }
int store_fail_count(void)  { return s_fails; }
int store_last_errno(void)  { return s_last_errno; }
