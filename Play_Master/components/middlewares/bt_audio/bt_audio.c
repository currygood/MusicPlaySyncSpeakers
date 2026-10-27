/**
 * @file bt_audio.c
 * @brief 经典蓝牙音频模块实现：A2DP Sink + HFP(HF) 语音链路
 *
 * 本阶段实现：
 *   - A2DP Sink：手机连接、SBC 解码 PCM 接收、on_pcm 回调输出；
 *   - HFP(HF)：SLC/SCO 语音链路；上行 44.1k 单声道经 send_pcm 喂入，模块内
 *     降采样到 SCO 速率（mSBC 16k / CVSD 8k）发送；下行 SCO 原始 PCM 上采样
 *     到 44.1k 单声道后经 on_pcm(BT_AUDIO_PCM_SOURCE_HFP_DOWNLINK) 输出；
 *   - 事件：连接/断开、流启停、采样率变化、HFP 状态经 on_event 回调输出；
 *   - 配对：SSP 自动确认（测试阶段）、PIN 码固定 "0000" 兜底；
 *   - 架构约定：蓝牙协议栈回调内只入队列/计数，对外回调统一在
 *     bt_audio 任务上下文触发，绝不在协议栈上下文调用用户回调。
 *
 * 说明：AVRCP 已实现播放控制（play/pause/prev/next/toggle）、音量（SetAbsoluteVolume）
 *       与曲目信息（元数据 + TRACK_CHANGE / PLAY_STATUS_CHANGE / VOLUME_CHANGE 通知），
 *       以及播放器重复模式下发（SetPlayerApplicationSettingValue，best-effort）。
 */

#include "bt_audio.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_hf_client_api.h"
#include "esp_hf_client_legacy_api.h"
#include "esp_coexist.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"

#define TAG "bt_audio"

#if BT_AUDIO_DEBUG_LOGS
#define BT_AUDIO_LOGE(...) ESP_LOGE(TAG, __VA_ARGS__)
#define BT_AUDIO_LOGW(...) ESP_LOGW(TAG, __VA_ARGS__)
#define BT_AUDIO_LOGI(...) ESP_LOGI(TAG, __VA_ARGS__)
#define BT_AUDIO_LOGD(...) ESP_LOGD(TAG, __VA_ARGS__)
#else
#define BT_AUDIO_LOGE(...) do { } while (0)
#define BT_AUDIO_LOGW(...) do { } while (0)
#define BT_AUDIO_LOGI(...) do { } while (0)
#define BT_AUDIO_LOGD(...) do { } while (0)
#endif

/* ======================== 默认参数 ============================================ */

/** A2DP PCM FIFO 默认大小（cfg->a2dp_fifo_bytes 为 0 时使用）
 *
 *  44.1kHz/2ch/16bit = 176400 B/s ⇒ 64KB ≈ 371ms。
 *  原来是 24KB（≈139ms），但蓝牙这条支路是"手机实时给多少就有多少"，
 *  FIFO 就是唯一的抖动垫子（SD 卡路径不是：解码端能跑在实时速率之前，
 *  64KB 的 sync 入口环常年是满的）。所以这里放大到 64KB，配合任务侧的
 *  开流预灌，把蓝牙路径的抗抖动能力从"零"提到 100ms 以上。 */
#define BT_A2DP_FIFO_DEFAULT_BYTES  (64 * 1024)

/** A2DP 开流预灌水位：开始往下游喂数据前，先让 FIFO 攒到这么多字节。
 *  24KB ≈ 139ms @44.1k/2ch，等于给蓝牙音频补一个"启动抖动缓冲"。 */
#define BT_A2DP_PREBUFFER_BYTES     (24 * 1024)

/** 字节流错位自愈后的重灌水位（8KB ≈ 46ms）。
 *  只在 FIFO 被 Reset 之后使用——那种情况下确实是"从头开始"，需要一小段
 *  余量；正常播放中读空**绝不**用这个值重灌（见任务里 bytes==0 的注释）。 */
#define BT_A2DP_REPRIME_BYTES       (8 * 1024)

/** HFP 上行 FIFO 默认大小（44.1kHz 单声道；1s ≈ 88200 字节） */
#define BT_HFP_UP_FIFO_DEFAULT_BYTES  (16 * 1024)

/** HFP 下行 FIFO 默认大小（16k 单声道；1s ≈ 32000 字节） */
#define BT_HFP_DN_FIFO_DEFAULT_BYTES  (8 * 1024)

/** HFP 下行单次拉取字节数（160 samples@8k=20ms / 320@16k=10ms） */
#define BT_HFP_DN_CHUNK_BYTES         (320)

/** HFP 上行重采样输入缓冲（44131→8k: 每帧最大需要 ≈1411 samples） */
#define BT_HFP_RS_IN_MAX_SAMPLES      (1536)


/** 事件队列深度（队列元素 bt_audio_event_t 约 536B，8 条 ≈4.3KB 内部 RAM，
 *  wifi+BT 同时开启时易分配失败，故取 4，与 MusicPlay 命令队列一致） */
#define BT_EVENT_QUEUE_LEN          4

/** 音量步进（bt_audio_cmd_t 的 VOLUME_UP/DOWN，范围 0..100） */
#define BT_AUDIO_VOLUME_STEP        5

#define BT_AUDIO_POS_NOTIFY_INTERVAL_MS 1000    /** AVRCP PLAY_POS_CHANGED 通知间隔（ms）：手机按此间隔主动上报播放位置 */

/** AVRCP 绝对音量换算：0..127 → 0..100 */
#define BT_VOL_AVRC_TO_LOCAL(v)     ((uint8_t)(((uint32_t)(v) * 100 + 63) / 127))
/** 0..100 → 0..127 */
#define BT_VOL_LOCAL_TO_AVRC(v)     ((uint8_t)(((uint32_t)(v) * 127 + 50) / 100))

/** 每次从 FIFO 取出的 PCM 块长度（int16 采样点数） */
#define BT_PCM_CHUNK_SAMPLES        2048

/**
 * A2DP PCM 的**整帧字节数**：16bit × 2 声道 = 4 字节。
 *
 * FIFO 是纯字节流，没有消息边界。只要写进去的字节数不是 4 的整数倍
 * （哪怕只差 1 字节），环里所有采样的边界就永久错位一格：消费端把相邻
 * 两字节拼成 int16，输出的就是满量程随机噪声（听感"吱吱/蹦蹦"），而且
 * 只有断开重连（FIFO 重建）才会恢复。
 *
 * 危险点在于 FreeRTOS 的语义：xStreamBufferSend() 在 xTicksToWait=0 且空间
 * 不足时是**部分写入**（写满剩余空间就返回），而 stream buffer 的空闲
 * 字节数 = xLength - used - 1，xLength 为偶数、游标按整帧走 ⇒ 空闲字节数
 * 恒为**奇数**。也就是说：只要 A2DP FIFO 溢出过一次，就必然写进半个采样，
 * 整条流从此变成噪声。所以生产端必须自己按整帧裁剪，不能依赖 send 的语义。
 */
#define BT_A2DP_FRAME_BYTES         4u

/**
 * 全链路固定采样率：amplifier 的 I2S、sync_protocol 的帧节拍（15ms = 661 样点）、
 * 组播包、从节点全部写死 44.1kHz，所以 bt_audio 对外只输出 44.1k。
 *
 * 源采样率不是 44.1k 时（Windows 笔记本的 SBC 典型是 48k，Android 手机是 44.1k）
 * 必须在这里先重采样，否则 48k 数据会以 8.16% 的速率盈余把 sync 的入口环灌满，
 * 之后每个 15ms 槽都要丢掉一段样点 —— 听感就是持续的"吱吱 / 收音机"底噪。
 */
#define BT_A2DP_TARGET_RATE         44100u

/** 重采样输出缓冲容量（int16 个数）。
 *  单次从 FIFO 最多取 BT_PCM_CHUNK_SAMPLES 个 int16；按 16k 单声道升采样最坏
 *  放大 2.76 倍 ⇒ 需要 ≥5650，这里留 3 倍余量。 */
#define BT_RS_BUF_SAMPLES           (BT_PCM_CHUNK_SAMPLES * 3)

/** 模块任务栈：大 PCM 缓冲已移出栈（A2DP/HFP 下行块放 PSRAM），
 *  栈上只剩事件结构体（约 536B）与回调调用帧（sync/Amplifier），4KB 足够 */
#define BT_AUDIO_TASK_STACK         4096

/** 模块任务优先级。原来为 5 —— 全系统倒数第二（只高于 Touch_Task 的 3），
 *  而音乐链路的其它环节都在 9~12（MusicPlay 9 / decoder 10 / SyncTx 11 /
 *  SyncPlay 12）。蓝牙这里是"实时生产者"，被压 15ms 以上就会让下游断音，
 *  所以往上提：11 —— 只低于 SyncPlay(12)（播放节拍绝不能被生产者抢掉），
 *  高于解码器(10)/MusicPlay(9)。 */
#define BT_AUDIO_TASK_PRIO          11

/** 模块任务绑核：固定在 core 1。
 *
 *  core 0 上已经挤了 WiFi(23) / BT 控制器(~23) / lwIP(18) / SyncTx(11) /
 *  UI / Touch，并且经典蓝牙与 WiFi 共用射频（CONFIG_ESP_COEX_SW_COEXIST_ENABLE），
 *  A2DP 收包本身就带几十毫秒抖动。把 FIFO 搬运任务也丢在 core 0，等于让
 *  "最怕抖"的一环去和射频栈抢 CPU。
 *
 *  core 1 上只有 SyncPlay(12) / decoder(10) / MusicPlay(9) / CallPhone(8)，
 *  且蓝牙放音时 decoder 空闲，搬运动作可以稳定拿到时间片。
 *  （协议栈的 A2DP 数据回调仍在 core 0 写 FIFO，这是跨核写入，但 FIFO 本身
 *   带锁且只是 memcpy，代价远小于被抢占） */
#define BT_AUDIO_TASK_CORE          1

/* 配对 PIN（测试阶段固定，后续可由 UI 配置） */
#define BT_PIN_CODE                 "0000"

/* 按 sdkconfig 控制器模式选择使能模式（bt_test.c 已验证 BTDM 可跑通） */
#if defined(CONFIG_BTDM_CTRL_MODE_BTDM)
#define BT_AUDIO_CTRL_MODE          ESP_BT_MODE_BTDM
#elif defined(CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY)
#define BT_AUDIO_CTRL_MODE          ESP_BT_MODE_CLASSIC_BT
#else
#define BT_AUDIO_CTRL_MODE          ESP_BT_MODE_BTDM
#endif

/* ======================== 内部状态 ========================================== */

/** bt_audio 模块内部实现（单实例） */
struct bt_audio_s
{
    /* ---- 对外回调（create 时固化；均由模块任务上下文触发） ---- */
    bt_audio_pcm_cb_t   on_pcm;        /* PCM 回调（bt_audio 任务上下文触发） */
    void               *pcm_ctx;
    bt_audio_event_cb_t on_event;      /* 事件回调（bt_audio 任务上下文触发） */
    void               *event_ctx;

    /* ---- 配置参数 ---- */
    char                device_name[32];
    uint32_t            a2dp_fifo_bytes;   /* 用户配置的 A2DP FIFO 大小 */
    uint32_t            hfp_up_fifo_bytes;  /* 用户配置的 HFP 上行 FIFO 大小 */

    /* ---- A2DP 运行状态 ---- */
    bt_audio_a2dp_info_t a2dp;           /* A2DP 运行参数 */
    uint8_t             remote_addr[6];  /* 当前已连接手机地址（stop 时断开用） */

    /* ---- 内部存储（StreamBuffer 数据缓冲放 PSRAM，控制块在内部 RAM） ---- */
    StreamBufferHandle_t pcm_fifo;       /* A2DP PCM 队列（协议栈回调入队） */
    StreamBufferHandle_t hfp_up_fifo;     /* HFP 上行（44.1k 单声道）PCM 队列（应用 push） */
    StreamBufferHandle_t hfp_dn_fifo;     /* HFP 下行（SCO 原始 8k/16k）PCM 队列（协议栈回调入队） */
    uint8_t             *pcm_fifo_buf;    /* PSRAM 数据缓冲（内部 RAM 紧张，大数据放 PSRAM） */
    uint8_t             *hfp_up_fifo_buf;
    uint8_t             *hfp_dn_fifo_buf;
    StaticStreamBuffer_t pcm_fifo_ctrl;
    StaticStreamBuffer_t hfp_up_fifo_ctrl;
    StaticStreamBuffer_t hfp_dn_fifo_ctrl;
    QueueHandle_t        evt_q;          /* 事件队列（协议栈回调入队） */
    TaskHandle_t         task;           /* 模块任务 */

    /* ---- 模块任务缓冲（大缓冲放 PSRAM，省内部 DRAM；栈上只留事件结构体） ---- */
    int16_t             *task_pcm_buf;     /* A2DP PCM 块缓冲（PSRAM，BT_PCM_CHUNK_SAMPLES×2B） */
    int16_t             *task_rs_buf;      /* A2DP 重采样输出缓冲（PSRAM，BT_RS_BUF_SAMPLES） */
    int16_t             *task_hfp_dn_raw;  /* HFP 下行原始块缓冲（PSRAM，BT_HFP_DN_CHUNK_BYTES） */
    uint8_t             *evt_q_storage;    /* 事件队列数据缓冲（PSRAM，静态队列用） */
    StaticQueue_t        evt_q_ctrl;       /* 事件队列控制块（内部 RAM） */

