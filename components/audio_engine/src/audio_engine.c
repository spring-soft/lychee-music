/*
 * ESP-Music 音频引擎实现
 *
 * 刻意不使用 GMF / esp_audio_simple_player：老实现里 FLAC 直传必崩（GMF 的
 * 输出缓冲"释放→重申请"路径有内存破坏 bug），而且拿不到真实播放位置。
 * 这里只用 esp_audio_codec 的 simple decoder，缓冲和任务全由我们自己管。
 */
#include <string.h>
#include <inttypes.h>
#include "audio_engine.h"
#include "audio_ring.h"
#include "board.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_types.h"

static const char *TAG = "ae";

/* ---------------- 缓冲与参数 ---------------- */
#define RING_IN_BYTES        (128 * 1024)   /* 压缩流预取（沿用实测有效的 128KB） */
#define RING_PCM_BYTES       (128 * 1024)   /* 解码后 PCM（44.1k/16bit/stereo ≈ 740ms） */
/* 起播前先攒多少 PCM。⚠️ 实测 32KB(186ms) 走网络流时不够 —— DMA 里本身就压着 185ms，
 *    网络一抖动就 underrun（实测 MP3 over HTTP 出现 12 次）。96KB ≈ 545ms 才稳。 */
#define PCM_PREBUFFER_BYTES  (96 * 1024)
#define DEC_STAGE_INIT       (32 * 1024)
#define DEC_STAGE_MAX        (128 * 1024)   /* FLAC 单帧远大于 MP3，必须能长 */
#define PCM_OUT_CAP          (256 * 1024)
#define SILENCE_CHUNK_MS     10
#define TASK_STACK_FETCH     6144
#define TASK_STACK_DECODE    8192
#define TASK_STACK_OUT       4096

/* ---------------- 状态 ---------------- */
static audio_ring_t s_rin, s_rpcm;
static ae_source_t *s_src;
static TaskHandle_t s_t_fetch, s_t_dec, s_t_out;
static bool s_inited;

static volatile uint32_t           s_gen;
static volatile ae_state_t         s_state = AE_STATE_IDLE;
static volatile bool               s_fetch_eof;     /* 源已读完 */
static volatile bool               s_dec_eof;       /* 解码器已冲完尾 */
static volatile bool               s_sniff_done;
static volatile bool               s_error;
static volatile bool               s_paused;
static volatile uint32_t           s_frames_out;    /* 已交给 DMA 的 PCM 帧数 */
/* seek 用：跳到 90 秒处时，服务端从这个位置开始发流，所以引擎自己数的帧是从 0 开始的，
 * 真实播放位置要加上这个基准。每首歌（含 seek）由 player 设置。 */
static volatile uint32_t           s_pos_base_ms;
static volatile uint32_t           s_frames_decoded;
static volatile uint32_t           s_underruns;
static volatile uint32_t           s_fs = 44100;
static volatile uint8_t            s_ch = 2;
static volatile uint8_t            s_bits = 16;
static volatile esp_audio_simple_dec_type_t s_type = ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;

/* eos 冲尾时 len=0，但 buffer 不能传 NULL（FLAC parser 会报 Invalid argument） */
static uint8_t s_eos_dummy[4];

/* 内嵌测试音频符号（EMBED_FILES 生成） */
extern const uint8_t test_melody_mp3_start[]  asm("_binary_test_melody_mp3_start");
extern const uint8_t test_melody_mp3_end[]    asm("_binary_test_melody_mp3_end");
extern const uint8_t test_melody_flac_start[] asm("_binary_test_melody_flac_start");
extern const uint8_t test_melody_flac_end[]   asm("_binary_test_melody_flac_end");

/* ============================================================ 格式嗅探 */

/* 不看 URL 扩展名（老实现就因为 /rest/stream.view?... 取不到 ".mp3" 而回落成 MP3 提示）。
 * 这里按文件头字节判断，顺序很重要：AAC(ADTS) 与 MP3 的同步字都是 0xFF 0xEx。 */
