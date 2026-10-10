/**
 * @file audio_decoder.c
 * @brief SD 卡 MP3 解码器实现（middlewares 层，见 audio_decoder.h）
 *
 * 设计依据（《主音频节点软件架构分层设计.md》3.2.3 / 任务表）：
 *   - 只做两件事：① MP3 流式解码；② 通过 on_pcm 回调把 PCM 丢出去；
 *     没有 play/pause 决策权，不提供 seek()，音量归 amplifier/上层；
 *   - 解码任务在模块内部创建：Core 1 / 优先级 10 / 栈 8192（minimp3 帧解码
 *     scratch 约 15.5KB 已由 open() 预分配至堆（PSRAM 优先），不再占任务栈，
 *     栈上只剩单帧 PCM 缓冲 4.6KB 与调用深度，故可用 8KB 栈）；
 *   - 文件读取走 middlewares/sd_card 的句柄式 API（FATFS/VFS）；
 *   - 解码库使用 third_party/minimp3（CC0-1.0）的基础帧解码接口
 *     （mp3dec_t + mp3dec_decode_frame），由本模块自建 8KB 输入缓冲并以
 *     memmove 滑动窗口续读。不使用 mp3dec_ex 流式接口——其内部 128KB
 *     IO 缓冲经 malloc 超阈值会落入 PSRAM，实测在本板（ESP32-WROVER-E +
 *     PSRAM）解码期间触发 cache-livelock 类 TG1WDT 复位（见排查记录）；
 *   - open 时由解码任务在自身 8KB 栈内探测首帧（不做整文件扫描），
 *     时长按"文件大小 × 8 / 码率"估算，避免大文件打开卡顿；
 *     首帧探测不在 open() 调用者的任务栈执行（主任务栈仅 3584B，栈内
 *     直接解码会溢出，已实测），改为解码任务探测后经信号量通知 open。
 *
 * minimp3 用法：在单一编译单元（本文件）定义 MINIMP3_IMPLEMENTATION 后
 * 引入 minimp3.h，全工程只在本处实例化解码器实现。
 */

#include "audio_decoder.h"

#include "sd_card.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#define MINIMP3_IMPLEMENTATION
#include "third_party/minimp3/minimp3.h"

/* ======================== 模块常量 =============================================== */

static const char *TAG = "AUDIO_DECODER";

/** 解码任务运行核：实时音频侧（设计任务表：Core 1） */
#define AUDIO_DECODER_TASK_CORE     1

/** 解码任务优先级（设计任务表：10） */
#define AUDIO_DECODER_TASK_PRIO     10

/** 解码任务栈大小（帧解码 scratch —— mp3dec_scratch_t 约 15.5KB —— 已由
 * open() 预分配至堆并 PSRAM 优先，不在任务栈上；栈上只剩单帧 PCM 4.6KB
 * 与调用深度，8KB 足够。原 32KB 是 scratch 落栈时的取值） */
#define AUDIO_DECODER_TASK_STACK    8192

/** 命令队列深度：同一时刻实际只有 1 条有效命令，留余量便于组合 */
#define AUDIO_DECODER_CMD_QUEUE_LEN 4

/** close() 等待解码任务退出上限（防止任务卡死时永久阻塞） */
#define AUDIO_DECODER_CLOSE_WAIT_MS 1000

/** MP3 输入缓冲（8KB，可容纳约 6 帧 320kbps/44.1kHz）；
 *  优先取 PSRAM（省内部 DRAM），分配失败自动回退内部 DRAM */
#define AUDIO_DECODER_IN_BUF_SIZE   8192

/** 缓冲内剩余字节低于该值时从 SD 续读（避免每帧都发起读卡） */
#define AUDIO_DECODER_IN_REFILL_MIN 512

/** open() 等待首帧探测完成的上限（探测在解码任务 8KB 栈内执行，
 *  正常情况 <1s 即可解出首帧；超时按非 MP3 文件处理） */
#define AUDIO_DECODER_PROBE_TIMEOUT_MS 5000

/* ======================== 内部定义 =============================================== */