    /* ---- 丢帧 / 丢事件统计 ---- */
    uint32_t             dropped_pcm;    /* A2DP FIFO 满丢弃字节数 */
    uint32_t             resync_cnt;     /* A2DP FIFO 出现非整帧数据后的复位次数（自愈） */
    uint32_t             dropped_evt;    /* 事件队列满丢弃个数 */
    uint32_t             hfp_dn_dropped; /* HFP 下行 FIFO 溢出丢弃字节数 */

    /* ---- A2DP 抖动缓冲（开流预灌） ---- */
    bool                 a2dp_primed;       /* FIFO 是否已攒够预灌水位 */
    uint32_t             a2dp_prime_bytes;  /* 本轮预灌目标（开流 24KB） */
    uint32_t             a2dp_idle_cnt;     /* FIFO 读空次数（稳态下本来就会发生，非异常） */
    uint32_t             a2dp_underrun_cnt; /* 连续 80ms 无数据 = 真欠载次数：卡顿的直接指标 */
    uint8_t              a2dp_idle_run;     /* 当前连续读空的轮数（用于区分"取太快"与"真断供"） */

    /* ---- A2DP 到达间隔统计（判断断供是"手机攒包"还是"射频丢包"） ---- */
    uint64_t             a2dp_last_cb_us;   /* 上一次数据回调的时刻 */
    uint32_t             a2dp_gap_max_ms;   /* 本统计窗口内相邻两次回调的最大间隔 */

    /* ---- A2DP 采样率适配（源 48k/32k → 44.1k） ----
     * 只由 bt_audio_task 读写（相位必须单写者，否则跨块插值会跳变）；
     * 协议栈回调只把 a2dp_rs_rate 清 0 表示"下一条流要重新起相位"。 */
    int64_t              a2dp_rs_pos;       /* 插值相位（32.32，单位=源采样帧，可跨块为负） */
    int16_t              a2dp_rs_last[2];   /* 上一块最后一帧（跨块插值用，按声道） */
    uint32_t             a2dp_rs_rate;      /* 相位当前对应的源采样率（0=需复位） */

    /* ---- 活体检测（卡死定位用，只增不减） ----
     * a2dp_cb_cnt 在协议栈数据回调里自增，task_beat 在模块任务循环里自增。
     * 二者组合能唯一区分三种"没声音"：
     *   两者都停        → 手机/链路不再送数据（host 没收到）
     *   cb 涨、beat 停   → bt_audio_task 自己卡住（被下游锁死）
     *   cb 停、beat 涨   → 数据没到，但任务活着 */
    uint32_t             a2dp_cb_cnt;       /* A2DP 数据回调累计次数 */
    uint32_t             task_beat;         /* 模块任务主循环累计圈数 */
    uint32_t             warn_last_ms;      /* 限流打印用：上次告警的 tick(ms) */

    /* HFP 状态 */
    bool                 hfp_connected;    /* SLC 已建立 */
    bool                 hfp_audio_open;  /* SCO/eSCO 已打开 */
    bool                 hfp_msbsbc;      /* 当前为 mSBC（否则 CVSD） */
    uint8_t              hfp_remote_addr[6];
    uint64_t             hfp_rs_pos;       /* 上行重采样 16.16 定点位置（跨帧连续） */
    int16_t              hfp_rs_in[BT_HFP_RS_IN_MAX_SAMPLES]; /* 上行重采样输入缓冲 */
    SemaphoreHandle_t    hfp_audio_sem;   /* SCO 打开信号（hfp_wait_audio_open 阻塞用） */

    /* ---- 常规状态 ---- */
    bool                 stream_started; /* 当前是否存在 A2DP 音频流 */
    uint8_t              volume;         /* 本地音量（0..100，AVRCP 已同步手机） */
    SemaphoreHandle_t    lock;

    /* ---- AVRCP 控制器状态（UI → 手机控制） ---- */
    bool                avrc_connected;        /* AVRCP（CT）链路已建立 */
    bool                avrc_abs_vol_ok;       /* 收到过 SetAbsoluteVolume 成功响应 */
    esp_avrc_rn_evt_cap_mask_t avrc_peer_rn_cap; /* 手机端支持的通知事件位掩码 */
    uint8_t             avrc_tl;               /* 事务标签循环 0..15 */
    bt_audio_play_state_t play_state;          /* 最近播放状态（AVRCP 上报 / 无 AVRCP 时推断） */
    uint32_t             play_pos_ms;            /* AVRCP 播放位置（ms，最近一次上报/响应） */
    bool                 play_pos_valid;          /* 是否收到过位置数据（断开/切曲时重置） */
    bt_audio_track_info_t track_info;          /* 最近曲目信息缓存（AVRCP 元数据） */
    bool                track_info_valid;
};

/**
 * 单实例（内部 DRAM 紧张：整个上下文 ~3.9KB 放 PSRAM）
 *
 * 上下文只在任务上下文（BTC / bt_audio_task）访问，不做 ISR/DMA，因此可以放
 * PSRAM。首次访问时惰性分配（PSRAM 优先，失败回退内部 RAM），并用宏别名保持
 * 既有 `s_bt_audio.xxx` 代码不变。
 */
static struct bt_audio_s *s_bt_audio_ptr = NULL;

static struct bt_audio_s *bt_audio_ctx(void)
{
    if (s_bt_audio_ptr == NULL)
    {
        s_bt_audio_ptr = (struct bt_audio_s *)heap_caps_calloc(
            1, sizeof(struct bt_audio_s), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_bt_audio_ptr == NULL)
        {
            s_bt_audio_ptr = (struct bt_audio_s *)heap_caps_calloc(
                1, sizeof(struct bt_audio_s), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
    }
    return s_bt_audio_ptr;
}

#define s_bt_audio (*bt_audio_ctx())

/** 蓝牙栈（控制器 + Bluedroid + A2DP 注册）是否已初始化 */
static bool s_stack_ready = false;

#define BT_AUDIO_LOCK()   xSemaphoreTake(s_bt_audio.lock, portMAX_DELAY)
#define BT_AUDIO_UNLOCK() xSemaphoreGive(s_bt_audio.lock)

/** 必须被看到的告警（限流 2s 一条）。
 *
 *  bt_audio 自己的 BT_AUDIO_LOGW 受 BT_AUDIO_DEBUG_LOGS 编译开关控制，
 *  该开关默认关闭 —— 于是 FIFO 溢出、欠载、错位这些**恰恰只在出问题时
 *  才发生**的事件，全都被编译掉了。用户看到的现象就是"卡死了，串口
 *  一个字都没有"，无法判断卡在哪一环。
 *  这里改用独立 tag "BT_PCM"，并做 2 秒限流，既不会刷屏也不会静默。 */
#define BT_PCM_WARN(ms_now, fmt, ...)                                        \
    do {                                                                     \
        uint32_t _ms = (uint32_t)(ms_now);                                    \
        if ((uint32_t)(_ms - s_bt_audio.warn_last_ms) >= 2000u) {             \
            s_bt_audio.warn_last_ms = _ms;                                    \
            ESP_LOGW("BT_PCM", fmt, ##__VA_ARGS__);                           \
        }                                                                     \
    } while (0)

/* ======================== 内部工具 ========================================== */

/** 向事件队列投递事件（协议栈上下文调用，只入队不回调） */
static void bt_audio_post_event(const bt_audio_event_t *evt)
{
    QueueHandle_t q;

    if (evt == NULL)
    {
        return;
    }

    q = s_bt_audio.evt_q;
    if (q == NULL)
    {
        return;
    }

    if (xQueueSend(q, evt, 0) != pdTRUE)
    {
        s_bt_audio.dropped_evt++;
    }
}

/** 从 SBC 能力结构解析采样率（与官方示例一致：位域值即 A2DP CIE 标志位） */
static uint32_t bt_audio_parse_sample_rate(const esp_a2d_mcc_t *mcc)
{
    uint8_t sf;

    if (mcc == NULL || mcc->type != ESP_A2D_MCT_SBC)
    {
        return 0;
    }

    sf = mcc->cie.sbc_info.samp_freq;
    if (sf & ESP_A2D_SBC_CIE_SF_48K) return 48000;
    if (sf & ESP_A2D_SBC_CIE_SF_44K) return 44100;
    if (sf & ESP_A2D_SBC_CIE_SF_32K) return 32000;
    if (sf & ESP_A2D_SBC_CIE_SF_16K) return 16000;
    return 0;
}

/** 从 SBC 能力结构解析声道数（Mono=1，其余按双声道） */
static uint8_t bt_audio_parse_channels(const esp_a2d_mcc_t *mcc)
{
    uint8_t cm;

    if (mcc == NULL || mcc->type != ESP_A2D_MCT_SBC)
    {
        return 0;
    }

    cm = mcc->cie.sbc_info.ch_mode;
    if (cm & ESP_A2D_SBC_CIE_CH_MODE_MONO)
    {
        return 1;
    }
    return 2;
}


/* ======================== HFP 重采样（16.16 定点线性插值） ==================== */

/**
 * @brief 单声道 16bit 线性插值重采样。
 * @param in      输入缓冲（44.1k mono）
 * @param in_n    输入样本数
 * @param out     输出缓冲（16/8k mono），容量须 >= out_n
 * @param out_n   需要输出的样本数
 * @param step    每输出样本前进的定点步长 = (src_rate << 16) / dst_rate
 * @param pos     位置状态（SCO 打开时清零，跨回调持续）
 * @return 实际输出样本数
 */
static uint32_t bt_audio_hfp_resample_16_16(const int16_t *in, uint32_t in_n,
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

/* HFP SCO 音频采样率：mSBC=16kHz，CVSD=8kHz（均为单声道 16bit） */
static uint32_t bt_audio_hfp_sco_rate(void)
{
    return s_bt_audio.hfp_msbsbc ? 16000 : 8000;
}

/* 上行重采样步长：44100 -> SCO 采样率 */
static uint64_t bt_audio_hfp_up_step(void)
{
    return ((uint64_t)44100 << 16) / bt_audio_hfp_sco_rate();
}

/* =================== 蓝牙协议栈回调（只入队/只记录） ========================= */

/** A2DP PCM 数据回调：协议栈上下文，只把 PCM 放入 FIFO，不调用任何用户回调 */
static void bt_a2dp_data_cb(const uint8_t *data, uint32_t len)
{
    StreamBufferHandle_t fifo;
    size_t space, room, whole, n, sent;
    uint64_t now_us;

    if (data == NULL || len == 0)
    {
        return;
    }

    fifo = s_bt_audio.pcm_fifo;
    if (fifo == NULL)
    {
        return;
    }

    /* ---- 到达间隔统计 ----
     * 手机 SBC 的 RTP 包间隔通常是 15~30ms。如果最大间隔稳定在某个
     * 固定值（比如 40/60/100ms），说明是**手机端在攒包**，不是干扰；
     * 如果间隔忽大忽小、最坏到 80ms 以上，那就是射频被抢导致的**丢包**。
     * 这两种情况的处理方式完全不同，所以必须分开测量。 */
    now_us = (uint64_t)esp_timer_get_time();
    if (s_bt_audio.a2dp_last_cb_us != 0)
    {
        uint32_t gap_ms = (uint32_t)((now_us - s_bt_audio.a2dp_last_cb_us) / 1000u);
        if (gap_ms > s_bt_audio.a2dp_gap_max_ms)
        {
            s_bt_audio.a2dp_gap_max_ms = gap_ms;
        }
    }
    s_bt_audio.a2dp_last_cb_us = now_us;
    s_bt_audio.a2dp_cb_cnt++;          /* 活体检测：协议栈还在往我们这里送数据 */

    /* 非阻塞写入：FIFO 满直接丢弃并计数，不阻塞协议栈任务。
     *
     * 但**不能**直接把 len 交给 xStreamBufferSend：空间不足时它会部分写入，
     * 而空闲字节数恒为奇数（见 BT_A2DP_FRAME_BYTES 注释），一旦部分写入就
     * 写进半个 int16，整条字节流永久错位 → 输出满量程噪声。
     * 所以这里先把"要写的量"和"能写的量"都裁剪到整帧边界：宁可丢整帧，
     * 也绝不让 FIFO 里出现非整帧字节数。 */
    whole = (size_t)len - ((size_t)len % BT_A2DP_FRAME_BYTES);
    if (whole == 0)
    {
        s_bt_audio.dropped_pcm += len;
        BT_PCM_WARN(now_us / 1000u, "A2DP chunk %u B not whole frame, drop",
                    (unsigned)len);
        return;
    }

    space = xStreamBufferSpacesAvailable(fifo);
    room  = space - (space % BT_A2DP_FRAME_BYTES);
    n     = (whole < room) ? whole : room;

    if (n == 0)
    {
        s_bt_audio.dropped_pcm += len;
        BT_PCM_WARN(now_us / 1000u, "A2DP FIFO full, drop %u B (consumer stuck?)",
                    (unsigned)len);
        return;
    }

    sent = xStreamBufferSend(fifo, data, n, 0);
    if (sent < n)
    {
        /* 空间已按整帧裁剪，正常不会走到这里；真发生了也只影响计数，
         * 因为 sent 必然是整帧的整数倍（send 只会写满 xSpace，而 xSpace
         * 已由我们按整帧对齐过）。 */
        BT_PCM_WARN(now_us / 1000u, "A2DP short write %u/%u B",
                    (unsigned)sent, (unsigned)n);
    }
    if (n < (size_t)len)
    {
        s_bt_audio.dropped_pcm += (uint32_t)((size_t)len - n);
        BT_PCM_WARN(now_us / 1000u, "A2DP FIFO overflow, drop %u B",
                    (unsigned)((size_t)len - n));
    }
}

/** A2DP 状态回调：协议栈上下文，只更新内部状态并投递事件 */
static void bt_a2dp_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event)
    {
    case ESP_A2D_CONNECTION_STATE_EVT:
    {
        bt_audio_event_t evt;
        memset(&evt, 0, sizeof(evt));
        memcpy(evt.remote_addr, param->conn_stat.remote_bda, 6);

        if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED)
        {
            evt.id = BT_AUDIO_EVT_A2DP_CONNECTED;

            BT_AUDIO_LOCK();
            s_bt_audio.a2dp.connected         = true;
            s_bt_audio.a2dp.sample_rate       = 0;   /* 在 AUDIO_CFG 事件里更新 */
            s_bt_audio.a2dp.channels          = 0;
            s_bt_audio.a2dp.bits_per_sample   = 16;
            s_bt_audio.a2dp_primed            = false;   /* 新链路：重新预灌 */
            s_bt_audio.a2dp_prime_bytes       = BT_A2DP_PREBUFFER_BYTES;
            s_bt_audio.a2dp_underrun_cnt      = 0;
            s_bt_audio.a2dp_idle_cnt          = 0;
            s_bt_audio.a2dp_idle_run          = 0;
            s_bt_audio.a2dp_rs_rate           = 0;       /* 重采样相位作废（任务侧复位） */
            memcpy(s_bt_audio.remote_addr, param->conn_stat.remote_bda, 6);
            BT_AUDIO_UNLOCK();

            BT_AUDIO_LOGI("A2DP connected");
        }
        else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED)
        {
            evt.id = BT_AUDIO_EVT_A2DP_DISCONNECTED;

            BT_AUDIO_LOCK();
            s_bt_audio.a2dp.connected         = false;
            s_bt_audio.a2dp.sample_rate       = 0;
            s_bt_audio.a2dp.channels          = 0;
            s_bt_audio.stream_started         = false;
            s_bt_audio.a2dp_primed            = false;
            s_bt_audio.a2dp_prime_bytes       = BT_A2DP_PREBUFFER_BYTES;
            s_bt_audio.a2dp_rs_rate           = 0;       /* 重采样相位作废（任务侧复位） */
            memset(s_bt_audio.remote_addr, 0, sizeof(s_bt_audio.remote_addr));
            BT_AUDIO_UNLOCK();

            BT_AUDIO_LOGI("A2DP disconnected");

            /* 异常断开时 stream_stopped 不一定先到，这里兜底还射频给 WiFi */
            (void)esp_coex_status_bit_clear(ESP_COEX_ST_TYPE_BT,
                                            ESP_COEX_BT_ST_A2DP_STREAMING);
        }
        else
        {
            break;
        }
        bt_audio_post_event(&evt);
        break;
    }

    case ESP_A2D_AUDIO_STATE_EVT:
    {
        bt_audio_event_t evt;
        memset(&evt, 0, sizeof(evt));
        memcpy(evt.remote_addr, param->audio_stat.remote_bda, 6);

        if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED)
        {
            evt.id = BT_AUDIO_EVT_A2DP_STREAM_STARTED;
            BT_AUDIO_LOCK();
            s_bt_audio.stream_started    = true;
            s_bt_audio.a2dp_primed       = false;   /* 开流：先攒够预灌水位再放行 */
            s_bt_audio.a2dp_prime_bytes  = BT_A2DP_PREBUFFER_BYTES;
            s_bt_audio.a2dp_idle_run     = 0;
            s_bt_audio.a2dp_last_cb_us   = 0;
            s_bt_audio.a2dp_gap_max_ms   = 0;
            s_bt_audio.a2dp_rs_rate      = 0;   /* 开流：重采样相位从块首重新起 */
            BT_AUDIO_UNLOCK();
            BT_AUDIO_LOGI("A2DP stream started");

            /* 软件共存的官方"我在放 A2DP"标志位。
             *
             * 主节点同时还在用 WiFi 每 15ms 给从机单播一包音频，两条链路
             * 抢同一个 2.4G 天线。不置这个位时共存器按 BALANCE 处理，WiFi
             * 的长帧会把蓝牙的接收窗口挤掉 → A2DP 包被链路层丢弃（手机侧
             * 早已发出，我们这边永远收不到）→ 音频内容出现 80ms 级的空洞，
             * 听感就是"老旧收音机"那种断续。
             *
             * 置位后共存器会为经典蓝牙留出更确定的窗口。代价是 WiFi 单播
             * 的发包时刻会被推迟若干 ms —— 从机侧有 200ms 延迟队列和 PLC，
             * 完全吃得下；而蓝牙是实时流，没有任何补救手段。 */
            (void)esp_coex_status_bit_set(ESP_COEX_ST_TYPE_BT,
                                          ESP_COEX_BT_ST_A2DP_STREAMING);
        }
        else if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_SUSPEND)
        {
            evt.id = BT_AUDIO_EVT_A2DP_STREAM_STOPPED;
            BT_AUDIO_LOCK();
            s_bt_audio.stream_started    = false;
            s_bt_audio.a2dp_primed       = false;   /* 暂停期间 FIFO 里只剩残帧 */
            s_bt_audio.a2dp_prime_bytes  = BT_A2DP_PREBUFFER_BYTES;
            s_bt_audio.a2dp_idle_run     = 0;
            BT_AUDIO_UNLOCK();
            BT_AUDIO_LOGI("A2DP stream stopped");

            /* 停流后立刻交还射频给 WiFi（否则 WiFi 发包会一直被压着） */
            (void)esp_coex_status_bit_clear(ESP_COEX_ST_TYPE_BT,
                                            ESP_COEX_BT_ST_A2DP_STREAMING);
        }
        else
        {
            break;
        }
        bt_audio_post_event(&evt);
        break;
    }

    case ESP_A2D_AUDIO_CFG_EVT:
    {
        uint32_t rate = bt_audio_parse_sample_rate(&param->audio_cfg.mcc);
        uint8_t  ch   = bt_audio_parse_channels(&param->audio_cfg.mcc);
        bt_audio_event_t evt;

        if (rate == 0)
        {
            rate = 44100;   /* 解析失败兜底 */
        }

        BT_AUDIO_LOCK();
        s_bt_audio.a2dp.sample_rate = rate;
        s_bt_audio.a2dp.channels    = ch;
        /* 采样率/声道变了 ⇒ FIFO 里旧格式的数据不能和新数据混着喂，
         * 丢掉重来并重新预灌（正常只在切歌/换编码时发生一次） */
        s_bt_audio.a2dp_primed      = false;
        s_bt_audio.a2dp_prime_bytes = BT_A2DP_PREBUFFER_BYTES;
        s_bt_audio.a2dp_rs_rate     = 0;   /* 采样率/声道变了：相位作废 */
        BT_AUDIO_UNLOCK();

        memset(&evt, 0, sizeof(evt));
        evt.id             = BT_AUDIO_EVT_A2DP_SAMPLE_RATE_CHANGED;
        evt.data.sample_rate = rate;
        memcpy(evt.remote_addr, param->audio_cfg.remote_bda, 6);
        bt_audio_post_event(&evt);

        BT_AUDIO_LOGI("A2DP audio cfg: rate=%" PRIu32 " Hz, ch=%" PRIu8 ", bits=16", rate, ch);
        break;
    }

    default:
        break;   /* profile 状态 / 媒体 ACK 等暂不处理 */
    }
}