static esp_audio_simple_dec_type_t ae_sniff(const uint8_t *b, size_t n)
{
    if (n < 12) return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
    if (!memcmp(b, "fLaC", 4))                              return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
    if (!memcmp(b, "OggS", 4))                              return ESP_AUDIO_SIMPLE_DEC_TYPE_OGG;
    if (!memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WAVE", 4)) return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
    if (!memcmp(b + 4, "ftyp", 4))                          return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
    if (b[0] == 0xFF && (b[1] & 0xF6) == 0xF0)              return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
    if (b[0] == 0xFF && (b[1] & 0xE0) == 0xE0)              return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    if (!memcmp(b, "ID3", 3))                               return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    /* 都不匹配：让上层按 MP3 试（simple_dec 内部还会再嗅探一次） */
    ESP_LOGW(TAG, "[sniff] 未识别文件头 %02X %02X %02X %02X，按 MP3 试",
             b[0], b[1], b[2], b[3]);
    return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
}

/* ==================================================== PCM 归一化到 16bit 立体声 */

static size_t ae_normalize(const uint8_t *src, size_t bytes, uint8_t bits, uint8_t ch,
                           uint8_t *dst, size_t dst_cap)
{
    size_t out = 0;
    if (bits == 16 && ch == 2) {
        if (bytes > dst_cap) bytes = dst_cap;
        memcpy(dst, src, bytes);
        return bytes;
    }
    if (bits == 16 && ch == 1) {
        size_t frames = bytes / 2;
        for (size_t i = 0; i < frames; i++) {
            if (out + 4 > dst_cap) break;
            dst[out++] = src[i * 2];
            dst[out++] = src[i * 2 + 1];
            dst[out++] = src[i * 2];
            dst[out++] = src[i * 2 + 1];
        }
        return out;
    }
    uint8_t in_bytes = bits / 8;
    if ((bits == 24 || bits == 32) && (ch == 1 || ch == 2)) {
        size_t frames = bytes / (in_bytes * ch);
        size_t off = in_bytes - 2;            /* 取高 16 位 */
        for (size_t i = 0; i < frames; i++) {
            const uint8_t *f = src + i * in_bytes * ch;
            if (out + 4 > dst_cap) break;
            dst[out++] = f[off];
            dst[out++] = f[off + 1];
            if (ch == 2) {
                dst[out++] = f[in_bytes + off];
                dst[out++] = f[in_bytes + off + 1];
            } else {
                dst[out++] = f[off];
                dst[out++] = f[off + 1];
            }
        }
        return out;
    }
    ESP_LOGW(TAG, "[pcm] 暂不支持 %u bit / %u ch 的归一化，丢弃 %u 字节",
             bits, ch, (unsigned)bytes);
    return 0;
}

/* ================================================================== 任务 */

static volatile bool s_fetch_eof_local;   /* fetch 任务自己的（按 gen 复位） */

static void fetch_task(void *arg)
{
    uint32_t my_gen = 0;
    bool sniffed = false;
    static uint8_t probe[8192];

    for (;;) {
        if (s_gen != my_gen) {
            my_gen = s_gen;
            sniffed = false;
            s_fetch_eof_local = false;
        }
        if (s_state == AE_STATE_IDLE || s_state == AE_STATE_ERROR ||
            s_state == AE_STATE_FINISHED || s_src == NULL) {
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }

        if (s_fetch_eof_local) {          /* 流已读完：别再反复调 read()（会刷屏且无意义） */
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (!sniffed) {
            int n = s_src->read(s_src, probe, sizeof(probe));
            if (n <= 0) {
                if (n == 0) { s_fetch_eof = true; s_sniff_done = true; }
                else        { s_error = true; s_fetch_eof = true; s_sniff_done = true; }
                s_fetch_eof_local = true;
                vTaskDelay(pdMS_TO_TICKS(30));
                continue;
            }
            s_type = ae_sniff(probe, (size_t)n);
            ESP_LOGI(TAG, "[sniff] %s（前 4 字节 %02X %02X %02X %02X，探测 %d 字节）",
                     esp_audio_simple_dec_get_name(s_type),
                     probe[0], probe[1], probe[2], probe[3], n);
            size_t w = audio_ring_write(&s_rin, probe, (size_t)n, pdMS_TO_TICKS(500));
            if (w != (size_t)n) ESP_LOGW(TAG, "[fetch] 探测数据只写入 %u/%d 字节", (unsigned)w, n);
            s_sniff_done = true;
            sniffed = true;
            continue;
        }

        uint8_t *dst = NULL;
        size_t span = audio_ring_write_span(&s_rin, &dst, pdMS_TO_TICKS(100));
        if (span == 0) continue;                 /* 满/超时，重试 */

        int n = s_src->read(s_src, dst, span);
        if (n > 0) {
            audio_ring_write_commit(&s_rin, (size_t)n);
        } else if (n == 0) {
            if (!s_fetch_eof_local) {
                ESP_LOGI(TAG, "[fetch] 流结束，ring_in 剩 %u KB 待解码",
                         (unsigned)(audio_ring_used(&s_rin) / 1024));
            }
            s_fetch_eof = true;
            s_fetch_eof_local = true;
            vTaskDelay(pdMS_TO_TICKS(30));
        } else {
            ESP_LOGE(TAG, "[fetch] 源读取出错");
            s_error = true;
            s_fetch_eof = true;
            s_fetch_eof_local = true;
            vTaskDelay(pdMS_TO_TICKS(30));
        }
    }
}

static void decode_task(void *arg)
{
    uint32_t my_gen = 0;
    esp_audio_simple_dec_handle_t dec = NULL;
    uint8_t *stage = NULL, *pcm_out = NULL;
    uint32_t stage_cap = 0;
    int open_fails = 0;
    int eos_retries = 0;

    for (;;) {
        if (s_gen != my_gen) {
            if (dec) { esp_audio_simple_dec_close(dec); dec = NULL; }
            if (stage)   { heap_caps_free(stage);   stage = NULL; }
            if (pcm_out) { heap_caps_free(pcm_out); pcm_out = NULL; }
            stage_cap = 0;
            open_fails = 0;
            eos_retries = 0;
            my_gen = s_gen;
        }
        if (s_state == AE_STATE_IDLE || s_state == AE_STATE_ERROR ||
            s_state == AE_STATE_FINISHED) {
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }
        if (!s_sniff_done) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (s_dec_eof)     { vTaskDelay(pdMS_TO_TICKS(30)); continue; }

        if (dec == NULL) {
            if (s_type == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
            esp_audio_simple_dec_cfg_t cfg = {
                .dec_type      = s_type,
                .dec_cfg       = NULL,
                .cfg_size      = 0,
                .use_frame_dec = false,   /* FLAC/OGG/M4A 必须 false（要 parser） */
            };
            esp_audio_err_t e = esp_audio_simple_dec_open(&cfg, &dec);
            if (e != ESP_AUDIO_ERR_OK) {
                open_fails++;
                ESP_LOGE(TAG, "[dec] 打开解码器失败: %d（第 %d 次）", e, open_fails);
                if (open_fails >= 3) {
                    ESP_LOGE(TAG, "[dec] 连续失败，判定为致命错误并停止重试");
                    s_error = true;
                    s_state = AE_STATE_ERROR;
                }
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
            open_fails = 0;
            stage_cap = DEC_STAGE_INIT;
            stage   = heap_caps_malloc(stage_cap, MALLOC_CAP_SPIRAM);
            pcm_out = heap_caps_malloc(PCM_OUT_CAP, MALLOC_CAP_SPIRAM);
            if (!stage || !pcm_out) {
                ESP_LOGE(TAG, "[dec] 暂存缓冲分配失败");
                s_error = true;
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
        }

        const uint8_t *in = NULL;
        size_t in_len = audio_ring_read_span(&s_rin, &in, pdMS_TO_TICKS(100));
        bool eos = false;
        if (in_len == 0) {
            if (!s_fetch_eof) continue;          /* 还在等数据 */
            eos = true;                          /* 喂 eos 把 parser 缓存冲出来 */
        } else if (s_fetch_eof && audio_ring_used(&s_rin) == in_len) {
            eos = true;                          /* 最后一块 */
        }

        esp_audio_simple_dec_raw_t raw = {
            /* ⚠️ eos 冲尾时 len=0，但 buffer 不能是 NULL ——
             * FLAC 的 parser 会报 "Invalid argument ... in buffer 0x0" 并拒绝处理 */
            .buffer = (uint8_t *)(in ? in : s_eos_dummy),
            .len    = (uint32_t)in_len,
            .eos    = eos,
        };
        esp_audio_simple_dec_out_t out = { .buffer = stage, .len = stage_cap };
        esp_audio_err_t err = esp_audio_simple_dec_process(dec, &raw, &out);

        if (err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            /* FLAC 单帧可以很大 —— 必须按 needed_size 长缓冲，不能按 MP3 假设定死 */
            uint32_t need = out.needed_size ? out.needed_size : stage_cap * 2;
            if (need > DEC_STAGE_MAX) need = DEC_STAGE_MAX;
            if (need > stage_cap) {
                uint8_t *ns = heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
                if (ns) {
                    heap_caps_free(stage);
                    stage = ns;
                    stage_cap = need;
                    ESP_LOGW(TAG, "[dec] 输出缓冲扩到 %u KB", (unsigned)(need / 1024));
                } else {
                    ESP_LOGE(TAG, "[dec] 扩容失败 (%u KB)", (unsigned)(need / 1024));
                    s_error = true;
                }
            } else {
                ESP_LOGE(TAG, "[dec] BUFF_NOT_ENOUGH 但已到上限 %u KB", (unsigned)(DEC_STAGE_MAX / 1024));
                s_error = true;
            }
            continue;
        } else if (err == ESP_AUDIO_ERR_DATA_LACK) {
            if (eos) {
                /* 冲尾：parser 缓存里可能还有最后一帧，多喂几次 eos 给它机会吐出来。
                 * （只喂一次实测会少放 ~89ms，正好一个 FLAC 帧） */
                if (eos_retries++ < 4) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }
                s_dec_eof = true;
                ESP_LOGI(TAG, "[dec] 解码收尾：共 %" PRIu32 " ms（eos 重试 %d 次）",
                         s_frames_decoded * 1000 / s_fs, eos_retries);
            } else {
                /* ⚠️ 只在生产者已停止时才敢搬运数据：audio_ring_defrag() 会重置 rd/wr，
                 *   而 fetch 任务可能正拿旧的 wr 往同一块区域写 → 数据错位（听感=咔哒）。
                 *   流还在来的时候就不搬：simple_dec 接受任意长度输入，能自己吃下被回绕切开的帧。 */
                if (s_fetch_eof) audio_ring_defrag(&s_rin);
                vTaskDelay(pdMS_TO_TICKS(5));
            }
            continue;
        } else if (err != ESP_AUDIO_ERR_OK && err != ESP_AUDIO_ERR_CONTINUE) {
            ESP_LOGE(TAG, "[dec] 解码错误 %d", err);
            s_error = true;
            continue;
        }

        if (raw.consumed) audio_ring_read_commit(&s_rin, raw.consumed);

        if (out.decoded_size) {
            eos_retries = 0;                     /* 有输出说明还在正常解，冲尾计数归零 */
            if (s_frames_decoded == 0) {
                esp_audio_simple_dec_info_t info = {0};
                if (esp_audio_simple_dec_get_info(dec, &info) == ESP_AUDIO_ERR_OK) {
                    s_fs   = info.sample_rate;
                    s_ch   = info.channel;
                    s_bits = info.bits_per_sample;
                    ESP_LOGI(TAG, "[dec] 格式: %" PRIu32 " Hz / %u ch / %u bit / %" PRIu32 " bps / 帧 %" PRIu32,
                             info.sample_rate, info.channel, info.bits_per_sample,
                             info.bitrate, info.frame_size);
                    if (board_i2s_get_rate() != info.sample_rate) {
                        board_i2s_set_rate(info.sample_rate);   /* 换 I2S 时钟，不做重采样 */
                    }
                }
            }
            size_t pcm_bytes = ae_normalize(stage, out.decoded_size, s_bits, s_ch,
                                            pcm_out, PCM_OUT_CAP);
            if (pcm_bytes) {
                /* ★ 关键：ring_pcm 由 out 任务【按实时速度】消费，解码器比实时快得多，
                 *   所以这里必须一直等到写完为止 —— 绝不能超时就丢掉解码好的 PCM
                 *   （超时丢数据会静默少放几秒，实测 FLAC 12 秒的歌只放了 9.4 秒）。
                 *   背压是正常现象，不是错误。 */
                size_t off = 0;
                while (off < pcm_bytes) {
                    size_t w = audio_ring_write(&s_rpcm, pcm_out + off, pcm_bytes - off,
                                                pdMS_TO_TICKS(200));
                    if (w > 0) {
                        off += w;
                        continue;
                    }
                    /* 一点没写进去：只有在切歌/停止时才放弃，否则继续等 */
                    if (s_gen != my_gen || s_state == AE_STATE_IDLE || s_state == AE_STATE_ERROR) {
                        ESP_LOGW(TAG, "[dec] 切歌/停止，丢弃剩余 %u 字节 PCM",
                                 (unsigned)(pcm_bytes - off));
                        break;
                    }
                }
                s_frames_decoded += pcm_bytes / 4;   /* 16bit stereo */
            }
        }
        if (eos && raw.consumed == 0 && out.decoded_size == 0) {
            s_dec_eof = true;
        }

        vTaskDelay(1);   /* ★ 关键：把 UI 的最坏响应延迟钉在 1 tick */
    }
}

static uint8_t s_silence[SILENCE_CHUNK_MS * 44100 / 1000 * 4];   /* 10ms @44.1k 立体声 16bit */

static void out_task(void *arg)
{
    uint32_t my_gen = 0;
    bool buffering = true;
    bool drain_waited = false;
    uint32_t underrun_logged = 0;

    for (;;) {
        if (s_gen != my_gen) {
            my_gen = s_gen;
            buffering = true;
            drain_waited = false;
        }
        if (s_state == AE_STATE_IDLE || s_state == AE_STATE_ERROR ||
            s_state == AE_STATE_FINISHED) {
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }
        if (s_paused) {
            size_t w = 0;
            board_i2s_write(s_silence, sizeof(s_silence), &w, pdMS_TO_TICKS(200));
            continue;                            /* 位置不累加 → 冻结 */
        }
        if (buffering) {
            /* 攒够 PCM 才起播；若流已结束（短音频）就按现有的直接起播 */
            if (audio_ring_used(&s_rpcm) >= PCM_PREBUFFER_BYTES || s_dec_eof) {
                buffering = false;
                s_state = AE_STATE_PLAYING;
                ESP_LOGW(TAG, "[out] 起播：PCM 预取 %u KB，ring_in 里还有 %u KB，DMA 压着 %u 帧（≈%u ms）",
                         (unsigned)(audio_ring_used(&s_rpcm) / 1024),
                         (unsigned)(audio_ring_used(&s_rin) / 1024),
                         (unsigned)BOARD_I2S_DMA_FRAMES,
                         (unsigned)(BOARD_I2S_DMA_FRAMES * 1000 / s_fs));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
        }

        const uint8_t *p = NULL;
        size_t n = audio_ring_read_span(&s_rpcm, &p, pdMS_TO_TICKS(100));
        /* ⚠️ 单次写入上限。ring_pcm 有 128KB，一次可能拿到 743ms 的连续 PCM，
         *    而 board_i2s_write 是【阻塞】的（要等 DMA 排空）。于是"按暂停"如果
         *    正好落在这么一次大写入中间，那 743ms 会被一次性喂进 DMA，
         *    位置（= 已交给 DMA 的帧数 - DMA 深度）就跟着一次跳过去 0.7 秒 ——
         *    表现为"按了暂停，进度条还往前蹿一下"。
         *    限到 16KB（≈92ms）后，暂停最多迟约 90ms 生效。 */
        #define OUT_WRITE_MAX (16 * 1024)
        if (n > OUT_WRITE_MAX) n = OUT_WRITE_MAX;
        if (n == 0) {
            if (s_dec_eof) {
                /* 解码结束且缓冲已空，但 DMA 里还压着 ~185ms 音频没放完。
                 * 等它放完再宣布结束 —— 这样"播完"事件才和耳朵听到的一致
                 * （M4 的自动下一首要靠它，否则会提前切歌）。 */
                if (!drain_waited) {
                    drain_waited = true;
                    uint32_t wait_ms = BOARD_I2S_DMA_FRAMES * 1000 / s_fs;
                    ESP_LOGI(TAG, "[out] 缓冲已空，等 DMA 把最后 %" PRIu32 " ms 放完", wait_ms);
                    vTaskDelay(pdMS_TO_TICKS(wait_ms));
                    continue;
                }
                s_state = AE_STATE_FINISHED;
                ESP_LOGI(TAG, "[out] 播放结束：位置 %" PRIu32 " ms / underrun %" PRIu32 " 次 / ring_in 高水位 %u KB",
                         audio_engine_position_ms(), s_underruns,
                         (unsigned)(s_rin.max_used / 1024));
                continue;
            }
            s_underruns++;                        /* 欠载：补静音，不让 I2S 停（避免爆音） */
            size_t w = 0;
            board_i2s_write(s_silence, sizeof(s_silence), &w, pdMS_TO_TICKS(200));
            if (underrun_logged++ < 8) {
                ESP_LOGW(TAG, "[out] 欠载 #%" PRIu32 "（ring_in %u KB / ring_pcm %u KB / fetch%s）",
                         s_underruns,
                         (unsigned)(audio_ring_used(&s_rin) / 1024),
                         (unsigned)(audio_ring_used(&s_rpcm) / 1024),
                         s_fetch_eof ? "已完成" : "还在拉流");
            }
            continue;
        }

        size_t w = 0;
        /* 超时给足：一段最多 128KB（743ms @44.1k），写进 DMA 需要等 DMA 排空 */
        esp_err_t e = board_i2s_write(p, n, &w, pdMS_TO_TICKS(5000));
        if (w > 0) {
            /* ★ 即使返回错误也要 commit 已写进去的部分！
             *   否则下一轮会把同样的 PCM 再写一遍 → 波形重复一小段 = 咔哒声 */
            audio_ring_read_commit(&s_rpcm, w);
            s_frames_out += w / 4;               /* 16bit stereo → 每帧 4 字节 */
        }
        if (e != ESP_OK || w < n) {
            s_underruns++;
            ESP_LOGW(TAG, "[out] I2S 写入异常: %s（%u/%u 字节）", esp_err_to_name(e),
                     (unsigned)w, (unsigned)n);
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

/* ================================================================== 对外 API */

esp_err_t audio_engine_init(void)
{
    if (s_inited) return ESP_OK;
    if (audio_ring_init(&s_rin,  RING_IN_BYTES,  "ring_in")  != ESP_OK) return ESP_ERR_NO_MEM;
    if (audio_ring_init(&s_rpcm, RING_PCM_BYTES, "ring_pcm") != ESP_OK) return ESP_ERR_NO_MEM;
    /* ⚠️ 两层都要注册：
     *   1) esp_audio_dec_register_default()    —— 底层真正的 MP3/AAC/FLAC 解码器
     *   2) esp_audio_simple_dec_register_default() —— 上层的解析器包装
     * 只注册第 2 个的话，open() 会以 -7 (NOT_SUPPORT) 失败并报
     * "Decoder MP3(540233805) not registered"。 */
    if (esp_audio_dec_register_default() != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "底层解码器注册失败");
        return ESP_FAIL;
    }
    if (esp_audio_simple_dec_register_default() != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "simple 解码器注册失败");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "解码器已注册（MP3/AAC/FLAC/PCM 在 Kconfig 里开）");
    memset(s_silence, 0, sizeof(s_silence));
    s_inited = true;
    ESP_LOGI(TAG, "引擎就绪：ring_in 128KB / ring_pcm 128KB（都在 PSRAM），预取阈值 %u KB",
             (unsigned)(PCM_PREBUFFER_BYTES / 1024));
    return ESP_OK;
}

esp_err_t audio_engine_play(ae_source_t *src)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;

    if (s_t_fetch == NULL) {
        /* ⚠️ 栈放哪：内部 SRAM 是这块板唯一紧的资源（加蓝牙后实测直接见底、
         *    崩溃重启），所以把【不碰 flash 写】的任务栈搬到 PSRAM。
         *    - decode / fetch：只用环形缓冲和网络，写 flash 的事一件不干 → 安全
         *    - ⚠️ out 特意【不搬】：它是 19 优先级的实时任务，栈在 PSRAM 会让
         *      上下文切换多走一次外部 RAM，而它是"欠载"的唯一防线，不值得为
         *      4KB 冒这个险。同理 player 任务要写 NVS/FATFS，也必须留在内部。
         *    （PSRAM 栈由 CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY 支持，
         *      别的任务写 flash 时会 halt 另一个核，所以不会撞上"cache 关了
         *      却去访问 PSRAM"那种崩溃。） */
        xTaskCreatePinnedToCore(out_task,    "ae_out",    TASK_STACK_OUT,    NULL, 19, &s_t_out,   1);
        xTaskCreatePinnedToCoreWithCaps(decode_task, "ae_decode", TASK_STACK_DECODE, NULL, 8, &s_t_dec, 1, MALLOC_CAP_SPIRAM);
        xTaskCreatePinnedToCoreWithCaps(fetch_task,  "ae_fetch",  TASK_STACK_FETCH,  NULL, 6, &s_t_fetch, 0, MALLOC_CAP_SPIRAM);
        ESP_LOGI(TAG, "三个任务已创建（out=core1/prio19, decode=core1/prio8, fetch=core0/prio6）");
    }

    audio_engine_stop();

    s_src = src;
    if (s_src && s_src->open) {
        esp_err_t e = s_src->open(s_src, NULL);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "源打开失败: %s", esp_err_to_name(e));
            s_state = AE_STATE_ERROR;
            return e;
        }
    }

    audio_ring_reset(&s_rin);
    audio_ring_reset(&s_rpcm);
    s_fetch_eof = false;
    s_dec_eof = false;
    s_sniff_done = false;
    s_error = false;
    s_paused = false;
    s_frames_out = 0;
    s_frames_decoded = 0;
    s_pos_base_ms = 0;
    s_underruns = 0;
    s_type = ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
    s_gen++;                                  /* 通知任务：新的一首 */
    s_state = AE_STATE_BUFFERING;
    ESP_LOGI(TAG, "开始播放（gen=%" PRIu32 "）", s_gen);
    return ESP_OK;
}

void audio_engine_stop(void)
{
    if (s_state == AE_STATE_IDLE && s_src == NULL) return;
    s_state = AE_STATE_IDLE;
    s_gen++;
    vTaskDelay(pdMS_TO_TICKS(120));            /* 让三个任务都看到 IDLE 并退出内层循环 */
    if (s_src && s_src->close) s_src->close(s_src);
    s_src = NULL;
    s_fetch_eof = false;
    s_dec_eof = false;
    s_paused = false;
}

void audio_engine_pause(bool pause) { s_paused = pause; }

ae_state_t audio_engine_state(void) { return s_state; }

const char *audio_engine_state_str(void)
{
    switch (s_state) {
        case AE_STATE_IDLE:      return "IDLE";
        case AE_STATE_BUFFERING: return "BUFFERING";
        case AE_STATE_PLAYING:   return "PLAYING";
        case AE_STATE_PAUSED:    return "PAUSED";
        case AE_STATE_FINISHED:  return "FINISHED";
        case AE_STATE_ERROR:     return "ERROR";
        default:                 return "?";
    }
}

const char *audio_engine_format_str(void)
{
    if (s_type == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE) return "none";
    return esp_audio_simple_dec_get_name(s_type);
}

uint32_t audio_engine_position_ms(void)
{
    uint32_t f = s_frames_out;
    uint32_t base = s_pos_base_ms;
    if (f <= BOARD_I2S_DMA_FRAMES) return base;   /* 还在 DMA 缓冲里，没真正出声 */
    return base + (f - BOARD_I2S_DMA_FRAMES) * 1000 / s_fs;
}

/* seek 之后把位置基准挪到目标位置（必须在 audio_engine_play() 之后调，
 * 因为 play 会把基准清 0） */
void audio_engine_set_pos_base(uint32_t ms) { s_pos_base_ms = ms; }

uint32_t audio_engine_decoded_ms(void) { return s_frames_decoded * 1000 / s_fs; }
bool     audio_engine_is_paused(void) { return s_paused; }
uint32_t audio_engine_sample_rate(void) { return s_fs; }
int      audio_engine_underruns(void) { return (int)s_underruns; }
size_t   audio_engine_ring_used_in(void) { return audio_ring_used(&s_rin); }
size_t   audio_engine_ring_used_pcm(void) { return audio_ring_used(&s_rpcm); }
size_t   audio_engine_ring_in_high_water(void) { return s_rin.max_used; }
bool     audio_engine_is_error(void) { return s_error; }

/* ============================================================ 内存源 */

typedef struct {
    const uint8_t *data;
    size_t         len;
    size_t         pos;
} mem_src_ctx_t;

static esp_err_t mem_open(ae_source_t *s, const char *uri)
{
    mem_src_ctx_t *c = s->ctx;
    c->pos = 0;
    return ESP_OK;
}

static int mem_read(ae_source_t *s, uint8_t *buf, size_t want)
{
    mem_src_ctx_t *c = s->ctx;
    size_t left = c->len - c->pos;
    if (left == 0) return 0;
    size_t n = want < left ? want : left;
    memcpy(buf, c->data + c->pos, n);
    c->pos += n;
    return (int)n;
}

static void mem_close(ae_source_t *s) { (void)s; }

ae_source_t *ae_source_memory_new(const uint8_t *data, size_t len)
{
    ae_source_t *s = heap_caps_calloc(1, sizeof(ae_source_t), MALLOC_CAP_SPIRAM);
    mem_src_ctx_t *c = heap_caps_calloc(1, sizeof(mem_src_ctx_t), MALLOC_CAP_SPIRAM);
    if (!s || !c) { if (s) heap_caps_free(s); if (c) heap_caps_free(c); return NULL; }
    c->data = data;
    c->len  = len;
    s->ctx = c;
    s->open = mem_open;
    s->read = mem_read;
    s->close = mem_close;
    return s;
}

void ae_source_memory_free(ae_source_t *s)
{
    if (!s) return;
    if (s->ctx) heap_caps_free(s->ctx);
    heap_caps_free(s);
}

const uint8_t *ae_embed_test_audio(bool flac, size_t *len)
{
    if (flac) {
        *len = (size_t)(test_melody_flac_end - test_melody_flac_start);
        return test_melody_flac_start;
    }
    *len = (size_t)(test_melody_mp3_end - test_melody_mp3_start);
    return test_melody_mp3_start;
}
