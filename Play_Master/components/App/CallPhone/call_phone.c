/**
 * @file call_phone.c
 * @brief 语音通话模块实现：本地唤醒词检测 + HFP 语音上行桥接
 *
 * 实现要点：
 *   1. create()：从 "model" 分区加载唤醒词模型（wn9s_nihaoxiaozhi，"你好小智"），
 *      创建 esp-sr AFE 实例；注册 audio_bus RX 读者 "call_phone_mic"；
 *      创建 CallPhone_Task（优先级 8，栈 8192，Core 1，见架构设计 6 任务表）。
 *   2. start_listening() 后任务循环：读麦克风 PCM（I2S 每 44.1kHz 帧 L/R 双槽位，
 *      先提取一路为 44.1kHz 单声道 16bit）→ 16.16 定点线性插值重采样到
 *      AFE 采样率（16kHz）→ feed → fetch 检测唤醒词；
 *   3. 检测命中：进入 CONNECTING，调用 bt_audio_hfp_start_voice() 请求手机语音助手；
 *      SCO 打开后进入 STREAMING：麦克风 PCM 经 bt_audio_hfp_send_pcm() 持续上行
 *      （44.1k 输入，bt_audio 内部降采样到 SCO 速率）；
 *   4. SCO 被关闭 / call_phone_hangup() / 超时后回到 LISTENING 继续监听。
 *
 * 数据流：麦克风 → audio_bus(RX) → call_phone_mic reader → esp-sr 唤醒检测
 *          → 命中 → HFP SCO 建立 → 麦克风上行 → 手机语音助手。
 *
 * 依赖：audio_bus（RX 多读者）、bt_audio（HFP 能力）、esp-sr（乐鑫组件 v2.5.3）。
 * 注意：本模块不处理 HFP 下行（手机应答放音由 bt_audio.on_pcm(HFP_DOWNLINK)
 *       输出并接至 sync_protocol，见主音频节点软件架构分层设计 3.3.2）。
 *
 * esp-sr 用法依据（本仓库 managed_components/espressif__esp-sr v2.5.3）：
 *   - esp_srmodel_init("model")：模型由构建流程打包进 "model" 分区
 *     （model_path.h / CMakeLists.txt "Add model partition and flash srmodels.bin"）；
 *   - afe_config_init("M", ...) + wakenet_model_name + create_from_config：
 *     唤醒模型随 AFE 实例创建时加载（esp_afe_config.h / esp_afe_sr_models.h）。
 */

#include "call_phone.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_wn_models.h"
#include "model_path.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* ======================== 模块常量定义 ================================================== */

#define TAG "CallPhone"

/** 麦克风固定采样率（与 audio_bus 全链路一致：AUDIO_BUS_SAMPLE_RATE） */
#define CP_MIC_SAMPLE_RATE        44100

/** esp-sr AFE 期望采样率：16kHz（见 esp_afe_sr_iface.h feed 注释 "16-bit @ 16 KHZ"） */
#define CP_AFE_RATE               16000

/** 上行 FIFO 默认字节数：16384 ≈ 186ms@44.1k 单声道 */
#define CP_PCM_FIFO_DEFAULT       16384

/** 唤醒命中后等待 SCO 打开的超时（ms） */
#define CP_SCO_TIMEOUT_MS         5000

/** STREAMING 单次上行块：20ms@44.1k = 882 采样（贴合 SCO 帧节奏喂入，避免大块突发） */
#define CP_UP_CHUNK_SAMPLES       882

/** 上行读取量（双槽位）：I2S 每帧 L/R 两个槽位，读取量 = 单声道块 × 2 */
#define CP_UP_PAIR_SAMPLES        (CP_UP_CHUNK_SAMPLES * 2)

/** LISTENING 读麦克风超时（ms）：一帧 16k 检测窗约 30ms，超时略大于一帧 */
#define CP_READ_TIMEOUT_MS        40

/** STREAMING 读麦克风超时（ms）：短超时把上行拆成小包（SCO 每 7.5/10ms 一帧） */
#define CP_STREAM_TIMEOUT_MS      10

/** 固定使用左槽（INMP441 L/R=GND → 左声道，见 docs/02-设计/引脚分配表.md）。
 * 实测两槽近似同源（浮空槽被"保持"出副本信号），切槽实为 50ms 量级抖动且无收益 */
#define CP_LANE_FIXED             0

/** 双槽能量 + 16k 馈入诊断日志间隔（LISTENING 帧数，32ms/帧，32 帧 ≈ 1s） */
#define CP_LANE_LOG_INTERVAL      32