/* ======================= HFP（语音）协议栈回调 =============================== */

/** HFP 下行（手机→板）SCO PCM：协议栈上下文只入 FIFO，不调用用户回调 */
static void bt_hfp_data_recv_cb(const uint8_t *data, uint32_t length)
{
    StreamBufferHandle_t fifo;
    size_t sent;

    if (data == NULL || length == 0)
    {
        return;
    }

    fifo = s_bt_audio.hfp_dn_fifo;
    if (fifo == NULL)
    {
        return;
    }

    /* 非阻塞写入：FIFO 满直接丢弃并计数，不阻塞协议栈任务 */
    sent = xStreamBufferSend(fifo, data, length, 0);
    if (sent != length)
    {
        s_bt_audio.hfp_dn_dropped += (uint32_t)(length - sent);
        BT_AUDIO_LOGW("HFP downlink FIFO overflow, drop %u bytes", (unsigned)(length - sent));
    }

    /* 关键：协议栈只有收到 CI_SCO_DATA 事件才拉取上行数据。
     * 官方模式：每个下行帧到达时调用一次，驱动 send_cb 从上行 FIFO 取数 */
    esp_hf_client_outgoing_data_ready();
}

/** HFP 上行（板→手机）PCM 回调：把上行 FIFO 的 44.1k 单声道降到 SCO 速率 */
static uint32_t bt_hfp_data_send_cb(uint8_t *out, uint32_t length)
{
    uint32_t out_samples, in_need, got;

    if (out == NULL || length == 0 || !s_bt_audio.hfp_audio_open)
    {
        return 0;
    }

    out_samples = length / 2;
    if (out_samples == 0)
    {
        return 0;
    }

    /* 需要的 44.1k 输入样本数 = out_samples * 44100 / sco_rate + 1 */
    in_need = (uint32_t)(((uint64_t)out_samples * 44100) / bt_audio_hfp_sco_rate()) + 1;
    if (in_need > BT_HFP_RS_IN_MAX_SAMPLES)
    {
        in_need = BT_HFP_RS_IN_MAX_SAMPLES;
    }

    got = (uint32_t)xStreamBufferReceive(s_bt_audio.hfp_up_fifo, s_bt_audio.hfp_rs_in,
                                         in_need * 2, 0);
    if (got < in_need * 2)
    {
        /* 上行数据不足：返回 0（协议栈跳过该帧），避免空帧占空中速率 */
        return 0;
    }

    bt_audio_hfp_resample_16_16(s_bt_audio.hfp_rs_in, got / 2,
                                (int16_t *)out, out_samples,
                                bt_audio_hfp_up_step(), &s_bt_audio.hfp_rs_pos);

    /* 游标折回本帧窗口：数据每次只提供 in_need 个新样本，
     * 16.16 定点位置不反折回将在第二帧起全部取到末尾样本（静音等效）。 */
    s_bt_audio.hfp_rs_pos -= (uint64_t)in_need << 16;
    if ((int64_t)s_bt_audio.hfp_rs_pos < 0)
    {
        s_bt_audio.hfp_rs_pos = 0;
    }
    return length;
}

/** HFP 事件回调：协议栈上下文，只更新内部状态并投递事件 */
static void bt_hfp_cb(esp_hf_client_cb_event_t event, esp_hf_client_cb_param_t *param)
{
    bt_audio_event_t evt;

    switch (event)
    {
    case ESP_HF_CLIENT_CONNECTION_STATE_EVT:
    {
        if (param->conn_stat.state == ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED)
        {
            BT_AUDIO_LOCK();
            s_bt_audio.hfp_connected = true;
            memcpy(s_bt_audio.hfp_remote_addr, param->conn_stat.remote_bda, 6);
            BT_AUDIO_UNLOCK();

            memset(&evt, 0, sizeof(evt));
            evt.id = BT_AUDIO_EVT_HFP_CONNECTED;
            memcpy(evt.remote_addr, param->conn_stat.remote_bda, 6);
            bt_audio_post_event(&evt);
            BT_AUDIO_LOGI("HFP SLC connected");
        }
        else if (param->conn_stat.state == ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED)
        {
            BT_AUDIO_LOCK();
            s_bt_audio.hfp_connected = false;
            s_bt_audio.hfp_audio_open = false;
            memset(s_bt_audio.hfp_remote_addr, 0, 6);
            BT_AUDIO_UNLOCK();

            memset(&evt, 0, sizeof(evt));
            evt.id = BT_AUDIO_EVT_HFP_DISCONNECTED;
            memcpy(evt.remote_addr, param->conn_stat.remote_bda, 6);
            bt_audio_post_event(&evt);
            BT_AUDIO_LOGI("HFP SLC disconnected");
        }
        break;
    }

    case ESP_HF_CLIENT_AUDIO_STATE_EVT:
    {
        esp_hf_client_audio_state_t st = param->audio_stat.state;

        if (st == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED ||
            st == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC)
        {
            BT_AUDIO_LOCK();
            s_bt_audio.hfp_audio_open = true;
            s_bt_audio.hfp_msbsbc = (st == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC);
            s_bt_audio.hfp_rs_pos = 0;
            BT_AUDIO_UNLOCK();

            if (s_bt_audio.hfp_up_fifo != NULL)
            {
                xStreamBufferReset(s_bt_audio.hfp_up_fifo);
            }
            if (s_bt_audio.hfp_dn_fifo != NULL)
            {
                xStreamBufferReset(s_bt_audio.hfp_dn_fifo);
            }

            /* SCO 打开后注册 PCM 收/发回调（legacy 接口，内部 codec 输出 16bit PCM） */
            if (s_bt_audio.hfp_audio_sem != NULL)
            {
                xSemaphoreGive(s_bt_audio.hfp_audio_sem);
            }
            esp_hf_client_register_data_callback(bt_hfp_data_recv_cb, bt_hfp_data_send_cb);

            memset(&evt, 0, sizeof(evt));
            evt.id = BT_AUDIO_EVT_HFP_AUDIO_OPEN;
            memcpy(evt.remote_addr, param->audio_stat.remote_bda, 6);
            evt.data.sample_rate = bt_audio_hfp_sco_rate();
            bt_audio_post_event(&evt);
            BT_AUDIO_LOGI("HFP SCO open (%s, %" PRIu32 " Hz, frame=%u B)",
                     s_bt_audio.hfp_msbsbc ? "mSBC" : "CVSD",
                     bt_audio_hfp_sco_rate(), param->audio_stat.preferred_frame_size);
        }
        else if (st == ESP_HF_CLIENT_AUDIO_STATE_DISCONNECTED)
        {
            esp_hf_client_register_data_callback(NULL, NULL);

            BT_AUDIO_LOCK();
            s_bt_audio.hfp_audio_open = false;
            s_bt_audio.hfp_msbsbc = false;
            BT_AUDIO_UNLOCK();

            if (s_bt_audio.hfp_up_fifo != NULL)
            {
                xStreamBufferReset(s_bt_audio.hfp_up_fifo);
            }
            if (s_bt_audio.hfp_dn_fifo != NULL)
            {
                xStreamBufferReset(s_bt_audio.hfp_dn_fifo);
            }

            memset(&evt, 0, sizeof(evt));
            evt.id = BT_AUDIO_EVT_HFP_AUDIO_CLOSE;
            memcpy(evt.remote_addr, param->audio_stat.remote_bda, 6);
            bt_audio_post_event(&evt);
            BT_AUDIO_LOGI("HFP SCO closed");
        }
        break;
    }

    case ESP_HF_CLIENT_BVRA_EVT:
        BT_AUDIO_LOGI("HFP voice recognition %s",
                 param->bvra.value == ESP_HF_VR_STATE_ENABLED ? "ENABLED" : "DISABLED");
        break;

    case ESP_HF_CLIENT_VOLUME_CONTROL_EVT:
    {
        /* AG 通过 +VGM/+VGS 调整本机音量（HF 侧） */
        if (param->volume_control.type == ESP_HF_VOLUME_CONTROL_TARGET_SPK)
        {
            /* 归一化：HFP +VGS 音量刻度为 0~15，而 AVRCP 绝对音量为 0~100，
             * 统一转成 0~100，仅用于日志/调试。 */
            uint32_t v015 = (uint32_t)param->volume_control.volume;
            if (v015 > 15) { v015 = 15; }
            uint8_t v100 = (uint8_t)((v015 * 100) / 15);

            /* 关键：AG 上报的 +VGS 不作为本机音量下发（不投事件、不改功放、不落 NVS）。
             * 手机的通话/SCO 音量流与媒体音量流是两个独立刻度，语音助手或通话
             * 期间 AG 常上报 +VGS=1（归一化后 6%）；若直接跟随，功放会被压到几乎
             * 无声，而用户按的是媒体音量键、怎么调都恢复不了（第十阶段问题复现）。
             * HFP 下行（语音助手/通话）响度统一由本机音量决定，这里只记录。 */
            BT_AUDIO_LOGI("HFP +VGS=%u (0~15) -> %u%% (recorded only, local volume kept)",
                     (unsigned)v015, (unsigned)v100);
        }
        break;
    }

    default:
        break;
    }
}