/** 内部解码任务命令（audio_decoder_play/pause/stop 的传输载体） */
typedef enum {
    AUDIO_DECODER_CMD_PLAY  = 0,
    AUDIO_DECODER_CMD_PAUSE,
    AUDIO_DECODER_CMD_STOP,
    AUDIO_DECODER_CMD_CLOSE,
} audio_decoder_cmd_t;

/** 句柄实现 */
struct audio_decoder_s
{
    audio_decoder_cfg_t   cfg;          /* 回调与用户上下文 */
    char                 *path;         /* 文件路径（strdup） */
    size_t                file_size;    /* 文件字节数（时长估算用） */
    sd_card_file_handle_t file;         /* sd_card 文件句柄 */

    /* ---- 流式解码状态（minimp3 基础 API + 自建滑动窗口） ---- */
    mp3dec_t              mp3;          /* 解码器核心状态（约 7KB，随结构体在内部 DRAM） */
    mp3dec_scratch_t     *scratch;      /* 帧解码 scratch（约 15.5KB，PSRAM 优先，不占任务栈） */
    uint8_t              *in_buf;       /* 输入缓冲（8KB，PSRAM 优先，失败回退内部） */
    size_t                in_fill;      /* 缓冲内有效字节数 */
    size_t                in_consumed;  /* 已消费偏移（滑动窗口起点） */
    bool                  in_eof;       /* 已读到文件尾（fread 返回 0） */
    uint64_t              cur_samples;  /* 已输出采样数（含声道），进度计算用 */

    audio_decoder_info_t  info;         /* 文件参数（事件 / get_info 携带） */
    QueueHandle_t         cmd_q;        /* 解码任务命令队列 */
    TaskHandle_t          task;         /* 内部解码任务 */
    bool                  eof;          /* 已解码到文件尾 */
    SemaphoreHandle_t     probe_done;   /* open() 等待首帧探测完成（探测在任务内跑） */
    bool                  probe_ok;     /* 首帧探测结果：是否找到有效 MP3 帧 */
    mp3dec_frame_info_t   probe_fi;     /* 首帧头信息（hz/ch/bitrate，供 open 组装 info） */
    struct audio_decoder_s *next;       /* 打开句柄链表（deinit 强制回收用） */
};

/* 模块级：打开句柄注册表（audio_decoder_deinit 强制关闭全部） */
static SemaphoreHandle_t       s_reg_lock = NULL;
static struct audio_decoder_s *s_reg_head = NULL;

/* ======================== 输入缓冲续读（经 sd_card 读文件） ====================== */

/**
 * @brief 把缓冲内未消费的字节移到头部，再从 SD 读入新数据
 *
 * @return 0 已成功补数据；1 已到文件尾 / 读错误（in_eof 置位）
 */
static int Audio_Decoder_Refill(struct audio_decoder_s *dec)
{
    size_t remaining  = dec->in_fill - dec->in_consumed;
    size_t free_space = AUDIO_DECODER_IN_BUF_SIZE - remaining;
    size_t got        = 0;

    if (dec->in_consumed > 0)
    {
        memmove(dec->in_buf, dec->in_buf + dec->in_consumed, remaining);
        dec->in_fill     = remaining;
        dec->in_consumed = 0;
    }
    if (free_space == 0)
    {
        return 0;   /* 缓冲已满，无需读卡 */
    }

    if (SD_Card_Read(dec->file, dec->in_buf + dec->in_fill, free_space, &got) != ESP_OK)
    {
        dec->in_eof = true;
        return 1;
    }
    if (got == 0)
    {
        dec->in_eof = true;   /* fread 0 字节 = 文件尾 */
        return 1;
    }
    dec->in_fill += got;
    return 0;
}

/**
 * @brief 解码未推进时的兜底：优先续读；缓冲已满仍无效则丢弃 1 字节
 *
 * minimp3 在“找到同步头但窗口内帧不完整”或“坏伪同步头”时会返回
 * 0 采样 + 0 字节消费，此时若窗口内可用字节仍 >= REFILL_MIN 就不会走
 * 顶部续读分支，循环会长时间空转。此函数强制补数据，补不了就逐字节
 * 跳过（绕开坏区后重新同步），确保解码循环总有进展。
 */