/** 命令队列长度（START/STOP/HANGUP/DESTROY） */
#define CP_CMD_QUEUE_LEN          8

/** 任务名 / 优先级 / 栈 / 核：与架构设计 6 任务表一致（CallPhone_Task, 8, 8192, Core1） */
#define CP_TASK_NAME              "CallPhone_Task"
#define CP_TASK_PRIO              8
#define CP_TASK_STACK             8192
#define CP_TASK_CORE              1

/** esp-sr 模型分区标签（构建时由 esp-sr 组件把 srmodels.bin 烧进该分区） */
#define CP_MODEL_PARTITION        "model"

/** 唤醒词模型过滤关键字：esp_srmodel_filter 过滤出 "wn9s_nihaoxiaozhi"（你好小智） */
#define CP_WAKE_MODEL_KEYWORD     "nihaoxiaozhi"
#define CP_WAKE_WORD_STRING       "你好小智"

/* 任务命令 */
typedef enum {
    CP_CMD_START_LISTEN,   /* 开始监听唤醒词 */
    CP_CMD_STOP_LISTEN,    /* 停止监听 */
    CP_CMD_HANGUP,         /* 主动结束当前语音（等价挂断） */
    CP_CMD_DESTROY,        /* 销毁模块（任务内释放资源后退出） */
} call_phone_cmd_t;

typedef struct {
    call_phone_cmd_t cmd;
} call_phone_cmd_msg_t;

/* ======================== 模块私有结构 ================================================== */

struct call_phone_s {
    audio_bus_handle_t bus;            /* 调用方传入的 audio_bus（本模块只注册/注销 reader） */
    bt_audio_handle_t  audio;          /* 复用 bt_audio 的 HFP 能力 */
    uint32_t           pcm_fifo_bytes; /* 麦克风 reader FIFO 大小 */

    /* esp-sr 唤醒检测 */
    srmodel_list_t           *models;       /* 模型列表（来自 model 分区） */
    const esp_afe_sr_iface_t *afe_if;       /* AFE 接口函数表 */
    esp_afe_sr_data_t        *afe_data;     /* AFE 实例 */
    int                       afe_rate;     /* AFE 期望采样率（16k） */
    int                       feed_chunk;   /* feed() 每次需要的样本数 */
    uint64_t                  rs_step;      /* 44.1k→16k 16.16 定点重采样步长 */
    uint64_t                  rs_pos;       /* 重采样位置（跨帧连续） */

    /* 麦克风 reader */
    audio_reader_handle_t reader;

    /* 缓冲（PSRAM 优先） */
    int16_t *rs_in;      /* 44.1k 输入缓冲（I2S L/R 槽位对，容量 rs_in_cap） */
    int16_t *feed_buf;   /* 16k AFE 输入缓冲 */
    int16_t *up_buf;     /* HFP 上行缓冲（I2S L/R 槽位对，容量 CP_UP_PAIR_SAMPLES） */
    size_t   rs_in_cap;  /* rs_in 容量（槽位对样本数，≥ 2×单声道帧需求） */
    int8_t   lane;       /* I2S 槽位（0/1）；-1 表示未开始：默认左槽，检测到语音级信号自动切换 */
    uint32_t lane_log_cnt;  /* 双槽能量诊断日志计数器 */

    /* 任务与同步 */
    TaskHandle_t      task;
    QueueHandle_t     cmd_q;
    SemaphoreHandle_t lock;           /* 保护 state */
    SemaphoreHandle_t destroy_done;   /* DESTROY 完成后由任务置位 */
    volatile call_phone_state_t state;

    uint32_t sco_deadline;            /* CONNECTING 等待 SCO 的绝对超时 tick */
};

/* ======================== 私有函数 ======================================================= */