/** GAP 回调：配对（SSP 自动确认 / PIN 兜底） */
static void bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event)
    {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        BT_AUDIO_LOGI("pair %s (%02x:%02x:%02x)",
                 (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) ? "success" : "failed",
                 param->auth_cmpl.bda[0], param->auth_cmpl.bda[1], param->auth_cmpl.bda[2]);
        break;

    case ESP_BT_GAP_CFM_REQ_EVT:
        /* 测试阶段自动确认；后续可改为 UI 弹窗确认 */
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        BT_AUDIO_LOGI("SSP confirm auto-accept");
        break;

    case ESP_BT_GAP_KEY_REQ_EVT:
    {
        /* esp_bt_pin_code_t 为 16 字节数组，必须用零填充缓冲区传递 */
        esp_bt_pin_code_t pin_code = {0};
        memcpy(pin_code, BT_PIN_CODE, strlen(BT_PIN_CODE));
        esp_bt_gap_pin_reply(param->key_req.bda, true, strlen(BT_PIN_CODE), pin_code);
        BT_AUDIO_LOGI("PIN reply: %s", BT_PIN_CODE);
        break;
    }

    case ESP_BT_GAP_KEY_NOTIF_EVT:
        BT_AUDIO_LOGI("PASSKEY: %" PRIu32, param->key_notif.passkey);
        break;

    default:
        break;
    }
}

/* =================== AVRCP（UI 控制手机）协议栈回调 =========================== */

/** 事务标签分配（0..15 循环，收发方向共用互不冲突） */
static uint8_t bt_audio_avrc_alloc_tl(void)
{
    uint8_t tl = s_bt_audio.avrc_tl;
    s_bt_audio.avrc_tl = (uint8_t)((s_bt_audio.avrc_tl + 1) % (ESP_AVRC_TRANS_LABEL_MAX + 1));
    return tl;
}

/** 请求手机曲目元数据（标题/歌手/专辑/曲目号/总曲目/时长） */
static void bt_audio_avrc_request_metadata(void)
{
    uint8_t mask = ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST |
                   ESP_AVRC_MD_ATTR_ALBUM | ESP_AVRC_MD_ATTR_TRACK_NUM |
                   ESP_AVRC_MD_ATTR_NUM_TRACKS | ESP_AVRC_MD_ATTR_PLAYING_TIME;
    esp_avrc_ct_send_metadata_cmd(bt_audio_avrc_alloc_tl(), mask);
}

/** 按手机能力注册通知（TRACK_CHANGE / PLAY_STATUS_CHANGE / VOLUME_CHANGE） */
static void bt_audio_avrc_register_rn(void)
{
    esp_avrc_rn_evt_cap_mask_t *cap = &s_bt_audio.avrc_peer_rn_cap;

    if (esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, cap,
                                           ESP_AVRC_RN_TRACK_CHANGE))
    {
        esp_avrc_ct_send_register_notification_cmd(bt_audio_avrc_alloc_tl(),
                                                   ESP_AVRC_RN_TRACK_CHANGE, 0);
    }
    if (esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, cap,
                                           ESP_AVRC_RN_PLAY_STATUS_CHANGE))
    {
        esp_avrc_ct_send_register_notification_cmd(bt_audio_avrc_alloc_tl(),
                                                   ESP_AVRC_RN_PLAY_STATUS_CHANGE, 0);
    }
    if (esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, cap,
                                           ESP_AVRC_RN_VOLUME_CHANGE))
    {
        esp_avrc_ct_send_register_notification_cmd(bt_audio_avrc_alloc_tl(),
                                                   ESP_AVRC_RN_VOLUME_CHANGE, 0);
    }
    if (esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, cap,
                                           ESP_AVRC_RN_PLAY_POS_CHANGED))
    {
        esp_avrc_ct_send_register_notification_cmd(bt_audio_avrc_alloc_tl(),
                                                   ESP_AVRC_RN_PLAY_POS_CHANGED,
                                                   BT_AUDIO_POS_NOTIFY_INTERVAL_MS);
    }
}

/** AVRCP 播放状态 → 模块枚举 */
static bt_audio_play_state_t bt_audio_avrc_map_playback(esp_avrc_playback_stat_t st)
{
    switch (st)
    {
    case ESP_AVRC_PLAYBACK_PLAYING: return BT_AUDIO_PLAY_STATE_PLAYING;
    case ESP_AVRC_PLAYBACK_PAUSED:  return BT_AUDIO_PLAY_STATE_PAUSED;
    case ESP_AVRC_PLAYBACK_STOPPED: return BT_AUDIO_PLAY_STATE_STOPPED;
    default:                        return BT_AUDIO_PLAY_STATE_UNKNOWN;
    }
}

/** 更新播放状态并投递事件（协议栈上下文：只改内部状态 + post_event） */
static void bt_audio_avrc_set_playback(bt_audio_play_state_t st)
{
    bt_audio_event_t evt;

    BT_AUDIO_LOCK();
    s_bt_audio.play_state = st;
    BT_AUDIO_UNLOCK();

    memset(&evt, 0, sizeof(evt));
    evt.id = BT_AUDIO_EVT_PLAY_STATE_CHANGED;
    evt.data.play_state = st;
    bt_audio_post_event(&evt);
}

/**
 * 更新本地音量并投递事件（协议栈上下文）
 *
 * 只用于 AVRCP 绝对音量（媒体音量，0~127），与 HFP +VGS 通话音量是两个独立
 * 刻度，这里不做混用：AVRCP 跟随手机媒体音量是设计行为，HFP +VGS 只记录。
 *
 * @param avrc_raw AVRCP 绝对音量原始值 0~127
 */
static void bt_audio_avrc_set_volume_local(uint8_t avrc_raw)
{
    bt_audio_event_t evt;
    uint8_t vol100;

    if (avrc_raw > 127)
    {
        avrc_raw = 127;
    }
    vol100 = BT_VOL_AVRC_TO_LOCAL(avrc_raw);

    BT_AUDIO_LOCK();
    s_bt_audio.volume = vol100;
    BT_AUDIO_UNLOCK();

    memset(&evt, 0, sizeof(evt));
    evt.id = BT_AUDIO_EVT_VOLUME_CHANGED;
    evt.data.volume = vol100;
    bt_audio_post_event(&evt);

    BT_AUDIO_LOGI("AVRCP abs vol %u (0~127) -> %u%% (media volume, applied)",
             (unsigned)avrc_raw, (unsigned)vol100);
}