static void Audio_Decoder_Force_Progress(struct audio_decoder_s *dec)
{
    size_t before_fill = dec->in_fill;

    if (dec->in_eof)
    {
        return;   /* 已到文件尾：交给 EOF 分支收尾 */
    }
    Audio_Decoder_Refill(dec);
    if (dec->in_fill != before_fill)
    {
        return;   /* 刚补到新数据，可重新解码 */
    }
    if (dec->in_consumed < dec->in_fill)
    {
        dec->in_consumed++;   /* 缓冲满仍无效：跳 1 字节重新同步 */
    }
}

/**
 * @brief 回卷到文件头（stop / 播放到 EOF 后的重新播放）
 *
 * 仅在解码任务空闲或解码循环内调用，不与 SD 读并发。
 */
static void Audio_Decoder_Rewind(struct audio_decoder_s *dec)
{
    if (dec->file != NULL)
    {
        SD_Card_Seek(dec->file, 0, SEEK_SET);
    }
    dec->in_fill     = 0;
    dec->in_consumed = 0;
    dec->in_eof      = false;
    dec->cur_samples = 0;
    mp3dec_init(&dec->mp3);
    dec->eof = false;
}

/**
 * @brief open() 阶段探测首帧（必须在解码任务 8KB 栈内调用）
 *
 * 帧解码 scratch 已由 open() 预分配至堆，但栈上仍需单帧 PCM 4.6KB，
 * 而 audio_decoder_open() 是在 app_main 主任务栈（默认 3584B）里被调用，
 * 直接在 open 里探测仍会栈溢出复位。因此探测放到解码任务中执行：
 * 逐块读入，minimp3 自动跳过 ID3v2 标签，找到并真实解码完整首个音频帧
 * 即成功；该帧 PCM 丢弃（约 26ms 音频），in_consumed 推进到下一帧，
 * 探测结果（probe_ok / probe_fi）通过信号量通知 open()。
 */
static void Audio_Decoder_Probe_In_Task(struct audio_decoder_s *dec,
                                        mp3d_sample_t *pcm)
{
    mp3dec_frame_info_t fi;
    bool ok = false;

    for (;;)
    {
        if (!dec->in_eof && (dec->in_fill - dec->in_consumed) < AUDIO_DECODER_IN_REFILL_MIN)
        {
            Audio_Decoder_Refill(dec);
        }

        size_t avail = dec->in_fill - dec->in_consumed;
        if (avail == 0)
        {
            break;   /* 空文件 / 全是标签 / 读到文件尾 */
        }

        memset(&fi, 0, sizeof(fi));
        int samples = mp3dec_decode_frame_scratch(&dec->mp3,
                                                  dec->in_buf + dec->in_consumed,
                                                  (int)avail, pcm, &fi,
                                                  dec->scratch);
        if (fi.frame_bytes > 0)
        {
            dec->in_consumed += (size_t)fi.frame_bytes;   /* 消费该块（含垃圾/首帧） */
        }

        if (samples > 0 && fi.hz > 0 && fi.channels > 0)
        {
            dec->probe_fi = fi;   /* 记录首帧头 */
            ok = true;
            break;
        }
        if (samples == 0 && fi.frame_bytes == 0 && !dec->in_eof)
        {
            Audio_Decoder_Force_Progress(dec);   /* (0,0)：优先续读或跳 1 字节 */
        }
        if (dec->in_eof)
        {
            break;   /* 读到文件尾仍无有效帧 */
        }
        /* 本缓冲内凑不齐完整帧：回到顶部续读 */
    }

    dec->probe_ok = ok;
    if (dec->probe_done != NULL)
    {
        xSemaphoreGive(dec->probe_done);
    }
}

/* ======================== 打开句柄注册表 ========================================= */

static void Audio_Decoder_Register(struct audio_decoder_s *dec)
{
    xSemaphoreTake(s_reg_lock, portMAX_DELAY);
    dec->next = s_reg_head;
    s_reg_head = dec;
    xSemaphoreGive(s_reg_lock);
}