/** 分配样本缓冲：PSRAM 优先，失败退回内部堆 */
static int16_t *CallPhone_AllocSamples(size_t samples)
{
    int16_t *buf = heap_caps_malloc(samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (buf == NULL)
    {
        buf = malloc(samples * sizeof(int16_t));
        if (buf != NULL)
        {
            ESP_LOGW(TAG, "buffer %u samples on internal heap", (unsigned)samples);
        }
    }
    return buf;
}

/** 设置状态（互斥保护，API 线程与任务线程共用） */
static void CallPhone_State_Set(call_phone_handle_t cp, call_phone_state_t state)
{
    xSemaphoreTake(cp->lock, portMAX_DELAY);
    cp->state = state;
    xSemaphoreGive(cp->lock);
}

/** 读取状态 */
static call_phone_state_t CallPhone_State_Get(call_phone_handle_t cp)
{
    call_phone_state_t state;

    xSemaphoreTake(cp->lock, portMAX_DELAY);
    state = cp->state;
    xSemaphoreGive(cp->lock);
    return state;
}

/**
 * @brief 单声道 16bit 线性插值重采样（16.16 定点，与 bt_audio HFP 上行同款算法）
 *
 * @param in    输入缓冲（44.1k 单声道）
 * @param in_n  输入样本数
 * @param out   输出缓冲，容量须 >= out_n
 * @param out_n 需要输出的样本数
 * @param step  每输出样本前进的定点步长 = (src_rate << 16) / dst_rate
 * @param pos   位置状态（流开始清零，跨调用连续）
 * @return 实际输出样本数（恒等于 out_n）
 */
static uint32_t CallPhone_Resample_16_16(const int16_t *in, uint32_t in_n,
                                         int16_t *out, uint32_t out_n,
                                         uint64_t step, uint64_t *pos)
{
    uint32_t i;

    if (in_n == 0 || out_n == 0)
    {
        return 0;
    }

    for (i = 0; i < out_n; i++)
    {
        uint32_t idx  = (uint32_t)(*pos >> 16);
        uint32_t frac = (uint32_t)(*pos & 0xFFFF);
        int32_t  s0, s1;

        s0 = in[(idx < in_n) ? idx : in_n - 1];
        s1 = in[((idx + 1) < in_n) ? (idx + 1) : in_n - 1];
        out[i] = (int16_t)(s0 + ((int32_t)(((int64_t)(s1 - s0) * frac) >> 16)));
        *pos += step;
    }
    return out_n;
}

/**
 * @brief 从 I2S 双槽位交错流中提取单声道（就地覆盖）
 *
 * audio_bus RX 每 44.1kHz 帧输出 L/R 两个 32-bit 槽位样本（I2S_SLOT_MODE_STEREO，
 * 每帧先左槽后右槽）。INMP441 只驱动物理左槽（L/R=GND → 左声道，见
 * docs/02-设计/引脚分配表.md），另一槽浮空：GPIO34 输入专用脚无内部下拉
 * （与 gpio_pulldown_en 报错同因），浮空槽会拾取串扰噪声。
 *
 * 选槽结论（实机验证）：两槽内容高度相似（浮空槽形成"保持"副本），
 * 自动切槽实为 50ms 量级无意义抖动，故固定使用 CP_LANE_FIXED，
 * 两槽平均幅度仅作为诊断日志输出。
 *
 * @param pair       交错槽位样本缓冲（就地被覆盖为单声道）
 * @param samples    缓冲内样本总数（应为 2 的倍数）
 * @param lane       槽位（0/1），固定取 CP_LANE_FIXED；-1 仅表示未初始化
 * @param lane_avg0  可空输出：左槽平均绝对幅度（诊断用）
 * @param lane_avg1  可空输出：右槽平均绝对幅度（诊断用）
 * @return 还原后的单声道样本数（= samples/2）
 */
static size_t CallPhone_ExtractMono(int16_t *pair, size_t samples, int8_t *lane,
                                    int32_t *lane_avg0, int32_t *lane_avg1)
{
    size_t  frames = samples / 2;
    int64_t energy0 = 0, energy1 = 0;

    if (samples < 2)
    {
        return 0;
    }

    *lane = (int8_t)CP_LANE_FIXED;   /* 固定左槽（实测两槽同源，见函数头注释） */

    for (size_t i = 0; i < frames; i++)
    {
        int32_t s0 = pair[i * 2 + 0];
        int32_t s1 = pair[i * 2 + 1];
        energy0 += (s0 < 0) ? -s0 : s0;
        energy1 += (s1 < 0) ? -s1 : s1;
    }

    int32_t avg0 = (int32_t)(energy0 / (int64_t)frames);
    int32_t avg1 = (int32_t)(energy1 / (int64_t)frames);

    for (size_t i = 0; i < frames; i++)
    {
        pair[i] = pair[i * 2 + *lane];
    }

    if (lane_avg0 != NULL)
    {
        *lane_avg0 = avg0;
    }
    if (lane_avg1 != NULL)
    {
        *lane_avg1 = avg1;
    }
    return frames;
}

/** LISTENING：读麦克风（提取单声道）→ 重采样 → 喂入 → 检测唤醒词 */
static void CallPhone_Listening(call_phone_handle_t cp)
{
    uint32_t   in_need, pair_need;
    size_t     got = 0;
    esp_err_t  ret;
    int32_t    lane_avg0 = 0, lane_avg1 = 0;

    in_need = (uint32_t)(((uint64_t)cp->feed_chunk * CP_MIC_SAMPLE_RATE)
                         / (uint32_t)cp->afe_rate) + 1;
    if (in_need > cp->rs_in_cap / 2)
    {
        in_need = (uint32_t)(cp->rs_in_cap / 2);
    }
    pair_need = in_need * 2;   /* I2S 每帧两个槽位 */

    got = 0;
    while (got < pair_need)
    {
        size_t  now = 0;
        ret = audio_reader_read_pcm16(cp->reader, cp->rs_in + got,
                                      pair_need - got, &now, CP_READ_TIMEOUT_MS);
        if (ret != ESP_OK || now == 0)
        {
            break;
        }
        got += now;
    }
    if (got < pair_need)
    {
        return;
    }

    if (CallPhone_ExtractMono(cp->rs_in, pair_need, &cp->lane,
                              &lane_avg0, &lane_avg1) != in_need)
    {
        return;
    }

    /* 重采样 44.1k 单声道 → 16k AFE 输入，填满 feed_buf 一帧 */
    if (CallPhone_Resample_16_16(cp->rs_in, in_need, cp->feed_buf,
                                 (uint32_t)cp->feed_chunk, cp->rs_step,
                                 &cp->rs_pos) != (uint32_t)cp->feed_chunk)
    {
        return;
    }

    /* 游标折回本帧窗口：每帧已消费 in_need 个输入样本 */
    cp->rs_pos -= ((uint64_t)in_need << 16);
    if ((int64_t)cp->rs_pos < 0)
    {
        cp->rs_pos = 0;   /* 每帧 0.2 样本级进字飘移，直接吸收 */
    }

    /* 实测说话平均幅度约 200~600（偏弱），×4 增益后再喂给 esp-sr，提高唤醒灵敏度 */
    {
        const size_t n = (size_t)cp->feed_chunk;
        int64_t      favg = 0;
        int32_t      fpeak = 0;

        for (size_t i = 0; i < n; i++)
        {
            int32_t gv = (int32_t)cp->feed_buf[i] * 4;
            if (gv > 32767)  { gv = 32767; }
            if (gv < -32768) { gv = -32768; }
            cp->feed_buf[i] = (int16_t)gv;
            favg  += (gv < 0) ? -gv : gv;
            if ((gv < 0 ? -gv : gv) > fpeak) { fpeak = ((gv < 0) ? -gv : gv); }
        }

        /* 双槽能量 + 16k 馈入诊断：每约 1s 输出（feed 即为喂给 esp-sr 的电平） */
        if (++cp->lane_log_cnt >= CP_LANE_LOG_INTERVAL)
        {
            cp->lane_log_cnt = 0;
            /* ESP_LOGI(TAG, "mic: lanes avg0=%d avg1=%d (use lane %d), feed16k avg=%lld peak=%d",
             *          (int)lane_avg0, (int)lane_avg1, (int)cp->lane,
             *          (long long)(favg / (int64_t)n), (int)fpeak);
             * 统计日志项多，默认注释；排查评音/电平时可恢复 */
        }
    }

    /* 喂入 AFE 处理流水线（feed 与 fetch 成对；fetch 非阻塞，返回 NULL 表示尚未有结果） */
    cp->afe_if->feed(cp->afe_data, cp->feed_buf);

    afe_fetch_result_t *res = cp->afe_if->fetch(cp->afe_data);
    if (res != NULL && res->wakeup_state == WAKENET_DETECTED)
    {
        ESP_LOGI(TAG, "wake word '%s' detected (index %d)",
                 CP_WAKE_WORD_STRING, res->wake_word_index);

        CallPhone_State_Set(cp, CALL_PHONE_STATE_CONNECTING);
        cp->sco_deadline = (uint32_t)xTaskGetTickCount()
                           + pdMS_TO_TICKS(CP_SCO_TIMEOUT_MS);

        /* 请求手机语音助手（AT+BVRA=1）；SLC 未连接会失败，忽略本次唤醒 */
        if (bt_audio_hfp_start_voice(cp->audio) != ESP_OK)
        {
            ESP_LOGW(TAG, "start voice failed (HFP not connected?), back to listening");
            CallPhone_State_Set(cp, CALL_PHONE_STATE_LISTENING);
        }
    }
}

/** CONNECTING：等待目标手机 SCO 打开，超时回到监听 */
static void CallPhone_Connecting(call_phone_handle_t cp)
{
    if (bt_audio_hfp_is_audio_open(cp->audio))
    {
        /* SCO 已打开：丢弃唤醒期间积压的旧麦克风数据，重采样状态复位后开始上行 */
        audio_reader_flush(cp->reader);
        cp->rs_pos = 0;
        CallPhone_State_Set(cp, CALL_PHONE_STATE_STREAMING);
        ESP_LOGI(TAG, "HFP SCO open, mic uplink started");
        return;
    }

    if ((int32_t)(xTaskGetTickCount() - cp->sco_deadline) >= 0)
    {
        ESP_LOGW(TAG, "SCO open timeout (%d ms), back to listening", CP_SCO_TIMEOUT_MS);
        bt_audio_hfp_stop_voice(cp->audio);  /* 取消语音请求，避免残留 SCO */
        CallPhone_State_Set(cp, CALL_PHONE_STATE_LISTENING);
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(20));
}

/** STREAMING：读回麦克风（提取单声道）→ 上行到 bt_audio（SCO 速率转换在 bt_audio 内部） */
static void CallPhone_Streaming(call_phone_handle_t cp)
{
    size_t    got = 0;
    esp_err_t ret;

    ret = audio_reader_read_pcm16(cp->reader, cp->up_buf,
                                  CP_UP_PAIR_SAMPLES, &got, CP_STREAM_TIMEOUT_MS);
    if (ret == ESP_OK && got >= 2)
    {
        size_t frames = CallPhone_ExtractMono(cp->up_buf, got, &cp->lane, NULL, NULL);
        if (frames > 0)
        {
            ret = bt_audio_hfp_send_pcm(cp->audio, cp->up_buf, frames, 5);
            if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT)
            {
                ESP_LOGW(TAG, "hfp_send_pcm: %s", esp_err_to_name(ret));
            }
            /* ESP_ERR_TIMEOUT：bt_audio 上行 FIFO 满，直接丢帧（SCO 侧由静音填充兜底） */
        }
    }

    if (!bt_audio_hfp_is_audio_open(cp->audio))
    {
        ESP_LOGW(TAG, "SCO closed by phone, back to listening");
        cp->rs_pos = 0;
        CallPhone_State_Set(cp, CALL_PHONE_STATE_LISTENING);
    }
}