/** 定长安全拷贝（UTF-8 元数据，超长截断并补 NUL） */
static void bt_audio_avrc_copy_str(char *dst, size_t dst_cap,
                                   const uint8_t *src, int src_len)
{
    size_t n = (src_len < 0) ? 0 : (size_t)src_len;
    if (n >= dst_cap)
    {
        n = dst_cap - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/** 元数据单属性（ESP_AVRC_CT_METADATA_RSP_EVT）写入缓存并投递 TRACK_INFO 事件 */
static void bt_audio_avrc_meta_handle(uint8_t attr_id, const uint8_t *text, int len)
{
    bt_audio_event_t evt;
    char num_str[16];

    if (text == NULL || len <= 0)
    {
        return;
    }

    switch (attr_id)
    {
    case ESP_AVRC_MD_ATTR_TITLE:
        bt_audio_avrc_copy_str(s_bt_audio.track_info.title,
                               sizeof(s_bt_audio.track_info.title), text, len);
        break;
    case ESP_AVRC_MD_ATTR_ARTIST:
        bt_audio_avrc_copy_str(s_bt_audio.track_info.artist,
                               sizeof(s_bt_audio.track_info.artist), text, len);
        break;
    case ESP_AVRC_MD_ATTR_ALBUM:
        bt_audio_avrc_copy_str(s_bt_audio.track_info.album,
                               sizeof(s_bt_audio.track_info.album), text, len);
        break;
    case ESP_AVRC_MD_ATTR_TRACK_NUM:
    case ESP_AVRC_MD_ATTR_NUM_TRACKS:
    case ESP_AVRC_MD_ATTR_PLAYING_TIME:
        bt_audio_avrc_copy_str(num_str, sizeof(num_str), text, len);
        if (attr_id == ESP_AVRC_MD_ATTR_TRACK_NUM)
        {
            s_bt_audio.track_info.track_num = (uint32_t)strtoul(num_str, NULL, 10);
        }
        else if (attr_id == ESP_AVRC_MD_ATTR_NUM_TRACKS)
        {
            s_bt_audio.track_info.total_tracks = (uint32_t)strtoul(num_str, NULL, 10);
        }
        else
        {
            s_bt_audio.track_info.duration_ms = (uint32_t)strtoul(num_str, NULL, 10);
        }
        break;
    default:
        return;   /* 未知属性：不投递 */
    }

    BT_AUDIO_LOCK();
    s_bt_audio.track_info_valid = true;
    BT_AUDIO_UNLOCK();

    memset(&evt, 0, sizeof(evt));
    evt.id = BT_AUDIO_EVT_TRACK_INFO;
    BT_AUDIO_LOCK();
    evt.data.track_info = s_bt_audio.track_info;
    BT_AUDIO_UNLOCK();
    bt_audio_post_event(&evt);
    BT_AUDIO_LOGD("AVRCP meta attr 0x%02x len %d", attr_id, len);
}

/** AVRCP 控制器回调：协议栈上下文，只更新内部状态并投递事件 */
static void bt_avrc_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    switch (event)
    {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
    {
        if (param->conn_stat.connected)
        {
            BT_AUDIO_LOCK();
            s_bt_audio.avrc_connected   = true;
            s_bt_audio.avrc_abs_vol_ok  = false;
            s_bt_audio.avrc_peer_rn_cap.bits = 0;
            BT_AUDIO_UNLOCK();

            /* 先取手机端能力（按支持程度注册通知） */
            esp_avrc_ct_send_get_rn_capabilities_cmd(bt_audio_avrc_alloc_tl());
            BT_AUDIO_LOGI("AVRCP connected");
        }
        else
        {
            BT_AUDIO_LOCK();
            s_bt_audio.avrc_connected      = false;
            s_bt_audio.avrc_abs_vol_ok     = false;
            s_bt_audio.avrc_peer_rn_cap.bits = 0;
            s_bt_audio.play_state          = BT_AUDIO_PLAY_STATE_UNKNOWN;
            s_bt_audio.track_info_valid    = false;
            s_bt_audio.play_pos_ms         = 0;
            s_bt_audio.play_pos_valid      = false;
            memset(&s_bt_audio.track_info, 0, sizeof(s_bt_audio.track_info));
            BT_AUDIO_UNLOCK();
            BT_AUDIO_LOGI("AVRCP disconnected");
        }
        break;
    }

    case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT:
        s_bt_audio.avrc_peer_rn_cap = param->get_rn_caps_rsp.evt_set;
        bt_audio_avrc_register_rn();
        /* 请求初始播放状态与曲目信息 */
        esp_avrc_ct_send_get_play_status_cmd(bt_audio_avrc_alloc_tl());
        bt_audio_avrc_request_metadata();
        break;

    case ESP_AVRC_CT_PLAY_STATUS_RSP_EVT:
    {
        /* 若手机没返回 PLAYING_TIME 元数据，用响应总时长的时长兜底 */
        if (s_bt_audio.track_info.duration_ms == 0 &&
            param->play_status_rsp.song_length != 0)
        {
            s_bt_audio.track_info.duration_ms = param->play_status_rsp.song_length;
        }
        bt_audio_avrc_set_playback(
            bt_audio_avrc_map_playback(param->play_status_rsp.play_status));
        break;
    }

    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
    {
        bt_audio_event_t evt;
        uint8_t eid = param->change_ntf.event_id;

        switch (eid)
        {
        case ESP_AVRC_RN_TRACK_CHANGE:
            /* 新曲加载：重新注册 + 拉最新元数据 */
            if (esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST,
                                                   &s_bt_audio.avrc_peer_rn_cap,
                                                   ESP_AVRC_RN_TRACK_CHANGE))
            {
                esp_avrc_ct_send_register_notification_cmd(
                    bt_audio_avrc_alloc_tl(), ESP_AVRC_RN_TRACK_CHANGE, 0);
            }
            /* 新曲位置重置；位置通知随新曲重新注册 */
            BT_AUDIO_LOCK();
            s_bt_audio.play_pos_ms    = 0;
            s_bt_audio.play_pos_valid = false;
            BT_AUDIO_UNLOCK();
            if (esp_avrc_rn_evt_bit_mask_operation(
                    ESP_AVRC_BIT_MASK_OP_TEST, &s_bt_audio.avrc_peer_rn_cap,
                    ESP_AVRC_RN_PLAY_POS_CHANGED))
            {
                esp_avrc_ct_send_register_notification_cmd(
                    bt_audio_avrc_alloc_tl(), ESP_AVRC_RN_PLAY_POS_CHANGED,
                    BT_AUDIO_POS_NOTIFY_INTERVAL_MS);
            }
            memset(&evt, 0, sizeof(evt));
            evt.id = BT_AUDIO_EVT_TRACK_CHANGED;
            bt_audio_post_event(&evt);
            bt_audio_avrc_request_metadata();
            break;

        case ESP_AVRC_RN_PLAY_STATUS_CHANGE:
            bt_audio_avrc_set_playback(
                bt_audio_avrc_map_playback(param->change_ntf.event_parameter.playback));
            if (esp_avrc_rn_evt_bit_mask_operation(
                    ESP_AVRC_BIT_MASK_OP_TEST, &s_bt_audio.avrc_peer_rn_cap,
                    ESP_AVRC_RN_PLAY_STATUS_CHANGE))
            {
                esp_avrc_ct_send_register_notification_cmd(
                    bt_audio_avrc_alloc_tl(), ESP_AVRC_RN_PLAY_STATUS_CHANGE, 0);
            }
            break;

        case ESP_AVRC_RN_VOLUME_CHANGE:
            bt_audio_avrc_set_volume_local(
                (uint8_t)param->change_ntf.event_parameter.volume);
            if (esp_avrc_rn_evt_bit_mask_operation(
                    ESP_AVRC_BIT_MASK_OP_TEST, &s_bt_audio.avrc_peer_rn_cap,
                    ESP_AVRC_RN_VOLUME_CHANGE))
            {
                esp_avrc_ct_send_register_notification_cmd(
                    bt_audio_avrc_alloc_tl(), ESP_AVRC_RN_VOLUME_CHANGE, 0);
            }
            break;

        case ESP_AVRC_RN_PLAY_POS_CHANGED:
            BT_AUDIO_LOCK();
            s_bt_audio.play_pos_ms    = param->change_ntf.event_parameter.play_pos;
            s_bt_audio.play_pos_valid = true;
            BT_AUDIO_UNLOCK();
            if (esp_avrc_rn_evt_bit_mask_operation(
                    ESP_AVRC_BIT_MASK_OP_TEST, &s_bt_audio.avrc_peer_rn_cap,
                    ESP_AVRC_RN_PLAY_POS_CHANGED))
            {
                esp_avrc_ct_send_register_notification_cmd(
                    bt_audio_avrc_alloc_tl(), ESP_AVRC_RN_PLAY_POS_CHANGED,
                    BT_AUDIO_POS_NOTIFY_INTERVAL_MS);
            }
            break;

        default:
            break;
        }
        break;
    }

    case ESP_AVRC_CT_METADATA_RSP_EVT:
        bt_audio_avrc_meta_handle(param->meta_rsp.attr_id,
                                  param->meta_rsp.attr_text,
                                  param->meta_rsp.attr_length);
        break;

    case ESP_AVRC_CT_SET_ABSOLUTE_VOLUME_RSP_EVT:
        s_bt_audio.avrc_abs_vol_ok = true;
        bt_audio_avrc_set_volume_local((uint8_t)param->set_volume_rsp.volume);
        break;

    case ESP_AVRC_CT_REMOTE_FEATURES_EVT:
        BT_AUDIO_LOGD("AVRCP remote features: 0x%" PRIx32, param->rmt_feats.feat_mask);
        break;

    case ESP_AVRC_CT_PASSTHROUGH_RSP_EVT:
        BT_AUDIO_LOGD("AVRCP pth rsp: key 0x%x rsp %d",
                 param->psth_rsp.key_code, param->psth_rsp.rsp_code);
        break;

    case ESP_AVRC_CT_PROF_STATE_EVT:
    default:
        break;
    }
}

/** AVRCP 目标（Target）回调：接受手机发起的 AVRCP 连接与绝对音量命令 */
static void bt_avrc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param)
{
    switch (event)
    {
    case ESP_AVRC_TG_CONNECTION_STATE_EVT:
        /* 手机（CT）建立 AVRCP 连接：A2DP Sink 必须注册 TG 才能接受 PSM 23 */
        BT_AUDIO_LOCK();
        s_bt_audio.avrc_connected = param->conn_stat.connected;
        if (param->conn_stat.connected)
        {
            s_bt_audio.avrc_abs_vol_ok = false;
        }
        else
        {
            s_bt_audio.avrc_peer_rn_cap.bits = 0;
            s_bt_audio.play_state          = BT_AUDIO_PLAY_STATE_UNKNOWN;
            s_bt_audio.track_info_valid    = false;
            s_bt_audio.play_pos_ms         = 0;
            s_bt_audio.play_pos_valid      = false;
            memset(&s_bt_audio.track_info, 0, sizeof(s_bt_audio.track_info));
        }
        BT_AUDIO_UNLOCK();
        BT_AUDIO_LOGI("AVRCP TG %s", param->conn_stat.connected ? "connected" : "disconnected");
        break;

    case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT:
        /* 手机调节音量：同步本地音量并上报 APP */
        bt_audio_avrc_set_volume_local((uint8_t)param->set_abs_vol.volume);
        break;

    case ESP_AVRC_TG_PASSTHROUGH_CMD_EVT:
        /* 手机通过透传键控制音箱（暂仅记录，后续可映射事件） */
        if (param->psth_cmd.key_state == ESP_AVRC_PT_CMD_STATE_PRESSED)
        {
            BT_AUDIO_LOGD("AVRCP TG pth key 0x%x", param->psth_cmd.key_code);
        }
        break;

    case ESP_AVRC_TG_REMOTE_FEATURES_EVT:
        BT_AUDIO_LOGD("AVRCP TG remote features: 0x%" PRIx32, param->rmt_feats.feat_mask);
        break;

    case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT:
    case ESP_AVRC_TG_PROF_STATE_EVT:
    default:
        break;
    }
}

/* ======================== FIFO 创建（PSRAM） ================================== */

/** 借用 PSRAM 创建 StreamBuffer：内部 RAM 仅保留控制块，数据缓冲放 PSRAM */
static StreamBufferHandle_t bt_audio_sbuf_create(size_t bytes,
                                                 StaticStreamBuffer_t *ctrl,
                                                 uint8_t **buf_out)
{
    uint8_t *buf = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL)
    {
        BT_AUDIO_LOGE("PSRAM alloc %u bytes failed", (unsigned)bytes);
        return NULL;
    }

    StreamBufferHandle_t h = xStreamBufferCreateStatic(bytes, 1, buf, ctrl);
    if (h == NULL)
    {
        heap_caps_free(buf);
        BT_AUDIO_LOGE("stream buffer create failed");
        return NULL;
    }
    *buf_out = buf;
    return h;
}

/* ======================== A2DP 采样率适配 ==================================== */

/**
 * @brief 把 A2DP 解出的 PCM 从源采样率线性插值到 44.1kHz（16bit 交错，多声道）
 *
 * 相位（pos）与"上一块最后一帧"（last[]）都**跨块保留**：插值绝不能按块重置，
 * 否则每个块边界都会产生一次相位跳变，听感是周期性的"咔"。
 *
 * 坐标系：pos 以"源采样帧"为单位、32.32 定点（32 位整数部分 + 32 位小数），
 * 相对**当前块**起点，允许为负（负值表示待输出点落在上一块末尾与当前块首帧
 * 之间，用 last[] 插值）。用 32.32 而不是 16.16：16.16 的步长量化误差会让
 * 48k→44.1k 的实际比例偏 13ppm（10 秒多出 4 帧），长期会把入口环慢慢灌满；
 * 32.32 的误差在 2^-32 量级，可以忽略。
 *
 * @param in         输入 PCM（源采样率，交错）
 * @param in_frames  输入帧数（一帧 = 所有声道各一个样点）
 * @param ch         声道数（1..2）
 * @param out        输出缓冲（44.1kHz，交错）
 * @param out_cap    输出缓冲能放的**帧**数
 * @return 实际输出帧数（0 表示不需要重采样或参数非法）
 */
static size_t bt_a2dp_resample_to_44k(const int16_t *in, size_t in_frames,
                                      uint8_t ch, int16_t *out, size_t out_cap)
{
    uint32_t src = s_bt_audio.a2dp.sample_rate;
    uint64_t step;
    int64_t  pos;
    size_t   out_n = 0;
    uint8_t  c;

    if (in == NULL || out == NULL || in_frames == 0 || out_cap == 0)
    {
        return 0;
    }
    if (ch == 0 || ch > 2)
    {
        ch = 2;
    }
    if (src == 0 || src == BT_A2DP_TARGET_RATE)
    {
        return 0;   /* 原生 44.1k：不插值，省 CPU 也避免无谓损失 */
    }

    step = ((uint64_t)src << 32) / BT_A2DP_TARGET_RATE;   /* 每输出一帧前进多少源帧 */
    if (step == 0)
    {
        return 0;
    }
    pos = s_bt_audio.a2dp_rs_pos;

    while (out_n < out_cap)
    {
        int64_t  idx  = pos >> 32;
        uint32_t frac = (uint32_t)(pos & 0xFFFFFFFF);

        if (idx + 1 >= (int64_t)in_frames)
        {
            break;   /* 尾部这一帧要等下一块的首帧才能插值 */
        }

        for (c = 0; c < ch; c++)
        {
            int32_t s0 = (idx < 0) ? (int32_t)s_bt_audio.a2dp_rs_last[c]
                                   : (int32_t)in[(size_t)idx * ch + c];
            int32_t s1 = (int32_t)in[(size_t)(idx + 1) * ch + c];

            /* 差值 × frac 最大 65535×2^32 ≈ 2.8e14，必须走 64 位 */
            out[out_n * ch + c] =
                (int16_t)(s0 + (int32_t)(((int64_t)(s1 - s0) * (int64_t)frac) >> 32));
        }
        out_n++;
        pos += (int64_t)step;
    }

    /* 记下本块最后一帧，供下一块做跨块插值 */
    for (c = 0; c < ch; c++)
    {
        s_bt_audio.a2dp_rs_last[c] = in[(in_frames - 1) * ch + c];
    }

    if (out_n >= out_cap)
    {
        /* 缓冲被填满（只可能是声道/块长配置异常）：丢掉本块剩余并复位相位，
         * 绝不能带着半截相位进下一块 —— 那会被 last[] 拉成一段斜坡噪声 */
        s_bt_audio.a2dp_rs_pos = 0;
        return out_n;
    }

    pos -= ((int64_t)in_frames << 32);   /* 换算到下一块的相对坐标（通常为负） */
    s_bt_audio.a2dp_rs_pos = pos;
    return out_n;
}

/* ======================== 模块任务 =========================================== */

