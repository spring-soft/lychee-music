/*
 * qrpng —— 实现（说明见 include/qrpng.h）
 *
 * 一条主线：PNG chunk → IDAT 拼接 → zlib 解压 → 反滤波 → 定位图案打分定模块数
 *          → 采样成 n×n 点阵。
 * 没有任何"猜"的地方：模块数由三个定位图案投票选出，选不出来就报错不画。
 */
#include <stdlib.h>
#include <string.h>
#include "qrpng.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "miniz.h"          /* ROM 里的 miniz：只用到 tinfl_decompress_mem_to_mem */

static const char *TAG = "qrpng";

#define PNG_MAX_DIM   256                                    /* 二维码图不可能更大 */
#define PNG_IDAT_MAX  (32 * 1024)
#define FINDER_MIN    140                                    /* 147 分制：留 7 分容错 */

/* 7×7 定位图案：MSB 是最左边那格 */
static const uint8_t k_finder[7] = { 0x7F, 0x41, 0x5D, 0x5D, 0x5D, 0x41, 0x7F };

typedef struct {
    int      w, h;
    int      bitdepth;
    int      colortype;
    int      stride;        /* 每行像素数据字节数（不含开头那 1 字节 filter） */
    uint8_t *raw;           /* 反滤波后的行数据（含每行开头的 filter 字节） */
    const uint8_t *plte;    /* 调色板，3 字节/项；指向输入 PNG 缓冲 */
    int      plte_n;
} png_t;

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void *alloc_any(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (p == NULL) p = heap_caps_malloc(n, MALLOC_CAP_8BIT);
    return p;
}

/* 某个像素是不是"黑" */
static bool px_dark(const png_t *im, int x, int y)
{
    const uint8_t *line = im->raw + (size_t)y * (size_t)(im->stride + 1) + 1;
    int idx;

    if (im->bitdepth == 1) idx = (line[x >> 3] >> (7 - (x & 7))) & 1;
    else                   idx = line[x];

    if (im->colortype == 3) {                               /* 调色板：按亮度判 */
        if (im->plte == NULL || idx >= im->plte_n) return idx != 0;
        const uint8_t *c = im->plte + (size_t)idx * 3;
        return (c[0] * 299 + c[1] * 587 + c[2] * 114) / 1000 < 128;
    }
    /* 灰度：1bit 时 0 = 黑；8bit 时按 128 分界 */
    if (im->bitdepth == 1) return idx == 0;
    return idx < 128;
}

/* 标准 PNG 反滤波。bpp = 一个像素至少占几个字节（这里恒为 1） */
static bool png_unfilter(uint8_t *buf, int h, int stride, int bpp)
{
    uint8_t *prev = NULL;
    for (int y = 0; y < h; y++) {
        uint8_t *line = buf + (size_t)y * (size_t)(stride + 1);
        int ft = line[0];
        uint8_t *cur = line + 1;
        switch (ft) {
        case 0:
            break;
        case 1:
            for (int i = bpp; i < stride; i++) cur[i] = (uint8_t)(cur[i] + cur[i - bpp]);
            break;
        case 2:
            if (prev) for (int i = 0; i < stride; i++) cur[i] = (uint8_t)(cur[i] + prev[i]);
            break;
        case 3:
            for (int i = 0; i < stride; i++) {
                int a = (i >= bpp) ? cur[i - bpp] : 0;
                int b = prev ? prev[i] : 0;
                cur[i] = (uint8_t)(cur[i] + ((a + b) >> 1));
            }
            break;
        case 4:
            for (int i = 0; i < stride; i++) {
                int a = (i >= bpp) ? cur[i - bpp] : 0;
                int b = prev ? prev[i] : 0;
                int c = (prev && i >= bpp) ? prev[i - bpp] : 0;
                int pa = abs(b - c), pb = abs(a - c), pc = abs(a + b - 2 * c);
                int pr = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                cur[i] = (uint8_t)(cur[i] + pr);
            }
            break;
        default:
            ESP_LOGW(TAG, "未知的行滤波类型 %d", ft);
            return false;
        }
        prev = cur;
    }
    return true;
}

/* 按"假设这张图是 n×n 个模块"去取第 (i,j) 格的中心像素。
 * 用整数运算：(2i+1)/(2n) 就是模块中心的比例位置 —— 避免浮点。 */