/**
 * @brief 统一释放资源（create 失败回滚与 DESTROY 共用）
 *
 * 逆序：注销 reader → 销毁 AFE 实例 → 释放模型 → 释放缓冲。
 * 仅任务线程（且模块已停）调用。
 */
static void CallPhone_Deinit(call_phone_handle_t cp)
{
    if (cp->reader != NULL)
    {
        audio_reader_unregister(cp->reader);
        cp->reader = NULL;
    }
    if (cp->afe_data != NULL && cp->afe_if != NULL)
    {
        cp->afe_if->destroy(cp->afe_data);
        cp->afe_data = NULL;
    }
    if (cp->models != NULL)
    {
        esp_srmodel_deinit(cp->models);
        cp->models = NULL;
    }
    if (cp->rs_in != NULL)
    {
        free(cp->rs_in);
        cp->rs_in = NULL;
    }
    if (cp->feed_buf != NULL)
    {
        free(cp->feed_buf);
        cp->feed_buf = NULL;
    }
    if (cp->up_buf != NULL)
    {
        free(cp->up_buf);
        cp->up_buf = NULL;
    }
}

/**
 * @brief 处理一条任务命令（任务线程内调用）
 *
 * @return true 表示已执行 DESTROY（任务将退出），false 继续运行
 */