/** bt_audio 任务：在安全上下文调用 on_event / on_pcm，参数在栈上取值 */
static void bt_audio_task(void *arg)
{
    /* 大 PCM 缓冲放 PSRAM（start 时分配）：任务栈仅保留事件结构体与调用帧 */
    int16_t *pcm_buf = s_bt_audio.task_pcm_buf;
    int16_t *hfp_dn_raw = s_bt_audio.task_hfp_dn_raw;
    bt_audio_event_t evt;

    (void)arg;

    /* 订阅任务看门狗：这一环卡住 = 音乐立刻断，而它下游全是 portMAX_DELAY
     * 的互斥量，一旦卡住就是"静默死机"（串口一个字都没有）。订阅之后，
     * 超过 CONFIG_ESP_TASK_WDT_TIMEOUT_S 未喂狗，TWDT 会打印所有任务的
     * 运行状态，直接指出是谁卡住了。 */
    (void)esp_task_wdt_add(NULL);

    for (;;)
    {
        (void)esp_task_wdt_reset();
        s_bt_audio.task_beat++;        /* 活体检测：本任务主循环还在转 */

        /* 事件优先：非阻塞取一条 */
        if (xQueueReceive(s_bt_audio.evt_q, &evt, 0) == pdTRUE)
        {
            bt_audio_event_cb_t cb = s_bt_audio.on_event;
            void *ctx = s_bt_audio.event_ctx;
            if (cb != NULL)
            {
                cb(&evt, ctx);
            }
            continue;
        }

        /* 其次处理 HFP 下行（应答）：SCO 原始采样 → 44.1kHz 单声道 */
        if (s_bt_audio.hfp_audio_open && s_bt_audio.hfp_dn_fifo != NULL)
        {
            size_t dn = xStreamBufferReceive(s_bt_audio.hfp_dn_fifo, hfp_dn_raw,
                                             BT_HFP_DN_CHUNK_BYTES, 0);
            if (dn >= 2)
            {
                uint32_t sco_rate = bt_audio_hfp_sco_rate();
                uint32_t out_n = (uint32_t)(((uint64_t)(dn / 2) * 44100) / sco_rate);
                uint64_t dn_pos = 0;

                bt_audio_hfp_resample_16_16(hfp_dn_raw, (uint32_t)(dn / 2),
                                            pcm_buf, out_n,
                                            ((uint64_t)sco_rate << 16) / 44100, &dn_pos);
                bt_audio_pcm_cb_t cb = s_bt_audio.on_pcm;
                void *ctx = s_bt_audio.pcm_ctx;
                if (cb != NULL)
                {
                    cb(BT_AUDIO_PCM_SOURCE_HFP_DOWNLINK, pcm_buf, out_n, 44100, ctx);
                }
                continue;
            }
        }

        /* ---- A2DP 抖动缓冲：开流预灌 ----
         * 蓝牙是严格实时的音源（手机喂多少就只有多少），数据一到就往下走，
         * 于是链路的抗抖动能力只剩 sync 的 200ms 延迟队列，而 SD 卡路径的
         * 解码端能跑在实时速率之前、把 64KB 入口环常年灌满。这里先在本地
         * FIFO 里攒够 a2dp_prime_bytes 再放行，给蓝牙补上同量级的缓冲余量。 */
        if (s_bt_audio.stream_started && !s_bt_audio.a2dp_primed &&
            s_bt_audio.pcm_fifo != NULL)
        {
            uint32_t need = (s_bt_audio.a2dp_prime_bytes > 0)
                                ? s_bt_audio.a2dp_prime_bytes
                                : BT_A2DP_PREBUFFER_BYTES;
            if (xStreamBufferBytesAvailable(s_bt_audio.pcm_fifo) < (size_t)need)
            {
                /* 还不够：让协议栈回调继续往里灌，任务这边小睡等待 */
                vTaskDelay(pdMS_TO_TICKS(2));
                continue;
            }
            s_bt_audio.a2dp_primed = true;
            BT_AUDIO_LOGI("A2DP prebuffer ready: %u bytes", (unsigned)need);
        }

        /* 最后处理 A2DP PCM（20ms 超时，兼顾事件实时性） */
        size_t bytes = xStreamBufferReceive(s_bt_audio.pcm_fifo, pcm_buf,
                                            BT_PCM_CHUNK_SAMPLES * sizeof(int16_t),
                                            pdMS_TO_TICKS(20));
        if (bytes == 0)
        {
            /* ===== 这里曾经是个"人造断音"的坑，务必别改回去 =====
             *
             * xStreamBufferReceive 返回 0 只代表"这 20ms 窗口里 FIFO 是空的"，
             * 不代表手机断供：本任务只要 FIFO 里有数据就立刻取走，而手机
             * SBC 的 RTP 包间隔本身就有 15~30ms，所以两次取之间 FIFO 空着
             * 是**稳态**，不是异常。
             *
             * 之前把这里当成欠载、重新预灌 8KB(≈46ms)，等于每 20ms 就可能
             * 人为停供 46ms。sync 层只有 200ms 延迟队列，被这样反复抽，
             * 就会周期性露底 → sync_feed_silence() 播静音 → 听感"偶尔卡一下"。
             *
             * 现在只做统计。真欠载的判据是"连续多轮完全没数据"（见下），
             * 且即便那样也**不重灌**——重灌只会让缺口变成有保证的 46ms，
             * 缓冲该由 sync 的 D 来兜，不该在生产者这一侧再砍一刀。
             */
            s_bt_audio.a2dp_idle_cnt++;
            if (s_bt_audio.stream_started && s_bt_audio.a2dp_idle_run < 0xFF)
            {
                s_bt_audio.a2dp_idle_run++;
            }
            /* 连续 4 轮（约 80ms）完全没有新数据，才记一次真欠载：
             * 这个量级手机侧补不回来，下游一定露底，用来定位是"手机/共存
             * 断供"还是"我们自己的时序问题" */
            if (s_bt_audio.stream_started && s_bt_audio.a2dp_idle_run == 4)
            {
                s_bt_audio.a2dp_underrun_cnt++;
                BT_PCM_WARN(esp_timer_get_time() / 1000,
                            "A2DP starved ~80ms (cnt=%u, avail=%u)",
                            (unsigned)s_bt_audio.a2dp_underrun_cnt,
                            (unsigned)xStreamBufferBytesAvailable(s_bt_audio.pcm_fifo));
            }
            continue;
        }

        /* 拿到了数据：清掉连续读空计数 */
        s_bt_audio.a2dp_idle_run = 0;

        /* 半个采样说明 FIFO 里的字节流已经错位（正常由上面 bt_a2dp_data_cb
         * 按整帧写入保证不会发生）。这种情况下继续往下播只会输出满量程
         * 噪声且永不自愈，所以直接复位整条流、丢掉这一块，相当于一次
         * 微小的断音，而不是一直"吱吱吱"。 */
        if ((bytes % BT_A2DP_FRAME_BYTES) != 0)
        {
            s_bt_audio.resync_cnt++;
            s_bt_audio.a2dp_primed      = false;          /* 复位后重新攒余量 */
            s_bt_audio.a2dp_prime_bytes = BT_A2DP_REPRIME_BYTES;
            xStreamBufferReset(s_bt_audio.pcm_fifo);
            ESP_LOGE("BT_PCM", "A2DP FIFO misaligned (got=%u B), reset (cnt=%u)",
                     (unsigned)bytes, (unsigned)s_bt_audio.resync_cnt);
            continue;
        }

        bt_audio_pcm_cb_t cb = s_bt_audio.on_pcm;
        void *ctx = s_bt_audio.pcm_ctx;
        uint32_t rate = 0;
        uint8_t  ch   = 2;

        BT_AUDIO_LOCK();
        rate = s_bt_audio.a2dp.sample_rate;
        ch   = s_bt_audio.a2dp.channels;
        BT_AUDIO_UNLOCK();
        if (ch == 0 || ch > 2)
        {
            ch = 2;   /* AUDIO_CFG 之前按立体声（与 BT_A2DP_FRAME_BYTES 一致） */
        }

        if (cb == NULL)
        {
            continue;
        }

        /* 源不是 44.1k（Windows 笔记本常见 48k）时先重采样再交给下游。
         * 下游全部写死 44.1k，直接灌 48k 会以 8.16% 的速率盈余把 sync 的
         * 入口环灌满，之后每 15ms 丢一段样点 → 持续"吱吱"。 */
        if (rate != 0 && rate != BT_A2DP_TARGET_RATE && s_bt_audio.task_rs_buf != NULL)
        {
            size_t frames;
            size_t out_n;

            if (rate != s_bt_audio.a2dp_rs_rate)
            {
                /* 新采样率/新流：相位与历史帧清零，避免沿用上一条流的位置 */
                s_bt_audio.a2dp_rs_pos     = 0;
                s_bt_audio.a2dp_rs_last[0] = 0;
                s_bt_audio.a2dp_rs_last[1] = 0;
                s_bt_audio.a2dp_rs_rate    = rate;
                ESP_LOGI("BT_PCM", "A2DP resample %u -> %u Hz (%uch, linear)",
                         (unsigned)rate, (unsigned)BT_A2DP_TARGET_RATE, (unsigned)ch);
            }

            frames = (size_t)(bytes / ((size_t)ch * sizeof(int16_t)));
            out_n  = bt_a2dp_resample_to_44k(pcm_buf, frames, ch,
                                             s_bt_audio.task_rs_buf,
                                             BT_RS_BUF_SAMPLES / (size_t)ch);
            if (out_n > 0)
            {
                cb(BT_AUDIO_PCM_SOURCE_A2DP, s_bt_audio.task_rs_buf,
                   out_n * ch, BT_A2DP_TARGET_RATE, ctx);
            }
        }
        else
        {
            s_bt_audio.a2dp_rs_rate = rate;   /* 44.1k 原生：相位无意义 */
            cb(BT_AUDIO_PCM_SOURCE_A2DP, pcm_buf, bytes / sizeof(int16_t), rate, ctx);
        }
    }

    /* 不可达（若真退出，先退订看门狗，避免 TWDT 误报"任务不存在"） */
    (void)esp_task_wdt_delete(NULL);
    vTaskDelete(NULL);
}

/* ======================== 对外 API =========================================== */

bt_audio_handle_t bt_audio_create(const bt_audio_cfg_t *cfg)
{
    if (s_bt_audio.task != NULL)
    {
        BT_AUDIO_LOGW("bt_audio already created");
        return &s_bt_audio;
    }

    memset(&s_bt_audio, 0, sizeof(s_bt_audio));

    if (cfg != NULL)
    {
        s_bt_audio.on_pcm    = cfg->on_pcm;
        s_bt_audio.pcm_ctx   = cfg->pcm_ctx;
        s_bt_audio.on_event  = cfg->on_event;
        s_bt_audio.event_ctx = cfg->event_ctx;

        if (cfg->device_name != NULL)
        {
            strncpy(s_bt_audio.device_name, cfg->device_name, sizeof(s_bt_audio.device_name) - 1);
            s_bt_audio.device_name[sizeof(s_bt_audio.device_name) - 1] = '\0';
        }
        s_bt_audio.a2dp_fifo_bytes = (cfg->a2dp_fifo_bytes > 0) ? cfg->a2dp_fifo_bytes
                                                                : BT_A2DP_FIFO_DEFAULT_BYTES;
        s_bt_audio.hfp_up_fifo_bytes = (cfg->hfp_fifo_bytes > 0) ? cfg->hfp_fifo_bytes
                                       : BT_HFP_UP_FIFO_DEFAULT_BYTES;
    }
    else
    {
        s_bt_audio.a2dp_fifo_bytes = BT_A2DP_FIFO_DEFAULT_BYTES;
        s_bt_audio.hfp_up_fifo_bytes = BT_HFP_UP_FIFO_DEFAULT_BYTES;
    }

    if (s_bt_audio.device_name[0] == '\0')
    {
        strncpy(s_bt_audio.device_name, "MPS-Sync-Speaker", sizeof(s_bt_audio.device_name) - 1);
        s_bt_audio.device_name[sizeof(s_bt_audio.device_name) - 1] = '\0';
    }

    s_bt_audio.lock = xSemaphoreCreateMutex();
    if (s_bt_audio.lock == NULL)
    {
        BT_AUDIO_LOGE("create mutex failed");
        return NULL;
    }
    s_bt_audio.hfp_audio_sem = xSemaphoreCreateBinary();
    if (s_bt_audio.hfp_audio_sem == NULL)
    {
        vSemaphoreDelete(s_bt_audio.lock);
        s_bt_audio.lock = NULL;
        BT_AUDIO_LOGE("create hfp audio sem failed");
        return NULL;
    }


    s_bt_audio.volume = 80;

    BT_AUDIO_LOGI("bt_audio created, device=\"%s\"", s_bt_audio.device_name);
    return &s_bt_audio;
}