static void Audio_Decoder_Unregister(struct audio_decoder_s *dec)
{
    struct audio_decoder_s **pp = NULL;

    xSemaphoreTake(s_reg_lock, portMAX_DELAY);
    pp = &s_reg_head;
    while (*pp != NULL && *pp != dec)
    {
        pp = &(*pp)->next;
    }
    if (*pp == dec)
    {
        *pp = dec->next;
    }
    xSemaphoreGive(s_reg_lock);
}

/**
 * @brief 释放 open() 失败路径上的资源（文件 / 缓冲 / 字符串 / 结构体）
 */
static void Audio_Decoder_Discard(struct audio_decoder_s *dec)
{
    if (dec == NULL)
    {
        return;
    }
    if (dec->probe_done != NULL)
    {
        vSemaphoreDelete(dec->probe_done);
    }
    if (dec->file != NULL)
    {
        SD_Card_Close(dec->file);
    }
    if (dec->in_buf != NULL)
    {
        free(dec->in_buf);
    }
    if (dec->scratch != NULL)
    {
        free(dec->scratch);
    }
    if (dec->path != NULL)
    {
        free(dec->path);
    }
    free(dec);
}
/* ======================== 内部解码任务 =========================================== */

static void Audio_Decoder_Task(void *arg)
{
    struct audio_decoder_s *dec  = (struct audio_decoder_s *)arg;
    audio_decoder_cmd_t cmd;
    bool quit = false;
    uint32_t decode_frames = 0;  /* 诊断：累计解码帧数 */
    int64_t  diag_last_us  = 0;  /* 诊断：上次心跳时间戳 */
    /* 单帧 PCM 缓冲：minimp3 单帧最多 1152 采样/声道 × 2 声道（46 字节×2304） */
    mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];

    /* open() 期间的首帧探测：帧解码 scratch（约 15.5KB）已在堆（PSRAM 优先），
     * 但解码任务栈上仍有单帧 PCM 4.6KB，主栈（默认 3584B）放不下，
     * 故仍在解码任务（8KB 栈）里执行、结果经信号量通知 open()
     * （open() 主栈直接探测已实测会栈溢出） */
    Audio_Decoder_Probe_In_Task(dec, pcm);

    for (;;)
    {
        /* 空闲：等待 play/pause/stop/close 命令 */
        if (xQueueReceive(dec->cmd_q, &cmd, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        if (cmd == AUDIO_DECODER_CMD_CLOSE)
        {
            break;   /* 退出任务，close() 负责回收资源 */
        }
        if (cmd == AUDIO_DECODER_CMD_STOP)
        {
            Audio_Decoder_Rewind(dec);
            continue;
        }
        if (cmd == AUDIO_DECODER_CMD_PAUSE)
        {
            continue;
        }
        if (cmd != AUDIO_DECODER_CMD_PLAY)
        {
            continue;
        }

        /* ---- 解码循环：逐帧解 MP3 → on_pcm，直到命令或文件尾 ---- */
        for (;;)
        {
            /* 新命令优先：暂停/停止/关闭可随时打断播放 */
            if (xQueueReceive(dec->cmd_q, &cmd, 0) == pdTRUE)
            {
                if (cmd == AUDIO_DECODER_CMD_CLOSE)
                {
                    quit = true;
                }
                else if (cmd == AUDIO_DECODER_CMD_STOP)
                {
                    Audio_Decoder_Rewind(dec);
                }
                break;
            }

            /* 缓冲续读：窗口剩余过少且未到文件尾时，从 SD 补新数据 */
            if (!dec->in_eof &&
                (dec->in_fill - dec->in_consumed) < AUDIO_DECODER_IN_REFILL_MIN)
            {
                Audio_Decoder_Refill(dec);
            }

            size_t avail = dec->in_fill - dec->in_consumed;
            if (avail == 0)
            {
                ESP_LOGI(TAG, "EOF: %s", dec->path);
                if (dec->cfg.on_event != NULL)
                {
                    dec->cfg.on_event(AUDIO_DECODER_EVT_EOF, &dec->info,
                                      dec->cfg.event_ctx);
                }
                dec->eof = true;
                break;
            }

            int64_t t0 = esp_timer_get_time();
            mp3dec_frame_info_t fi;
            memset(&fi, 0, sizeof(fi));
            int samples = mp3dec_decode_frame_scratch(&dec->mp3,
                                                      dec->in_buf + dec->in_consumed,
                                                      (int)avail, pcm, &fi,
                                                      dec->scratch);
            if (fi.frame_bytes > 0)
            {
                dec->in_consumed += (size_t)fi.frame_bytes;
            }

            if (samples > 0 && fi.hz > 0 && fi.channels > 0)
            {
                size_t total = (size_t)samples * (size_t)fi.channels;
                dec->cur_samples += (uint64_t)total;

                if (dec->cfg.on_pcm != NULL)
                {
                    dec->cfg.on_pcm(pcm, total, (uint32_t)fi.hz,
                                    (uint8_t)fi.channels, dec->cfg.pcm_ctx);
                }
                decode_frames++;

                /* 速率节流：解码+回调总耗时不低于"本帧音频时长"。有 I2S 回写时
                 * 写阻塞已天然限速，这里主要保护"无回写/受限"场景（测试/静音），
                 * 避免解码循环满速占满 CPU，与 PSRAM/Flash 缓存争用触发 TG1WDT。 */
                int64_t elapsed_us = esp_timer_get_time() - t0;
                int64_t frame_us   = (int64_t)samples * 1000000LL / (int64_t)fi.hz;
                int64_t remain_us  = frame_us - elapsed_us;
                if (remain_us > 1000)
                {
                    vTaskDelay(pdMS_TO_TICKS((uint32_t)(remain_us / 1000)));
                }
            }
            else if (dec->in_eof)
            {
                /* 剩余字节不足以再构成完整帧（尾部标签/数据）→ 正常结束 */
                ESP_LOGI(TAG, "EOF: %s (tail %u bytes)", dec->path,
                         (unsigned)avail);
                if (dec->cfg.on_event != NULL)
                {
                    dec->cfg.on_event(AUDIO_DECODER_EVT_EOF, &dec->info,
                                      dec->cfg.event_ctx);
                }
                dec->eof = true;
                break;
            }
            else if (samples == 0 && fi.frame_bytes == 0)
            {
                /* (0,0)：找到同步头但帧不完整 / 伪同步头，强制续读或跳 1 字节 */
                Audio_Decoder_Force_Progress(dec);
            }
            /* samples==0 且未到文件尾：frame_bytes 已跳过垃圾/不足字节，
             * 回到循环顶部续读，缓冲区凑齐完整帧后再次解码 */

            /* 诊断心跳：帧数/位置/栈水位（每秒 1 次，仅定位用） */
            if (esp_timer_get_time() - diag_last_us >= 1000000)
            {
                uint32_t pos_ms = 0;
                if (dec->info.sample_rate != 0 && dec->info.channels != 0)
                {
                    pos_ms = (uint32_t)((uint64_t)dec->cur_samples * 1000ULL /
                            ((uint64_t)dec->info.sample_rate * dec->info.channels));
                }
                diag_last_us = esp_timer_get_time();
                ESP_LOGI(TAG, "diag: frames=%lu pos=%lu/%lums heap=%u stack=%u",
                         (unsigned long)decode_frames, (unsigned long)pos_ms,
                         (unsigned long)dec->info.duration_ms,
                         (unsigned int)esp_get_free_heap_size(),
                         (unsigned int)uxTaskGetStackHighWaterMark(NULL));
            }
        }

        if (quit)
        {
            break;
        }
    }

    /* 任务退出：close() 正在等待 task 置 NULL */
    dec->task = NULL;
    vTaskDelete(NULL);
}

/* ======================== 对外 API =============================================== */

esp_err_t audio_decoder_init(void)
{
    if (s_reg_lock == NULL)
    {
        s_reg_lock = xSemaphoreCreateMutex();
        if (s_reg_lock == NULL)
        {
            ESP_LOGE(TAG, "create registry mutex failed");
            return ESP_ERR_NO_MEM;
        }
    }
    ESP_LOGI(TAG, "audio_decoder initialized");
    return ESP_OK;
}

esp_err_t audio_decoder_deinit(void)
{
    if (s_reg_lock == NULL)
    {
        ESP_LOGW(TAG, "deinit: not initialized");
        return ESP_OK;
    }

    /* 强制关闭所有仍打开的句柄 */
    for (;;)
    {
        struct audio_decoder_s *dec = NULL;

        xSemaphoreTake(s_reg_lock, portMAX_DELAY);
        if (s_reg_head == NULL)
        {
            xSemaphoreGive(s_reg_lock);
            break;
        }
        dec = s_reg_head;
        xSemaphoreGive(s_reg_lock);
        audio_decoder_close((audio_decoder_handle_t)dec);
    }

    if (s_reg_lock != NULL)
    {
        vSemaphoreDelete(s_reg_lock);
        s_reg_lock = NULL;
    }
    ESP_LOGI(TAG, "audio_decoder deinitialized");
    return ESP_OK;
}

/* ======================== 打开/关闭 ============================================== */

audio_decoder_handle_t audio_decoder_open(const char *path,
                                          const audio_decoder_cfg_t *cfg)
{
    struct audio_decoder_s *dec = NULL;
    sd_card_file_handle_t file   = NULL;
    size_t fsize = 0;
    BaseType_t rc;

    if (path == NULL || cfg == NULL || cfg->on_pcm == NULL)
    {
        ESP_LOGE(TAG, "open: invalid arg (path/cfg/on_pcm)");
        return NULL;
    }
    if (!SD_Card_Is_Mounted())
    {
        ESP_LOGE(TAG, "open: SD card not mounted, call SD_Card_Init first");
        return NULL;
    }
    if (s_reg_lock == NULL)
    {
        ESP_LOGE(TAG, "open: call audio_decoder_init() first");
        return NULL;
    }

    file = SD_Card_Open(path, "rb");
    if (file == NULL)
    {
        ESP_LOGE(TAG, "open: cannot open %s", path);
        return NULL;
    }

    /* 文件大小（供时长估算；查完回到文件头） */
    if (SD_Card_Seek(file, 0, SEEK_END) != ESP_OK ||
        SD_Card_Tell(file, &fsize) != ESP_OK ||
        SD_Card_Seek(file, 0, SEEK_SET) != ESP_OK)
    {
        ESP_LOGE(TAG, "open: query file size failed: %s", path);
        SD_Card_Close(file);
        return NULL;
    }

    dec = (struct audio_decoder_s *)calloc(1, sizeof(*dec));
    if (dec == NULL)
    {
        ESP_LOGE(TAG, "open: no memory");
        SD_Card_Close(file);
        return NULL;
    }
    dec->file      = file;
    dec->file_size = fsize;
    dec->cfg       = *cfg;
    dec->path      = strdup(path);
    if (dec->path == NULL)
    {
        ESP_LOGE(TAG, "open: strdup failed");
        Audio_Decoder_Discard(dec);
        return NULL;
    }

    /* 输入缓冲：8KB，PSRAM 优先省内部 RAM；分配失败自动回退内部。
     * 注：文件头记录的 cache-livelock 踩坑属 mp3dec_ex 的 128KB IO 缓冲，
     * 本模块用 minimp3 基础 API（8KB 缓冲）无此风险。 */
    dec->in_buf = (uint8_t *)heap_caps_malloc(AUDIO_DECODER_IN_BUF_SIZE,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (dec->in_buf == NULL)
    {
        dec->in_buf = (uint8_t *)heap_caps_malloc(AUDIO_DECODER_IN_BUF_SIZE,
                                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (dec->in_buf == NULL)
    {
        ESP_LOGE(TAG, "open: input buffer alloc failed");
        Audio_Decoder_Discard(dec);
        return NULL;
    }
    /* 帧解码 scratch（mp3dec_scratch_t，约 15.5KB）：不占任务栈，
     * open() 预分配、close() 释放；PSRAM 优先省内部 RAM，失败回退内部。
     * 注：cache-livelock 踩坑属 mp3dec_ex 的 128KB IO 缓冲，本模块用
     * minimp3 基础 API（16KB 级 scratch）无此风险。 */
    dec->scratch = (mp3dec_scratch_t *)heap_caps_malloc(sizeof(*dec->scratch),
                                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (dec->scratch == NULL)
    {
        dec->scratch = (mp3dec_scratch_t *)heap_caps_malloc(sizeof(*dec->scratch),
                                                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (dec->scratch == NULL)
    {
        ESP_LOGE(TAG, "open: scratch alloc failed");
        Audio_Decoder_Discard(dec);
        return NULL;
    }
    mp3dec_init(&dec->mp3);

    /* 首帧探测完成信号量：探测在解码任务 8KB 栈上执行（帧 scratch 已在堆，
     * 栈上只剩单帧 PCM 4.6KB；主任务 3584B 栈放不下会 Double Exception
     * 复位，已实测）。open() 创建任务后经该信号量等待结果，再用 probe_fi
     * 组装文件参数。 */
    dec->probe_done = xSemaphoreCreateBinary();
    if (dec->probe_done == NULL)
    {
        ESP_LOGE(TAG, "open: probe semaphore create failed");
        Audio_Decoder_Discard(dec);
        return NULL;
    }

    /* 命令队列 */
    dec->cmd_q = xQueueCreate(AUDIO_DECODER_CMD_QUEUE_LEN,
                              sizeof(audio_decoder_cmd_t));
    if (dec->cmd_q == NULL)
    {
        ESP_LOGE(TAG, "open: queue create failed");
        Audio_Decoder_Discard(dec);
        return NULL;
    }

    Audio_Decoder_Register(dec);

    /* 创建解码任务（固定 Core 1，见设计任务表） */
    rc = xTaskCreatePinnedToCore(Audio_Decoder_Task, "audio_decoder",
                                 AUDIO_DECODER_TASK_STACK, dec,
                                 AUDIO_DECODER_TASK_PRIO, &dec->task,
                                 AUDIO_DECODER_TASK_CORE);
    if (rc != pdPASS)
    {
        ESP_LOGE(TAG, "open: task create failed: %s", path);
        Audio_Decoder_Unregister(dec);
        vQueueDelete(dec->cmd_q);
        Audio_Decoder_Discard(dec);
        return NULL;
    }

    /* 等待解码任务完成首帧探测（正常 <1s；超时按非 MP3 文件处理） */
    if (xSemaphoreTake(dec->probe_done,
                       pdMS_TO_TICKS(AUDIO_DECODER_PROBE_TIMEOUT_MS)) != pdTRUE)
    {
        ESP_LOGE(TAG, "open: first-frame probe timeout: %s", path);
        vTaskDelete(dec->task);
        dec->task = NULL;
        Audio_Decoder_Unregister(dec);
        vQueueDelete(dec->cmd_q);
        Audio_Decoder_Discard(dec);
        return NULL;
    }
    if (!dec->probe_ok)
    {
        ESP_LOGE(TAG, "open: not a mp3 file: %s", path);
        vTaskDelete(dec->task);
        dec->task = NULL;
        Audio_Decoder_Unregister(dec);
        vQueueDelete(dec->cmd_q);
        Audio_Decoder_Discard(dec);
        return NULL;
    }

    /* 组装文件参数（取探测到的首个音频帧头） */
    dec->info.format      = AUDIO_DECODER_FMT_MP3;
    dec->info.sample_rate = (uint32_t)dec->probe_fi.hz;
    dec->info.channels    = (uint8_t)dec->probe_fi.channels;
    dec->info.bitrate_bps = (uint32_t)dec->probe_fi.bitrate_kbps * 1000U;
    if (dec->info.bitrate_bps > 0)
    {
        /* 估算：文件比特数 / 基准码率（不做整文件扫描） */
        dec->info.duration_ms = (uint32_t)(((uint64_t)fsize * 8ULL * 1000ULL) /
                                           dec->info.bitrate_bps);
    }

    ESP_LOGI(TAG, "open %s : %luHz/%uch bitrate=%lubps duration=%lums",
             path, (unsigned long)dec->info.sample_rate, dec->info.channels,
             (unsigned long)dec->info.bitrate_bps,
             (unsigned long)dec->info.duration_ms);

    /* READY：文件头解析完成，上层可读取参数 */
    if (dec->cfg.on_event != NULL)
    {
        dec->cfg.on_event(AUDIO_DECODER_EVT_READY, &dec->info,
                          dec->cfg.event_ctx);
    }

    return (audio_decoder_handle_t)dec;
}

esp_err_t audio_decoder_close(audio_decoder_handle_t dec)
{
    struct audio_decoder_s *d  = (struct audio_decoder_s *)dec;
    audio_decoder_cmd_t cmd    = AUDIO_DECODER_CMD_CLOSE;
    int waits = 0;

    if (d == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    /* 通知解码任务退出（队列满则直接跳过：任务会优先处理 CLOSE） */
    xQueueSend(d->cmd_q, &cmd, pdMS_TO_TICKS(100));

    /* 等待任务自行退出（任务退出时会把 task 置 NULL） */
    while (d->task != NULL && waits < (AUDIO_DECODER_CLOSE_WAIT_MS / 10))
    {
        vTaskDelay(pdMS_TO_TICKS(10));
        waits++;
    }
    if (d->task != NULL)
    {
        ESP_LOGW(TAG, "close: task not exit in %dms, force delete",
                 AUDIO_DECODER_CLOSE_WAIT_MS);
        vTaskDelete(d->task);
        d->task = NULL;
    }

    Audio_Decoder_Unregister(d);
    vQueueDelete(d->cmd_q);
    if (d->probe_done != NULL)
    {
        vSemaphoreDelete(d->probe_done);
    }
    if (d->file != NULL)
    {
        SD_Card_Close(d->file);
    }
    if (d->in_buf != NULL)
    {
        free(d->in_buf);
    }
    if (d->scratch != NULL)
    {
        free(d->scratch);
    }
    free(d->path);
    free(d);
    ESP_LOGI(TAG, "closed handle");
    return ESP_OK;
}

/* ======================== 播放控制 =============================================== */

esp_err_t audio_decoder_play(audio_decoder_handle_t dec)
{
    struct audio_decoder_s *d  = (struct audio_decoder_s *)dec;
    audio_decoder_cmd_t cmd    = AUDIO_DECODER_CMD_PLAY;

    if (d == NULL || d->cmd_q == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    /* 已到文件尾：先回卷再播放（EOF 之后重新开始） */
    if (d->eof)
    {
        Audio_Decoder_Rewind(d);
    }

    if (xQueueSend(d->cmd_q, &cmd, pdMS_TO_TICKS(100)) != pdTRUE)
    {
        ESP_LOGW(TAG, "play: command queue full");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t audio_decoder_pause(audio_decoder_handle_t dec)
{
    struct audio_decoder_s *d  = (struct audio_decoder_s *)dec;
    audio_decoder_cmd_t cmd    = AUDIO_DECODER_CMD_PAUSE;

    if (d == NULL || d->cmd_q == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (xQueueSend(d->cmd_q, &cmd, pdMS_TO_TICKS(100)) != pdTRUE)
    {
        ESP_LOGW(TAG, "pause: command queue full");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t audio_decoder_stop(audio_decoder_handle_t dec)
{
    struct audio_decoder_s *d  = (struct audio_decoder_s *)dec;
    audio_decoder_cmd_t cmd    = AUDIO_DECODER_CMD_STOP;

    if (d == NULL || d->cmd_q == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (xQueueSend(d->cmd_q, &cmd, pdMS_TO_TICKS(100)) != pdTRUE)
    {
        ESP_LOGW(TAG, "stop: command queue full");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t audio_decoder_get_info(audio_decoder_handle_t dec,
                                 audio_decoder_info_t *info)
{
    struct audio_decoder_s *d = (struct audio_decoder_s *)dec;

    if (d == NULL || info == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *info = d->info;
    return ESP_OK;
}

esp_err_t audio_decoder_get_position_ms(audio_decoder_handle_t dec,
                                        uint32_t *position_ms)
{
    struct audio_decoder_s *d = (struct audio_decoder_s *)dec;
    uint64_t total = 0;

    if (d == NULL || position_ms == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (d->info.sample_rate == 0 || d->info.channels == 0)
    {
        *position_ms = 0;
        return ESP_OK;
    }

    /* 位置 = 已输出采样数（含声道）× 1000 /（采样率 × 声道） */
    total = (uint64_t)d->cur_samples * 1000ULL /
            ((uint64_t)d->info.sample_rate * d->info.channels);
    *position_ms = (uint32_t)total;
    return ESP_OK;
}