static bool CallPhone_HandleCmd(call_phone_handle_t cp,
                                const call_phone_cmd_msg_t *msg)
{
    switch (msg->cmd)
    {
    case CP_CMD_START_LISTEN:
        if (CallPhone_State_Get(cp) == CALL_PHONE_STATE_IDLE)
        {
            cp->rs_pos = 0;
            CallPhone_State_Set(cp, CALL_PHONE_STATE_LISTENING);
            ESP_LOGI(TAG, "start listening, wake word '%s'", CP_WAKE_WORD_STRING);
        }
        break;

    case CP_CMD_STOP_LISTEN:
        if (CallPhone_State_Get(cp) == CALL_PHONE_STATE_LISTENING)
        {
            CallPhone_State_Set(cp, CALL_PHONE_STATE_IDLE);
            ESP_LOGI(TAG, "stop listening");
        }
        break;

    case CP_CMD_HANGUP:
    {
        call_phone_state_t state = CallPhone_State_Get(cp);

        if (state == CALL_PHONE_STATE_CONNECTING || state == CALL_PHONE_STATE_STREAMING)
        {
            bt_audio_hfp_stop_voice(cp->audio);
            ESP_LOGI(TAG, "hangup done, back to listening");
        }
        cp->rs_pos = 0;
        CallPhone_State_Set(cp, CALL_PHONE_STATE_LISTENING);
        break;
    }

    case CP_CMD_DESTROY:
        if (CallPhone_State_Get(cp) == CALL_PHONE_STATE_CONNECTING ||
            CallPhone_State_Get(cp) == CALL_PHONE_STATE_STREAMING)
        {
            bt_audio_hfp_stop_voice(cp->audio);  /* 先停 HFP 语音，再注销 reader/FIFO */
        }
        CallPhone_Deinit(cp);
        xSemaphoreGive(cp->destroy_done);
        vTaskDelete(NULL);
        return true;

    default:
        break;
    }
    return false;
}