esp_err_t bt_audio_destroy(bt_audio_handle_t audio)
{
    (void)audio;

    if (s_bt_audio.task != NULL)
    {
        BT_AUDIO_LOGW("destroy: call bt_audio_stop first");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_bt_audio.pcm_fifo != NULL)
    {
        vStreamBufferDelete(s_bt_audio.pcm_fifo);
        s_bt_audio.pcm_fifo = NULL;
    }
    if (s_bt_audio.pcm_fifo_buf != NULL)
    {
        heap_caps_free(s_bt_audio.pcm_fifo_buf);
        s_bt_audio.pcm_fifo_buf = NULL;
    }
    if (s_bt_audio.evt_q != NULL)
    {
        vQueueDelete(s_bt_audio.evt_q);
        s_bt_audio.evt_q = NULL;
    }
    if (s_bt_audio.hfp_up_fifo != NULL)
    {
        vStreamBufferDelete(s_bt_audio.hfp_up_fifo);
        s_bt_audio.hfp_up_fifo = NULL;
    }
    if (s_bt_audio.hfp_up_fifo_buf != NULL)
    {
        heap_caps_free(s_bt_audio.hfp_up_fifo_buf);
        s_bt_audio.hfp_up_fifo_buf = NULL;
    }
    if (s_bt_audio.hfp_dn_fifo != NULL)
    {
        vStreamBufferDelete(s_bt_audio.hfp_dn_fifo);
        s_bt_audio.hfp_dn_fifo = NULL;
    }
    if (s_bt_audio.hfp_dn_fifo_buf != NULL)
    {
        heap_caps_free(s_bt_audio.hfp_dn_fifo_buf);
        s_bt_audio.hfp_dn_fifo_buf = NULL;
    }
    if (s_bt_audio.task_pcm_buf != NULL)
    {
        heap_caps_free(s_bt_audio.task_pcm_buf);
        s_bt_audio.task_pcm_buf = NULL;
    }
    if (s_bt_audio.task_rs_buf != NULL)
    {
        heap_caps_free(s_bt_audio.task_rs_buf);
        s_bt_audio.task_rs_buf = NULL;
    }
    if (s_bt_audio.task_hfp_dn_raw != NULL)
    {
        heap_caps_free(s_bt_audio.task_hfp_dn_raw);
        s_bt_audio.task_hfp_dn_raw = NULL;
    }
    if (s_bt_audio.evt_q_storage != NULL)
    {
        heap_caps_free(s_bt_audio.evt_q_storage);
        s_bt_audio.evt_q_storage = NULL;
    }

    if (s_bt_audio.lock != NULL)
    {
        vSemaphoreDelete(s_bt_audio.lock);
        s_bt_audio.lock = NULL;
    }
    if (s_bt_audio.hfp_audio_sem != NULL)
    {
        vSemaphoreDelete(s_bt_audio.hfp_audio_sem);
        s_bt_audio.hfp_audio_sem = NULL;
    }

    memset(&s_bt_audio, 0, sizeof(s_bt_audio));
    BT_AUDIO_LOGI("bt_audio destroyed");
    return ESP_OK;
}