static bool sample_dark(const png_t *im, int x0, int y0, int cw, int ch,
                        int n, int i, int j)
{
    if (i < 0 || j < 0 || i >= n || j >= n) return false;
    int x = x0 + (int)(((int64_t)(2 * i + 1) * cw) / (2 * n));
    int y = y0 + (int)(((int64_t)(2 * j + 1) * ch) / (2 * n));
    if (x >= im->w) x = im->w - 1;
    if (y >= im->h) y = im->h - 1;
    return px_dark(im, x, y);
}

esp_err_t qrpng_decode(const uint8_t *png, size_t len, qrpng_t *out)
{
    static const uint8_t SIG[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

    if (png == NULL || out == NULL || len < 8) return ESP_ERR_INVALID_ARG;
    if (memcmp(png, SIG, 8) != 0) return ESP_ERR_INVALID_RESPONSE;

    png_t im;
    memset(&im, 0, sizeof(im));
    int      ct = -1;
    const uint8_t *plte = NULL;
    int      plte_n = 0;
    uint8_t *idat = NULL;
    size_t   idat_len = 0, idat_cap = 0;
    esp_err_t ret = ESP_ERR_INVALID_RESPONSE;

    /* ---------------- 1. chunk 遍历 ---------------- */
    size_t p = 8;
    while (p + 12 <= len) {
        uint32_t clen = be32(png + p);
        const uint8_t *type = png + p + 4;
        if ((size_t)clen > len - p - 12) break;              /* 截断 */
        const uint8_t *data = png + p + 8;

        if (memcmp(type, "IHDR", 4) == 0) {
            if (clen != 13) goto done;
            im.w = (int)be32(data);
            im.h = (int)be32(data + 4);
            im.bitdepth  = data[8];
            im.colortype = data[9];
            ct = im.colortype;
            if (data[10] != 0 || data[11] != 0) goto done;   /* 压缩/滤波方法必须为 0 */
            if (data[12] != 0) {
                ESP_LOGW(TAG, "隔行 PNG 不认识（interlace=%d）", data[12]);
                ret = ESP_ERR_NOT_SUPPORTED;
                goto done;
            }
        } else if (memcmp(type, "PLTE", 4) == 0) {
            plte = data;
            plte_n = (int)(clen / 3);
        } else if (memcmp(type, "IDAT", 4) == 0) {
            if (idat_len + clen > PNG_IDAT_MAX) {
                ESP_LOGW(TAG, "IDAT 超过 %d 字节", PNG_IDAT_MAX);
                ret = ESP_ERR_NOT_SUPPORTED;
                goto done;
            }
            if (idat_len + clen > idat_cap) {
                size_t ncap = idat_cap ? idat_cap * 2 : 4096;
                while (ncap < idat_len + clen) ncap *= 2;
                uint8_t *nb = realloc(idat, ncap);
                if (nb == NULL) { ret = ESP_ERR_NO_MEM; goto done; }
                idat = nb;
                idat_cap = ncap;
            }
            memcpy(idat + idat_len, data, clen);
            idat_len += clen;
        } else if (memcmp(type, "IEND", 4) == 0) {
            break;
        }
        p += 12 + clen;
    }

    /* ---------------- 2. 形态校验 ---------------- */
    if (im.w <= 0 || im.h <= 0 || im.w > PNG_MAX_DIM || im.h > PNG_MAX_DIM) {
        ESP_LOGW(TAG, "尺寸不认识：%dx%d", im.w, im.h);
        ret = ESP_ERR_NOT_SUPPORTED;
        goto done;
    }
    if (!((ct == 3 || ct == 0) && (im.bitdepth == 1 || im.bitdepth == 8))) {
        ESP_LOGW(TAG, "PNG 形态不认识：color_type=%d bit_depth=%d", ct, im.bitdepth);
        ret = ESP_ERR_NOT_SUPPORTED;
        goto done;
    }
    if (ct == 3 && plte == NULL) {
        ESP_LOGW(TAG, "调色板 PNG 里没有 PLTE 块");
        ret = ESP_ERR_NOT_SUPPORTED;
        goto done;
    }
    if (idat_len == 0) goto done;

    im.stride  = (im.w * im.bitdepth + 7) / 8;
    im.plte    = plte;
    im.plte_n  = plte_n;

    /* ---------------- 3. 解压 + 反滤波 ---------------- */
    size_t need = (size_t)im.h * (size_t)(im.stride + 1);
    im.raw = alloc_any(need);
    if (im.raw == NULL) { ret = ESP_ERR_NO_MEM; goto done; }

    size_t got = tinfl_decompress_mem_to_mem(im.raw, need, idat, idat_len,
                                             TINFL_FLAG_PARSE_ZLIB_HEADER);
    if (got == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) {
        ESP_LOGW(TAG, "zlib 解压失败（IDAT %u 字节）", (unsigned)idat_len);
        goto done;
    }
    if (got != need) {
        ESP_LOGW(TAG, "解压后 %u 字节，期望 %u（%dx%d bd=%d）",
                 (unsigned)got, (unsigned)need, im.w, im.h, im.bitdepth);
        goto done;
    }
    if (!png_unfilter(im.raw, im.h, im.stride, (im.bitdepth + 7) / 8)) goto done;

    /* ---------------- 4. 裁到墨迹范围（这张图本来没有静区，但别假设） ---------------- */
    int x0 = im.w, x1 = -1, y0 = im.h, y1 = -1;
    for (int y = 0; y < im.h; y++) {
        for (int x = 0; x < im.w; x++) {
            if (!px_dark(&im, x, y)) continue;
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
        }
    }
    if (x1 < 0) {
        ESP_LOGW(TAG, "图里一个黑像素都没有");
        ret = ESP_ERR_NOT_FOUND;
        goto done;
    }
    int cw = x1 - x0 + 1, ch = y1 - y0 + 1;

    /* ---------------- 5. 定位图案打分，定模块数 ---------------- */
    int best_n = 0, best_total = -1, best_finder = 0;
    for (int n = 21; n <= QRPNG_MAX_MODULES; n += 4) {
        int fs = 0;
        const int ox[3] = { 0, n - 7, 0 };
        const int oy[3] = { 0, 0, n - 7 };
        for (int k = 0; k < 3; k++) {
            for (int r = 0; r < 7; r++) {
                for (int c = 0; c < 7; c++) {
                    int want = (k_finder[r] >> (6 - c)) & 1;
                    bool got = sample_dark(&im, x0, y0, cw, ch, n, ox[k] + c, oy[k] + r);
                    if (want == (int)got) fs++;
                }
            }
        }
        int ts = 0;                                          /* 时序图案：i 偶数格为黑 */
        for (int i = 8; i <= n - 9; i++) {
            int want = ((i & 1) == 0);
            if (want == (int)sample_dark(&im, x0, y0, cw, ch, n, i, 6)) ts++;
            if (want == (int)sample_dark(&im, x0, y0, cw, ch, n, 6, i)) ts++;
        }
        int total = fs * 4 + ts;                             /* 定位图案权重更高 */
        if (total > best_total) {
            best_total = total;
            best_n = n;
            best_finder = fs;
        }
    }

    ESP_LOGI(TAG, "PNG %dx%d bd=%d ct=%d，墨迹 %dx%d → 模块数 %d（定位图案 %d/147）",
             im.w, im.h, im.bitdepth, ct, cw, ch, best_n, best_finder);
    if (best_finder < FINDER_MIN) {
        ESP_LOGW(TAG, "定位图案对不上（最好只有 %d/147）—— 这不像一张二维码",
                 best_finder);
        ret = ESP_ERR_NOT_FOUND;
        goto done;
    }

    /* ---------------- 6. 采样成点阵 ---------------- */
    out->n = best_n;
    memset(out->row, 0, sizeof(out->row));
    for (int j = 0; j < best_n; j++) {
        for (int i = 0; i < best_n; i++) {
            if (sample_dark(&im, x0, y0, cw, ch, best_n, i, j)) {
                out->row[j][i >> 3] |= (uint8_t)(0x80 >> (i & 7));
            }
        }
    }
    ret = ESP_OK;

done:
    if (im.raw) heap_caps_free(im.raw);
    free(idat);
    return ret;
}

void qrpng_dump(const qrpng_t *q)
{
    if (q == NULL || q->n <= 0) return;
    ESP_LOGI(TAG, "点阵 %dx%d（# = 黑）：", q->n, q->n);
    for (int j = 0; j < q->n; j++) {
        char line[QRPNG_MAX_MODULES + 1];
        for (int i = 0; i < q->n; i++) line[i] = qrpng_dark(q, i, j) ? '#' : '.';
        line[q->n] = 0;
        ESP_LOGI(TAG, "  %s", line);
    }
}