/** CallPhone_Task：状态机主循环（优先级 8 / 栈 8192 / Core 1） */
static void CallPhone_Task(void *arg)
{
    call_phone_handle_t  cp = (call_phone_handle_t)arg;
    call_phone_cmd_msg_t msg;

    ESP_LOGI(TAG, "task started: mic=%d Hz -> afe=%d Hz, feed chunk=%d",
             CP_MIC_SAMPLE_RATE, cp->afe_rate, cp->feed_chunk);

    for (;;)
    {
        /* IDLE 时阻塞等待命令（省 CPU）；运行中非阻塞轮询，保证及时响应 */
        TickType_t wait = (CallPhone_State_Get(cp) == CALL_PHONE_STATE_IDLE)
                          ? portMAX_DELAY : 0;

        if (xQueueReceive(cp->cmd_q, &msg, wait) == pdTRUE)
        {
            if (CallPhone_HandleCmd(cp, &msg))
            {
                break;   /* 保险：DESTROY 已在 HandleCmd 内删除本任务 */
            }
        }

        switch (CallPhone_State_Get(cp))
        {
        case CALL_PHONE_STATE_IDLE:
            break;   /* 无命令时阻塞在 xQueueReceive，不空转 */

        case CALL_PHONE_STATE_LISTENING:
            CallPhone_Listening(cp);
            break;

        case CALL_PHONE_STATE_CONNECTING:
            CallPhone_Connecting(cp);
            break;

        case CALL_PHONE_STATE_STREAMING:
            CallPhone_Streaming(cp);
            break;
        }
    }
}

/* ======================== 公共 API ============================================================ */