/** 初始化蓝牙控制器 + Bluedroid + A2DP（只执行一次，stop 后保留） */
/** 临时诊断：打印内部/PSRAM 堆占用（定位 WiFi+BT 初始化各阶段内存消耗，联调完删除） */
static void bt_audio_heap_dump(const char *stage)
{
    BT_AUDIO_LOGI("[heap] %s: int free=%u largest=%u, psram free=%u",
             stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static esp_err_t bt_stack_init(void)
{
    esp_err_t err;

    bt_audio_heap_dump("stack init enter");

    /* NVS：蓝牙配对/绑定表 */
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK)
    {
        BT_AUDIO_LOGE("nvs init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 控制器：BTDM 无需释放 BLE 内存 */
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK)
    {
        BT_AUDIO_LOGE("controller init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_bt_controller_enable(BT_AUDIO_CTRL_MODE);
    if (err != ESP_OK)
    {
        BT_AUDIO_LOGE("controller enable failed: %s", esp_err_to_name(err));
        return err;
    }
    bt_audio_heap_dump("after controller enable");

    /* Bluedroid 协议栈 */
    err = esp_bluedroid_init();
    if (err != ESP_OK)
    {
        BT_AUDIO_LOGE("bluedroid init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_bluedroid_enable();
    if (err != ESP_OK)
    {
        BT_AUDIO_LOGE("bluedroid enable failed: %s", esp_err_to_name(err));
        return err;
    }
    bt_audio_heap_dump("after bluedroid enable");

    /* AVRCP：音箱需同时注册 TG（接受手机发起的连接/音量命令）与 CT（UI 主动控制手机） */
    /* 注意：AVRCP 必须先于 A2DP 初始化（协议栈要求 AVRC 预先注册）。 */
    err = esp_avrc_ct_init();
    if (err != ESP_OK)
    {
        BT_AUDIO_LOGE("avrc ct init failed: %s", esp_err_to_name(err));
        return err;
    }
    esp_avrc_ct_register_callback(bt_avrc_cb);
    err = esp_avrc_tg_init();
    if (err != ESP_OK)
    {
        BT_AUDIO_LOGE("avrc tg init failed: %s", esp_err_to_name(err));
        return err;
    }
    esp_avrc_tg_register_callback(bt_avrc_tg_cb);

    /* A2DP Sink + 状态/数据回调 + GAP 配对回调 */
    err = esp_a2d_sink_init();
    if (err != ESP_OK)
    {
        BT_AUDIO_LOGE("a2dp sink init failed: %s", esp_err_to_name(err));
        return err;
    }
    esp_a2d_register_callback(bt_a2dp_cb);
    /* 数据回调为 legacy API：协议栈解体 SBC 后输出 int16 PCM（与 bt_test.c 一致已验证） */
    esp_a2d_sink_register_data_callback(bt_a2dp_data_cb);

    /* HFP(HF)：SLC 连接与 SCO 音频回调 */
    err = esp_hf_client_init();
    if (err != ESP_OK)
    {
        BT_AUDIO_LOGE("hfp client init failed: %s", esp_err_to_name(err));
        return err;
    }
    esp_hf_client_register_callback(bt_hfp_cb);
    esp_bt_gap_register_callback(bt_gap_cb);
    esp_bt_gap_set_device_name(s_bt_audio.device_name);
    bt_audio_heap_dump("after profile init");
    return ESP_OK;
}

esp_err_t bt_audio_start(bt_audio_handle_t audio)
{
    esp_err_t err;

    (void)audio;

    if (s_bt_audio.lock == NULL)
    {
        return ESP_ERR_INVALID_STATE;   /* 未 create */
    }
    if (s_bt_audio.task != NULL)
    {
        return ESP_OK;                  /* 已启动 */
    }

    bt_audio_heap_dump("bt_audio_start enter");

    if (!s_stack_ready)
    {
        err = bt_stack_init();
        if (err != ESP_OK)
        {
            return err;
        }
        s_stack_ready = true;
    }
    else
    {
        /* 栈还在：重新设置设备名即可 */
        esp_bt_gap_set_device_name(s_bt_audio.device_name);
    }

    /* 内部存储：优先复用（stop 后 restart 场景），否则新建（大数据缓冲放 PSRAM） */
    if (s_bt_audio.pcm_fifo == NULL)
    {
        s_bt_audio.pcm_fifo = bt_audio_sbuf_create(s_bt_audio.a2dp_fifo_bytes,
                                                   &s_bt_audio.pcm_fifo_ctrl,
                                                   &s_bt_audio.pcm_fifo_buf);
        if (s_bt_audio.pcm_fifo == NULL)
        {
            BT_AUDIO_LOGE("pcm fifo create failed");
            return ESP_ERR_NO_MEM;
        }
    }
    else
    {
        xStreamBufferReset(s_bt_audio.pcm_fifo);
    }

    if (s_bt_audio.evt_q == NULL)
    {
        /* 队列数据缓冲放 PSRAM（4 条 ≈2.1KB，省内部 RAM），控制块留内部 */
        if (s_bt_audio.evt_q_storage == NULL)
        {
            s_bt_audio.evt_q_storage = (uint8_t *)heap_caps_malloc(
                BT_EVENT_QUEUE_LEN * sizeof(bt_audio_event_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (s_bt_audio.evt_q_storage == NULL)
            {
                s_bt_audio.evt_q_storage = (uint8_t *)heap_caps_malloc(
                    BT_EVENT_QUEUE_LEN * sizeof(bt_audio_event_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            }
            if (s_bt_audio.evt_q_storage == NULL)
            {
                vStreamBufferDelete(s_bt_audio.pcm_fifo);
                s_bt_audio.pcm_fifo = NULL;
                BT_AUDIO_LOGE("evt queue storage alloc failed");
                return ESP_ERR_NO_MEM;
            }
        }
        s_bt_audio.evt_q = xQueueCreateStatic(BT_EVENT_QUEUE_LEN, sizeof(bt_audio_event_t),
                                              s_bt_audio.evt_q_storage, &s_bt_audio.evt_q_ctrl);
        if (s_bt_audio.evt_q == NULL)
        {
            vStreamBufferDelete(s_bt_audio.pcm_fifo);
            s_bt_audio.pcm_fifo = NULL;
            BT_AUDIO_LOGE("evt queue create failed");
            return ESP_ERR_NO_MEM;
        }
    }
    else
    {
        xQueueReset(s_bt_audio.evt_q);
    }


    /* HFP 上行/下行 FIFO（协议栈回调入队，模块任务出队/转换） */
    if (s_bt_audio.hfp_up_fifo == NULL)
    {
        s_bt_audio.hfp_up_fifo = bt_audio_sbuf_create(s_bt_audio.hfp_up_fifo_bytes,
                                                      &s_bt_audio.hfp_up_fifo_ctrl,
                                                      &s_bt_audio.hfp_up_fifo_buf);
        if (s_bt_audio.hfp_up_fifo == NULL)
        {
            BT_AUDIO_LOGE("hfp up fifo create failed");
            return ESP_ERR_NO_MEM;
        }
    }
    else
    {
        xStreamBufferReset(s_bt_audio.hfp_up_fifo);
    }

    if (s_bt_audio.hfp_dn_fifo == NULL)
    {
        s_bt_audio.hfp_dn_fifo = bt_audio_sbuf_create(BT_HFP_DN_FIFO_DEFAULT_BYTES,
                                                      &s_bt_audio.hfp_dn_fifo_ctrl,
                                                      &s_bt_audio.hfp_dn_fifo_buf);
        if (s_bt_audio.hfp_dn_fifo == NULL)
        {
            BT_AUDIO_LOGE("hfp dn fifo create failed");
            return ESP_ERR_NO_MEM;
        }
    }
    else
    {
        xStreamBufferReset(s_bt_audio.hfp_dn_fifo);
    }
    /* 任务缓冲：A2DP PCM 块 / HFP 下行块放 PSRAM（省内部 RAM；常规 CPU 读写） */
    if (s_bt_audio.task_pcm_buf == NULL)
    {
        s_bt_audio.task_pcm_buf = (int16_t *)heap_caps_malloc(BT_PCM_CHUNK_SAMPLES * sizeof(int16_t),
                                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_bt_audio.task_pcm_buf == NULL)
        {
            s_bt_audio.task_pcm_buf = (int16_t *)heap_caps_malloc(BT_PCM_CHUNK_SAMPLES * sizeof(int16_t),
                                                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
        if (s_bt_audio.task_pcm_buf == NULL)
        {
            BT_AUDIO_LOGE("task pcm buf alloc failed");
            return ESP_ERR_NO_MEM;
        }
    }
    /* 重采样输出缓冲：失败也不阻止启动（退化为"不做采样率适配"的原行为），
     * 因为这是音质/兼容性问题，不该让整条蓝牙链路起不来 */
    if (s_bt_audio.task_rs_buf == NULL)
    {
        s_bt_audio.task_rs_buf = (int16_t *)heap_caps_malloc(BT_RS_BUF_SAMPLES * sizeof(int16_t),
                                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_bt_audio.task_rs_buf == NULL)
        {
            s_bt_audio.task_rs_buf = (int16_t *)heap_caps_malloc(BT_RS_BUF_SAMPLES * sizeof(int16_t),
                                                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
        if (s_bt_audio.task_rs_buf == NULL)
        {
            BT_AUDIO_LOGW("resample buf alloc failed, non-44.1k source unsupported");
        }
    }
    if (s_bt_audio.task_hfp_dn_raw == NULL)
    {
        s_bt_audio.task_hfp_dn_raw = (int16_t *)heap_caps_malloc(BT_HFP_DN_CHUNK_BYTES,
                                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_bt_audio.task_hfp_dn_raw == NULL)
        {
            s_bt_audio.task_hfp_dn_raw = (int16_t *)heap_caps_malloc(BT_HFP_DN_CHUNK_BYTES,
                                                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
        if (s_bt_audio.task_hfp_dn_raw == NULL)
        {
            BT_AUDIO_LOGE("task hfp dn buf alloc failed");
            return ESP_ERR_NO_MEM;
        }
    }

    bt_audio_heap_dump("before task create");
    BaseType_t rt = xTaskCreatePinnedToCore(bt_audio_task, "bt_audio_task",
                                            BT_AUDIO_TASK_STACK, NULL,
                                            BT_AUDIO_TASK_PRIO, &s_bt_audio.task,
                                            BT_AUDIO_TASK_CORE);
    if (rt != pdPASS)
    {
        vStreamBufferDelete(s_bt_audio.pcm_fifo);
        vQueueDelete(s_bt_audio.evt_q);
        s_bt_audio.pcm_fifo = NULL;
        s_bt_audio.evt_q = NULL;
        if (s_bt_audio.pcm_fifo_buf != NULL) { heap_caps_free(s_bt_audio.pcm_fifo_buf); s_bt_audio.pcm_fifo_buf = NULL; }
        if (s_bt_audio.hfp_up_fifo != NULL) { vStreamBufferDelete(s_bt_audio.hfp_up_fifo); s_bt_audio.hfp_up_fifo = NULL; }
        if (s_bt_audio.hfp_up_fifo_buf != NULL) { heap_caps_free(s_bt_audio.hfp_up_fifo_buf); s_bt_audio.hfp_up_fifo_buf = NULL; }
        if (s_bt_audio.hfp_dn_fifo != NULL) { vStreamBufferDelete(s_bt_audio.hfp_dn_fifo); s_bt_audio.hfp_dn_fifo = NULL; }
        if (s_bt_audio.hfp_dn_fifo_buf != NULL) { heap_caps_free(s_bt_audio.hfp_dn_fifo_buf); s_bt_audio.hfp_dn_fifo_buf = NULL; }
        BT_AUDIO_LOGE("task create failed: internal free=%u largest=%u",
               heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
               heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        return ESP_ERR_NO_MEM;
    }

    /* 可被发现 / 可连接 */
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    BT_AUDIO_LOGI("bt_audio started, discoverable ON");
    return ESP_OK;
}

esp_err_t bt_audio_stop(bt_audio_handle_t audio)
{
    (void)audio;

    if (s_bt_audio.task == NULL)
    {
        return ESP_OK;
    }

    /* 停止广播与断开连接（保留蓝牙栈，便于再次 start） */
    esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);

    BT_AUDIO_LOCK();
    bool connected = s_bt_audio.a2dp.connected;
    uint8_t remote_addr[6];
    memcpy(remote_addr, s_bt_audio.remote_addr, 6);
    BT_AUDIO_UNLOCK();

    if (connected)
    {
        esp_a2d_sink_disconnect(remote_addr);   /* 异步断开，不等待 */
    }

    BT_AUDIO_LOCK();
    bool hfp_connected = s_bt_audio.hfp_connected;
    uint8_t hfp_addr[6];
    memcpy(hfp_addr, s_bt_audio.hfp_remote_addr, 6);
    BT_AUDIO_UNLOCK();

    if (hfp_connected)
    {
        esp_hf_client_disconnect(hfp_addr);   /* 异步断开 SLC（含 SCO） */
    }

    vTaskDelete(s_bt_audio.task);
    s_bt_audio.task = NULL;

    BT_AUDIO_LOGI("bt_audio stopped");
    return ESP_OK;
}

esp_err_t bt_audio_set_device_name(bt_audio_handle_t audio, const char *name)
{
    (void)audio;

    if (name == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(s_bt_audio.device_name, name, sizeof(s_bt_audio.device_name) - 1);
    s_bt_audio.device_name[sizeof(s_bt_audio.device_name) - 1] = '\0';

    if (s_stack_ready)
    {
        esp_bt_gap_set_device_name(s_bt_audio.device_name);
    }
    return ESP_OK;
}

esp_err_t bt_audio_set_discoverable(bt_audio_handle_t audio, bool enable)
{
    (void)audio;

    if (s_bt_audio.task == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (enable)
    {
        esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    }
    else
    {
        /* 保持可连接但不可被发现 */
        esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
    }
    return ESP_OK;
}

esp_err_t bt_audio_get_a2dp_info(bt_audio_handle_t audio, bt_audio_a2dp_info_t *info)
{
    (void)audio;

    if (info == NULL || s_bt_audio.lock == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    BT_AUDIO_LOCK();
    *info = s_bt_audio.a2dp;
    BT_AUDIO_UNLOCK();
    return ESP_OK;
}

/* ======================== HFP 语音链路 ======================================== */

esp_err_t bt_audio_hfp_start_voice(bt_audio_handle_t audio)
{
    esp_err_t err;

    (void)audio;

    if (s_bt_audio.lock == NULL || s_bt_audio.task == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_bt_audio.hfp_connected)
    {
        BT_AUDIO_LOGW("hfp_start_voice: HFP SLC not connected");
        return ESP_ERR_INVALID_STATE;
    }

    /* 向手机（AG）发送 AT+BVRA=1：启动其语音识别（唤醒语音助手） */
    err = esp_hf_client_start_voice_recognition();
    if (err != ESP_OK)
    {
        BT_AUDIO_LOGW("start voice recognition failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t bt_audio_hfp_stop_voice(bt_audio_handle_t audio)
{
    esp_err_t err = ESP_OK;

    (void)audio;

    if (s_bt_audio.lock == NULL || s_bt_audio.task == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_bt_audio.hfp_connected)
    {
        return ESP_OK;   /* 未连接，无需停止 */
    }

    err = esp_hf_client_stop_voice_recognition();
    if (err != ESP_OK)
    {
        BT_AUDIO_LOGW("stop voice recognition failed: %s", esp_err_to_name(err));
    }

    /* 手机助手结束对话后通常自动断开 SCO；这里再主动请求一次以确保释放 */
    esp_hf_client_disconnect_audio(s_bt_audio.hfp_remote_addr);
    return err;
}

/* 上行 PCM 说明：samples 为单声道 16bit 采样数，采样率固定 44.1kHz，
 * 由 bt_audio 内部降采样到 SCO 速率（mSBC 16k / CVSD 8k）。 */
esp_err_t bt_audio_hfp_send_pcm(bt_audio_handle_t audio, const int16_t *pcm,
                                size_t samples, uint32_t timeout_ms)
{
    size_t bytes, sent;

    (void)audio;

    if (pcm == NULL || samples == 0 || s_bt_audio.hfp_up_fifo == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    bytes = samples * sizeof(int16_t);
    sent  = xStreamBufferSend(s_bt_audio.hfp_up_fifo, pcm, bytes, pdMS_TO_TICKS(timeout_ms));
    if (sent != bytes)
    {
        return ESP_ERR_TIMEOUT;   /* FIFO 满：上层应丢帧，SCO 侧由静音填充兜底 */
    }
    return ESP_OK;
}

esp_err_t bt_audio_hfp_wait_audio_open(bt_audio_handle_t audio, uint32_t timeout_ms)
{
    (void)audio;

    if (s_bt_audio.hfp_audio_sem == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_bt_audio.hfp_audio_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE)
    {
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

bool bt_audio_hfp_is_audio_open(bt_audio_handle_t audio)
{
    (void)audio;
    return s_bt_audio.hfp_audio_open;
}


/* ======================== AVRCP（UI → 手机控制） ============================== */

esp_err_t bt_audio_send_ctrl_cmd(bt_audio_handle_t audio, bt_audio_cmd_t cmd)
{
    uint8_t key = 0;
    esp_err_t err;

    (void)audio;

    if (!s_bt_audio.avrc_connected)
    {
        return ESP_ERR_INVALID_STATE;   /* AVRCP 未连接，手机控制不可用 */
    }

    switch (cmd)
    {
    case BT_AUDIO_CMD_PLAY:         key = ESP_AVRC_PT_CMD_PLAY;     break;
    case BT_AUDIO_CMD_PAUSE:        key = ESP_AVRC_PT_CMD_PAUSE;    break;
    case BT_AUDIO_CMD_PREV:         key = ESP_AVRC_PT_CMD_BACKWARD; break;
    case BT_AUDIO_CMD_NEXT:         key = ESP_AVRC_PT_CMD_FORWARD;  break;
    case BT_AUDIO_CMD_VOLUME_UP:    key = ESP_AVRC_PT_CMD_VOL_UP;   break;
    case BT_AUDIO_CMD_VOLUME_DOWN:  key = ESP_AVRC_PT_CMD_VOL_DOWN; break;
    case BT_AUDIO_CMD_TOGGLE_PLAY:
        key = (s_bt_audio.play_state == BT_AUDIO_PLAY_STATE_PLAYING)
              ? ESP_AVRC_PT_CMD_PAUSE : ESP_AVRC_PT_CMD_PLAY;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    /* 按下 + 释放两个透传记录 */
    err = esp_avrc_ct_send_passthrough_cmd(bt_audio_avrc_alloc_tl(), key,
                                           ESP_AVRC_PT_CMD_STATE_PRESSED);
    if (err != ESP_OK)
    {
        return err;
    }
    return esp_avrc_ct_send_passthrough_cmd(bt_audio_avrc_alloc_tl(), key,
                                            ESP_AVRC_PT_CMD_STATE_RELEASED);
}

esp_err_t bt_audio_set_repeat_mode(bt_audio_handle_t audio, bool single)
{
    esp_err_t err;

    (void)audio;

    if (!s_bt_audio.avrc_connected)
    {
        return ESP_ERR_INVALID_STATE;   /* AVRCP 未连接，无法向手机下发 */
    }

    /* 值使用 AVRCP 规范字面量：0x00=OFF / 0x01=SINGLE / 0x02=ALL。
     * 不可改用 esp_avrc_ps_rpt_value_ids_t：IDF 该枚举从 0x1 起算
     * （OFF=1/SINGLE=2/GROUP=3），而 bluedroid 在 avrc_bld_ct.c 里把
     * attr_val 原样打包、不做重映射，按枚举名发 SINGLE(2) 实际发出去的
     * 是规范的 ALL TRACK REPEAT（全列表循环），现象就是"设了等于没设"。 */
    err = esp_avrc_ct_send_set_player_value_cmd(bt_audio_avrc_alloc_tl(),
                                                ESP_AVRC_PS_REPEAT_MODE,
                                                single ? 0x01u : 0x00u);
    BT_AUDIO_LOGI("AVRCP repeat -> %s: %s",
                  single ? "SINGLE(0x01)" : "OFF(0x00)",
                  esp_err_to_name(err));
    return err;
}

esp_err_t bt_audio_get_play_state(bt_audio_handle_t audio, bt_audio_play_state_t *state)
{
    (void)audio;

    if (state == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    BT_AUDIO_LOCK();
    if (s_bt_audio.avrc_connected)
    {
        /* AVRCP 已连接：用手机上报的真实状态 */
        *state = s_bt_audio.play_state;
    }
    else
    {
        /* 未连接或断开瞬间：按 A2DP 传输状态兜底推断 */
        *state = s_bt_audio.stream_started ? BT_AUDIO_PLAY_STATE_PLAYING
               : s_bt_audio.a2dp.connected ? BT_AUDIO_PLAY_STATE_PAUSED
               : BT_AUDIO_PLAY_STATE_STOPPED;
    }
    BT_AUDIO_UNLOCK();
    return ESP_OK;
}
esp_err_t bt_audio_get_position_ms(bt_audio_handle_t audio, uint32_t *position_ms)
{
    (void)audio;

    if (position_ms == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    BT_AUDIO_LOCK();
    if (!s_bt_audio.play_pos_valid)
    {
        BT_AUDIO_UNLOCK();
        return ESP_ERR_NOT_FOUND;   /* 手机未上报过位置（不支持 PLAY_POS_CHANGED / 未播放） */
    }
    *position_ms = s_bt_audio.play_pos_ms;
    BT_AUDIO_UNLOCK();
    return ESP_OK;
}

esp_err_t bt_audio_get_track_info(bt_audio_handle_t audio,
                                  const bt_audio_track_info_t **info)
{
    (void)audio;

    if (info == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    BT_AUDIO_LOCK();
    if (!s_bt_audio.track_info_valid)
    {
        BT_AUDIO_UNLOCK();
        return ESP_ERR_NOT_FOUND;   /* 手机尚未上报过曲目元数据 */
    }
    *info = &s_bt_audio.track_info;   /* 返回缓存指针（只读） */
    BT_AUDIO_UNLOCK();
    return ESP_OK;
}

/* ======================== 音量（本地缓存 + AVRCP 绝对音量） =================== */

esp_err_t bt_audio_set_volume(bt_audio_handle_t audio, uint8_t volume)
{
    bool avrc;

    (void)audio;
    if (volume > 100)
    {
        volume = 100;
    }

    BT_AUDIO_LOCK();
    s_bt_audio.volume = volume;
    avrc = s_bt_audio.avrc_connected;
    BT_AUDIO_UNLOCK();

    /* 手机已连接：将音量同步为 AVRCP SetAbsoluteVolume（0..127） */
    if (avrc)
    {
        esp_err_t err = esp_avrc_ct_send_set_absolute_volume_cmd(
            bt_audio_avrc_alloc_tl(), BT_VOL_LOCAL_TO_AVRC(volume));
        if (err != ESP_OK)
        {
            BT_AUDIO_LOGW("send abs volume failed: %s", esp_err_to_name(err));
        }
    }
    return ESP_OK;
}

uint8_t bt_audio_get_volume(bt_audio_handle_t audio)
{
    (void)audio;
    return s_bt_audio.volume;
}

/* ======================== 调试 ================================================ */

uint32_t bt_audio_get_pcm_dropped(bt_audio_handle_t audio, bt_audio_pcm_source_t source)
{
    (void)audio;
    if (source == BT_AUDIO_PCM_SOURCE_A2DP)
    {
        return s_bt_audio.dropped_pcm;
    }
    if (source == BT_AUDIO_PCM_SOURCE_HFP_DOWNLINK)
    {
        return s_bt_audio.hfp_dn_dropped;
    }
    return 0;
}

uint32_t bt_audio_get_pcm_resync(bt_audio_handle_t audio)
{
    (void)audio;
    return s_bt_audio.resync_cnt;
}

uint32_t bt_audio_get_a2dp_underrun(bt_audio_handle_t audio)
{
    (void)audio;
    return s_bt_audio.a2dp_underrun_cnt;
}

uint32_t bt_audio_get_a2dp_idle(bt_audio_handle_t audio)
{
    (void)audio;
    return s_bt_audio.a2dp_idle_cnt;
}

uint32_t bt_audio_get_and_reset_a2dp_gap(bt_audio_handle_t audio)
{
    uint32_t gap;
    (void)audio;
    gap = s_bt_audio.a2dp_gap_max_ms;
    s_bt_audio.a2dp_gap_max_ms = 0;
    return gap;
}

uint32_t bt_audio_get_a2dp_cb_count(bt_audio_handle_t audio)
{
    (void)audio;
    return s_bt_audio.a2dp_cb_cnt;
}

uint32_t bt_audio_get_task_beat(bt_audio_handle_t audio)
{
    (void)audio;
    return s_bt_audio.task_beat;
}

uint32_t bt_audio_get_a2dp_fifo_used(bt_audio_handle_t audio)
{
    (void)audio;
    if (s_bt_audio.pcm_fifo == NULL)
    {
        return 0;
    }
    return (uint32_t)xStreamBufferBytesAvailable(s_bt_audio.pcm_fifo);
}