call_phone_handle_t call_phone_create(const call_phone_cfg_t *cfg)
{
    call_phone_handle_t  cp = NULL;
    call_phone_cmd_msg_t msg;
    afe_config_t        *afe_cfg = NULL;
    char                *model_name;
    esp_err_t            ret;

    /* ---- 参数检查：采样率固定 44.1k；bus/audio 句柄必须有效 ---- */
    if (cfg == NULL || cfg->bus == NULL || cfg->audio == NULL)
    {
        ESP_LOGE(TAG, "create: invalid cfg (bus/audio required)");
        return NULL;
    }
    if (cfg->sample_rate != 0 && cfg->sample_rate != CP_MIC_SAMPLE_RATE)
    {
        ESP_LOGW(TAG, "sample_rate %lu forced to %d (全链路固定)",
                 (unsigned long)cfg->sample_rate, CP_MIC_SAMPLE_RATE);
    }

    cp = calloc(1, sizeof(*cp));
    if (cp == NULL)
    {
        ESP_LOGE(TAG, "create: calloc failed");
        return NULL;
    }
    cp->bus = cfg->bus;
    cp->audio = cfg->audio;
    cp->pcm_fifo_bytes = (cfg->pcm_fifo_bytes == 0)
                         ? CP_PCM_FIFO_DEFAULT : cfg->pcm_fifo_bytes;
    cp->state = CALL_PHONE_STATE_IDLE;
    cp->lane = -1;   /* 首次读取时按能量自动选择 I2S 槽位 */

    /* ---- 同步原语 ---- */
    cp->lock = xSemaphoreCreateMutex();
    cp->destroy_done = xSemaphoreCreateBinary();
    cp->cmd_q = xQueueCreate(CP_CMD_QUEUE_LEN, sizeof(msg));
    if (cp->lock == NULL || cp->destroy_done == NULL || cp->cmd_q == NULL)
    {
        ESP_LOGE(TAG, "create: sync primitives failed");
        goto fail;
    }

    /* ---- 步骤 1：加载模型（esp-sr v2.5.3：模型打包进 "model" 分区） ---- */
    cp->models = esp_srmodel_init(CP_MODEL_PARTITION);
    if (cp->models == NULL)
    {
        ESP_LOGE(TAG, "esp_srmodel_init(%s) failed: 请确认 partitions.csv 含 model 分区",
                 CP_MODEL_PARTITION);
        goto fail;
    }

    /* ---- 步骤 2：按名过滤 唤醒模型（wn9s_nihaoxiaozhi） ---- */
    model_name = esp_srmodel_filter(cp->models, ESP_WN_PREFIX, CP_WAKE_MODEL_KEYWORD);
    if (model_name == NULL)
    {
        ESP_LOGE(TAG, "wake model '%s' not found in model partition", CP_WAKE_MODEL_KEYWORD);
        goto fail;
    }
    ESP_LOGI(TAG, "wake model: %s", model_name);

    /* ---- 步骤 3：AFE 配置（单麦 "M"，仅启用唤醒检测，同参考写法） ---- */
    afe_cfg = afe_config_init("M", cp->models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    if (afe_cfg == NULL)
    {
        ESP_LOGE(TAG, "afe_config_init failed");
        goto fail;
    }
    afe_cfg->wakenet_init = true;
    afe_cfg->wakenet_model_name = model_name;
    /* 只做唤醒词检测，关闭其余算法以节省 CPU/内存 */
    afe_cfg->aec_init = false;
    afe_cfg->se_init  = false;
    afe_cfg->ns_init  = false;
    afe_cfg->vad_init = false;
    afe_cfg->agc_init = false;

    afe_cfg = afe_config_check(afe_cfg);
    if (afe_cfg == NULL)
    {
        ESP_LOGE(TAG, "afe_config_check failed");
        goto fail;
    }

    /* ---- 步骤 4：创建 AFE 实例（配置使命完成，随后释放） ---- */
    cp->afe_if = esp_afe_handle_from_config(afe_cfg);
    if (cp->afe_if == NULL)
    {
        ESP_LOGE(TAG, "esp_afe_handle_from_config failed");
        goto fail;
    }
    cp->afe_data = cp->afe_if->create_from_config(afe_cfg);
    if (cp->afe_data == NULL)
    {
        ESP_LOGE(TAG, "create_from_config failed");
        goto fail;
    }
    afe_config_free(afe_cfg);
    afe_cfg = NULL;

    /* ---- 步骤 5：查询 AFE 参数，计算重采样步长与输入帧大小 ---- */
    cp->afe_rate   = cp->afe_if->get_samp_rate(cp->afe_data);
    cp->feed_chunk = cp->afe_if->get_feed_chunksize(cp->afe_data);
    if (cp->afe_rate != CP_AFE_RATE)
    {
        ESP_LOGW(TAG, "unexpected afe rate %d (expect %d)", cp->afe_rate, CP_AFE_RATE);
    }
    cp->rs_step = ((uint64_t)CP_MIC_SAMPLE_RATE << 16) / (uint32_t)cp->afe_rate;
    /* 单帧 44.1k 单声道需求，再 ×2 存储 I2S L/R 槽位对 */
    cp->rs_in_cap = ((size_t)(((uint64_t)cp->feed_chunk * CP_MIC_SAMPLE_RATE)
                              / (uint32_t)cp->afe_rate) + 2) * 2;

    /* ---- 步骤 6：缓冲分配（PSRAM 优先） ---- */
    cp->rs_in    = CallPhone_AllocSamples(cp->rs_in_cap);
    cp->feed_buf = CallPhone_AllocSamples((size_t)cp->feed_chunk);
    cp->up_buf   = CallPhone_AllocSamples(CP_UP_PAIR_SAMPLES);
    if (cp->rs_in == NULL || cp->feed_buf == NULL || cp->up_buf == NULL)
    {
        ESP_LOGE(TAG, "create: buffer alloc failed");
        goto fail;
    }

    /* ---- 步骤 7：注册麦克风 reader（name "call_phone_mic"，FIFO 满丢旧数据） ---- */
    ret = audio_reader_register(cfg->bus, "call_phone_mic", cp->pcm_fifo_bytes,
                                AUDIO_FIFO_DROP_OLDEST, &cp->reader);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "audio_reader_register: %s", esp_err_to_name(ret));
        goto fail;
    }

    /* ---- 步骤 8：创建任务（CallPhone_Task） ---- */
    if (xTaskCreatePinnedToCore(CallPhone_Task, CP_TASK_NAME, CP_TASK_STACK, cp,
                                CP_TASK_PRIO, &cp->task, CP_TASK_CORE) != pdPASS)
    {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore failed");
        goto fail;
    }

    ESP_LOGI(TAG, "created: fifo=%u bytes, feed=%d samples@%d Hz",
             (unsigned)cp->pcm_fifo_bytes, cp->feed_chunk, cp->afe_rate);
    return cp;

fail:
    if (afe_cfg != NULL)
    {
        afe_config_free(afe_cfg);
    }
    if (cp != NULL)
    {
        CallPhone_Deinit(cp);
        if (cp->cmd_q != NULL)
        {
            vQueueDelete(cp->cmd_q);
        }
        if (cp->lock != NULL)
        {
            vSemaphoreDelete(cp->lock);
        }
        if (cp->destroy_done != NULL)
        {
            vSemaphoreDelete(cp->destroy_done);
        }
        free(cp);
    }
    ESP_LOGE(TAG, "create failed");
    return NULL;
}

/**
 * @brief 销毁模块：向任务投 DESTROY，等待任务完成清理后释放句柄
 *
 * @note 销毁后句柄即刻失效，调用方不得再使用
 */
esp_err_t call_phone_destroy(call_phone_handle_t cp)
{
    call_phone_cmd_msg_t msg;

    if (cp == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    msg.cmd = CP_CMD_DESTROY;
    if (xQueueSend(cp->cmd_q, &msg, pdMS_TO_TICKS(100)) != pdTRUE)
    {
        ESP_LOGE(TAG, "destroy: queue send timeout");
        return ESP_ERR_TIMEOUT;
    }
    if (xSemaphoreTake(cp->destroy_done, pdMS_TO_TICKS(3000)) != pdTRUE)
    {
        ESP_LOGE(TAG, "destroy: task exit timeout");
        return ESP_ERR_TIMEOUT;
    }

    vQueueDelete(cp->cmd_q);
    vSemaphoreDelete(cp->lock);
    vSemaphoreDelete(cp->destroy_done);
    free(cp);
    ESP_LOGI(TAG, "destroy done");
    return ESP_OK;
}

esp_err_t call_phone_start_listening(call_phone_handle_t cp)
{
    call_phone_cmd_msg_t msg;
    call_phone_state_t   state;

    if (cp == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    state = CallPhone_State_Get(cp);
    if (state == CALL_PHONE_STATE_LISTENING)
    {
        return ESP_OK;   /* 已在监听 */
    }
    if (state != CALL_PHONE_STATE_IDLE)
    {
        return ESP_ERR_INVALID_STATE;  /* 通话/建链中，先 hangup 再监听 */
    }

    msg.cmd = CP_CMD_START_LISTEN;
    return (xQueueSend(cp->cmd_q, &msg, pdMS_TO_TICKS(100)) == pdTRUE)
           ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t call_phone_stop_listening(call_phone_handle_t cp)
{
    call_phone_cmd_msg_t msg;
    call_phone_state_t   state;

    if (cp == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    state = CallPhone_State_Get(cp);
    if (state == CALL_PHONE_STATE_IDLE)
    {
        return ESP_OK;   /* 已停止 */
    }
    if (state != CALL_PHONE_STATE_LISTENING)
    {
        return ESP_ERR_INVALID_STATE;  /* 通话中先 hangup */
    }

    msg.cmd = CP_CMD_STOP_LISTEN;
    return (xQueueSend(cp->cmd_q, &msg, pdMS_TO_TICKS(100)) == pdTRUE)
           ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t call_phone_hangup(call_phone_handle_t cp)
{
    call_phone_cmd_msg_t msg;
    call_phone_state_t   state;

    if (cp == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    state = CallPhone_State_Get(cp);
    if (state == CALL_PHONE_STATE_IDLE || state == CALL_PHONE_STATE_LISTENING)
    {
        return ESP_OK;   /* 无通话在建立/进行 */
    }

    msg.cmd = CP_CMD_HANGUP;
    return (xQueueSend(cp->cmd_q, &msg, pdMS_TO_TICKS(100)) == pdTRUE)
           ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t call_phone_get_state(call_phone_handle_t cp, call_phone_state_t *state)
{
    if (cp == NULL || state == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *state = CallPhone_State_Get(cp);
    return ESP_OK;
}