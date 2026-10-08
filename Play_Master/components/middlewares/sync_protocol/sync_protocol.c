/**
 * @file sync_protocol.c
 * @brief 音频同步协议模块实现（主节点侧）：PCM 汇聚 → 分包 → 组播发送 +
 *        本地延迟（D）播放 + 控制/时间同步通道处理
 *
 * 实现要点（对应《主音频节点软件架构分层设计.md》3.2.8 / 第 6 节任务表）：
 *   - SyncTx_Task：优先级 11 / 栈 8192 / Core 0。轮询入口环攒够一帧（约 15ms），
 *     打 esp_timer 时间戳 → 组播发送 → 推入本地延迟队列；同时处理控制通道
 *     （PING → PONG 应答、SYNC_START 一次下发、SYNC_HEARTBEAT 每秒下发、
 *      从节点在线超时检测）；本模块不持有 UDP socket（wifi_manager 通道句柄传入）。
 *   - SyncLocalPlay_Task：优先级 10 / 栈 10240 / Core 1。按 t_play = t_tx + D
 *     出队，单声道 → 立体声（L/R 写入同一份）后经 amplifier 写 I2S。
 *   - 帧节奏：每帧 15ms，帧大小在 661/662 采样之间交替，保证平均 44.1kHz
 *     （44.1k×0.015=661.5 非整数）；
 *   - 独立于音源：push_pcm 线程安全非阻塞入环；暂停/停止时环满丢弃。
 *
 * 本地最终出声使用 Amplifier_Play_Buffer()（第九阶段讨论决定），因此
 * 本模块不注册任何写者：经 Amplifier_Play_Buffer() 直接写 I2S。
 */

#include "sync_protocol.h"

#include "amplifier.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#define TAG "SYNC_PROTOCOL"

/* ======================== 默认参数 ======================== */

/** 统一同步缓冲 D（cfg->sync_delay_ms 为 0 时使用） */
#define SYNC_CFG_DEFAULT_DELAY_MS        (200)
/** 每包 PCM 时长（cfg->frame_duration_ms 为 0 时使用） */
#define SYNC_CFG_DEFAULT_FRAME_MS        (15)
/** 本地延迟队列默认容量（须 ≥ D / 每包时长，200/15≈13.3） */
#define SYNC_CFG_DEFAULT_DELAY_PKTS      (16)
/** PCM 入口环默认字节容量 */
#define SYNC_CFG_DEFAULT_INGRESS_BYTES   (24576)

/** SyncTx_Task：优先级/栈/核（任务表） */
#define SYNC_TX_TASK_PRIO   (11)
#define SYNC_TX_TASK_STACK  (8192)
#define SYNC_TX_TASK_CORE   (0)

/** SyncLocalPlay_Task：优先级/栈/核（任务表） */
#define SYNC_PLAY_TASK_PRIO  (10)
#define SYNC_PLAY_TASK_STACK (10240)
#define SYNC_PLAY_TASK_CORE  (1)

/** 每帧最大采样数（44.1k×15ms=661.5，取 700 余量；帧长在 661/662 交替） */
#define SYNC_FRAME_MAX_SAMPLES 700

/** 入环单次搬移缓冲（字节） */
#define SYNC_IO_BUF_BYTES 2048

/** 控制通道单次接收缓冲 */
#define SYNC_CTRL_RX_BUF  256

/** SYNC_START 与心跳周期 */
#define SYNC_HEARTBEAT_PERIOD_MS 1000

/** 从节点在线判定超时（暂定为：从机启动 10 次 PING 后按 30s 周期维持） */
#define SYNC_SLAVE_ONLINE_TIMEOUT_MS 30000

/** 控制命令组播发送超时 */
#define SYNC_CTRL_SEND_TIMEOUT_MS 100

/** 本地播放写入 I2S 超时（写满待 DMA 时最长等 500ms，再长视为异常） */
#define SYNC_PLAY_WRITE_TIMEOUT_MS 500

/** 出队时间与 t_play_master 时差在此范围内即认为“准时” */
#define SYNC_PLAY_DELTA_TOLERATE_US 2000

/** 音频包组装缓冲（16B 头 + 最大帧载荷） */
#define SYNC_PKT_BUF_BYTES (SYNC_AUDIO_HDR_SIZE + SYNC_FRAME_MAX_SAMPLES * 2)

/* ======================== 内部结构 ======================== */

/** 本地延迟队列条目：一帧 mono PCM + 播放时间戳 */
typedef struct {
    uint16_t stream_id;
    uint16_t seq;
    uint64_t t_tx_us;              /* 主节点发送时刻（esp_timer） */
    uint16_t frame_samples;        /* 本帧 mono 采样数（661/662） */
    int16_t  pcm[SYNC_FRAME_MAX_SAMPLES];
} sync_delay_entry_t;

/** 模块单例 */
typedef struct {
    bool inited;
    bool started;
    bool deinit_pending;           /* deinit 通知两个内部任务退出 */
    sync_protocol_state_t state;

    uint16_t stream_id;            /* 0 = 未启动 */
    uint32_t sample_rate;
    uint8_t  channels;
    uint32_t auto_stream_seq;      /* stream_id=0 自动生成用 */

    uint32_t cfg_delay_ms;
    uint32_t cfg_frame_ms;
    uint32_t cfg_delay_pkts;
    uint32_t cfg_ingress_bytes;
    wifi_mcast_handle_t audio_chan;
    wifi_mcast_handle_t control_chan;

    /* 入口环 */
    StreamBufferHandle_t  ingress;
    StaticStreamBuffer_t  ingress_sb;
    uint8_t              *ingress_buf;   /* PSRAM 优先 */
    uint32_t              dropped_pkts;  /* 入口环满丢弃 + 发送失败累计 */

    /* 帧化 */
    int32_t  frame_filled;         /* 已凑 mono 采样数 */
    uint32_t frame_target;         /* 本帧目标 mono 采样数（661/662 交替） */
    int16_t *frame_buf;           /* 攒帧缓冲（PSRAM） */
    int16_t *play_buf;            /* 单声道→立体声拓展缓冲（PSRAM） */

    /* 本地延迟队列（SPSC，TX推 / Play拉） */
    sync_delay_entry_t *delay_q;
    uint32_t delay_cap;
    volatile uint32_t delay_head;
    volatile uint32_t delay_count;

    /* 发送节奏 */
    uint64_t next_slot_us;     /* 下一帧计划发送时刻 */
    uint64_t first_tx_us;      /* 首个音频帧的发送时刻（SYNC_START 的 t0） */
    uint16_t tx_seq;
    uint32_t tx_pkts;
    int64_t  last_played_pts_us;
    int64_t  play_delta_us;

    /* 时间同步 */
    bool     slave_online;
    uint64_t last_ping_us;
    uint64_t last_heartbeat_us;
    bool     start_sent;

    /* 事件 */
    sync_protocol_event_cb_t event_cb;
    void                    *event_ctx;

    TaskHandle_t tx_task;
    TaskHandle_t play_task;
} sync_master_t;

static sync_master_t s_master;
static SemaphoreHandle_t s_lock = NULL;   /* 保护 state/参数/队列元数据 */

/* 内部 IO 中转（避免任务栈过大） */
static uint8_t  s_io_buf[SYNC_IO_BUF_BYTES];

/* ======================== 内部工具 ======================== */

static void sync_snapshot(sync_protocol_master_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state        = s_master.state;
    out->stream_id    = s_master.stream_id;
    out->sample_rate  = s_master.sample_rate;
    out->channels     = s_master.channels;
    out->frame_seq    = s_master.tx_seq;
    out->tx_pkts      = s_master.tx_pkts;
    out->dropped_pkts = s_master.dropped_pkts;
    out->ingress_used = (s_master.ingress != NULL)
                            ? (uint32_t)xStreamBufferBytesAvailable(s_master.ingress) : 0;
    out->delay_queue_used = (s_master.delay_cap > 0)
                            ? ((uint32_t)(s_master.delay_count)) : 0;
    out->play_delta_us = s_master.play_delta_us;
    out->slave_online  = s_master.slave_online;
    out->last_slave_offset_us = 0;   /* 主节点不能单边计算 offset */
}

/* ---------------- 事件回调 ---------------- */

static void sync_emit_event(sync_protocol_event_id_t id)
{
    sync_protocol_event_t evt;

    if (s_master.event_cb == NULL)
    {
        return;
    }
    evt.id = id;
    sync_snapshot(&evt.status);
    s_master.event_cb(&evt, s_master.event_ctx);
}

/* ---------------- 延迟队列（TX 推 / Play 拉，环形） ---------------- */

static bool sync_delay_push(const sync_delay_entry_t *ent)
{
    if (s_master.delay_q == NULL ||
        s_master.delay_count >= s_master.delay_cap)
    {
        return false;   /* 队列满：调用方应丢弃该帧 */
    }
    uint32_t tail = (s_master.delay_head + s_master.delay_count) % s_master.delay_cap;
    memcpy(&s_master.delay_q[tail], ent, sizeof(*ent));
    /* SPSC：先填数据再提交 count，避免播放任务看到半成品 */
    s_master.delay_count++;
    return true;
}

static bool sync_delay_pop(sync_delay_entry_t *out)
{
    if (s_master.delay_count == 0)
    {
        return false;
    }
    memcpy(out, &s_master.delay_q[s_master.delay_head], sizeof(*out));
    s_master.delay_head = (s_master.delay_head + 1) % s_master.delay_cap;
    s_master.delay_count--;
    return true;
}

static void sync_delay_clear(void)
{
    s_master.delay_head  = 0;
    s_master.delay_count = 0;
}

/* ---------------- 组播发送 ---------------- */

/** 发送基础控制命令帧（Magic+Cmd+Param） */
static esp_err_t sync_send_ctrl_basic(sync_protocol_cmd_t cmd, uint8_t param)
{
    sync_ctrl_basic_t f;

    if (s_master.control_chan == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    memset(&f, 0, sizeof(f));
    f.magic = SYNC_MAGIC;
    f.cmd   = (uint8_t)cmd;
    f.param = param;
    return wifi_manager_mcast_send(s_master.control_chan, &f, sizeof(f),
                                   SYNC_CTRL_SEND_TIMEOUT_MS);
}

/** 发送控制扩展帧（时间同步） */
static esp_err_t sync_send_ctrl_ext(uint8_t type, const void *payload, uint8_t payload_len)
{
    uint8_t buf[SYNC_CTRL_RX_BUF];
    sync_ctrl_ext_t *ext = (sync_ctrl_ext_t *)buf;
    size_t total;

    if (s_master.control_chan == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (payload_len > sizeof(buf) - sizeof(sync_ctrl_ext_t))
    {
        return ESP_ERR_INVALID_ARG;
    }
    total = sizeof(sync_ctrl_ext_t) + payload_len;
    memset(buf, 0, total);
    ext->magic = SYNC_MAGIC;
    ext->type  = type;
    ext->len   = payload_len;
    if (payload_len > 0)
    {
        memcpy(ext->data, payload, payload_len);
    }
    return wifi_manager_mcast_send(s_master.control_chan, buf, total,
                                   SYNC_CTRL_SEND_TIMEOUT_MS);
}

/** 下发一次 SYNC_START（首帧发送成功后调用） */
static void sync_send_start(void)
{
    sync_start_payload_t pl;

    if (s_master.stream_id == 0)
    {
        return;
    }
    memset(&pl, 0, sizeof(pl));
    pl.stream_id = s_master.stream_id;
    pl.t0_us     = s_master.first_tx_us;   /* 第一帧的发送时刻 */
    pl.d_ms      = s_master.cfg_delay_ms;
    if (sync_send_ctrl_ext(SYNC_MSG_START, &pl, sizeof(pl)) != ESP_OK)
    {
        ESP_LOGW(TAG, "SYNC_START send failed");
    }
    else
    {
        ESP_LOGI(TAG, "SYNC_START sent: stream=%u t0=%lld d=%u ms",
                 pl.stream_id, (long long)pl.t0_us, pl.d_ms);
    }
}

/** 每秒下发 SYNC_HEARTBEAT（Play 侧更新 last_played_pts_us） */
static void sync_send_heartbeat(void)
{
    sync_heartbeat_payload_t pl;

    if (s_master.stream_id == 0)
    {
        return;
    }
    memset(&pl, 0, sizeof(pl));
    pl.stream_id        = s_master.stream_id;
    pl.master_pts_us    = (uint64_t)s_master.last_played_pts_us;
    pl.t_master_now_us  = (uint64_t)esp_timer_get_time();
    sync_send_ctrl_ext(SYNC_MSG_HEARTBEAT, &pl, sizeof(pl));
}

/** 控制通道接收处理：PING→PONG、从节点在线检测（在 TX 任务上下文） */
static void sync_handle_control_rx(void)
{
    uint8_t buf[SYNC_CTRL_RX_BUF];
    size_t  len = 0;
    int64_t now_us;

    if (s_master.control_chan == NULL)
    {
        return;
    }
    while (wifi_manager_mcast_recv(s_master.control_chan, buf, sizeof(buf), &len, 1) == ESP_OK)
    {
        if (len < 4)
        {
            continue;
        }
        sync_ctrl_basic_t *b = (sync_ctrl_basic_t *)buf;
        if (b->magic != SYNC_MAGIC)
        {
            continue;
        }
        if (len == sizeof(sync_ctrl_basic_t))
        {
            /* 基础控制帧：从节点通常不主动下发，忽略 */
            continue;
        }
        if (len < sizeof(sync_ctrl_ext_t))
        {
            continue;
        }
        sync_ctrl_ext_t *ext = (sync_ctrl_ext_t *)buf;
        if (ext->type == SYNC_MSG_PING && ext->len == sizeof(sync_ping_payload_t))
        {
            sync_ping_payload_t  ping;
            sync_pong_payload_t  pong;
            uint8_t  pong_pkt[SYNC_CTRL_RX_BUF];
            sync_ctrl_ext_t *out = (sync_ctrl_ext_t *)pong_pkt;

            memcpy(&ping, ext->data, sizeof(ping));
            now_us = esp_timer_get_time();
            pong.t1_us = ping.t1_us;
            pong.t2_us = (uint64_t)now_us;
            pong.t3_us = (uint64_t)esp_timer_get_time();   /* 发出时刻 */
            if (s_master.slave_online == false)
            {
                s_master.slave_online = true;
                ESP_LOGI(TAG, "slave online (first PING)");
                sync_emit_event(SYNC_EVT_SLAVE_SYNCED);
            }
            s_master.last_ping_us = (uint64_t)now_us;
            out->magic = SYNC_MAGIC;
            out->type  = SYNC_MSG_PONG;
            out->len   = sizeof(sync_pong_payload_t);
            memcpy(out->data, &pong, sizeof(pong));
            size_t total = sizeof(sync_ctrl_ext_t) + sizeof(pong);
            if (wifi_manager_mcast_send(s_master.control_chan, out, total,
                                        SYNC_CTRL_SEND_TIMEOUT_MS) != ESP_OK)
            {
                ESP_LOGW(TAG, "PONG send failed");
            }
            ESP_LOGD(TAG, "PING(t1=%" PRId64 ") -> PONG(t2=%" PRId64 ", t3=%" PRId64 ")",
                     (int64_t)ping.t1_us, (int64_t)pong.t2_us, (int64_t)pong.t3_us);
        }
    }

    /* 从节点在线超时检测（如已超过一个 PING 周期未收到） */
    if (s_master.slave_online &&
        ((uint64_t)esp_timer_get_time() - s_master.last_ping_us) >
            (uint64_t)SYNC_SLAVE_ONLINE_TIMEOUT_MS * 1000u)
    {
        s_master.slave_online = false;
        ESP_LOGW(TAG, "slave lost (no PING in %u ms)", SYNC_SLAVE_ONLINE_TIMEOUT_MS);
        sync_emit_event(SYNC_EVT_SLAVE_LOST);
    }
}

/* ======================== 任务实现 ======================== */

/**
 * @brief SyncTx_Task：攒帧 → 打时间戳 → 组播发送 → 入本地延迟队列
 *
 * 帧节奏：首帧立即发送并记录 next_slot = t_tx + 15ms；之后每帧等到
 * next_slot 才发送（esp_timer 保证节奏），帧长 661/662 采样交替取平均
 * 44.1kHz。入环数据不足时按静音帧缺帧处理（丢弃，不补静音占位——
 * 从节点对缺包自行补静音）。
 */
static void sync_tx_task(void *arg)
{
    (void)arg;

    for (;;)
    {
        /* 控制通道轮询（PING→PONG、在线检测） */
        sync_handle_control_rx();

        if (s_master.deinit_pending)
        {
            break;
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool active = s_master.started &&
                      (s_master.state == SYNC_STATE_PLAYING ||
                       s_master.state == SYNC_STATE_PREPARING);
        uint32_t nxt_frame = s_master.frame_target; /* 本帧目标采样数 */
        xSemaphoreGive(s_lock);

        if (!active)
        {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        /* ---- 从入口环攒够一帧 mono PCM ---- */
        while (s_master.frame_filled < (int32_t)nxt_frame)
        {
            size_t want = (size_t)(nxt_frame - s_master.frame_filled) *
                          s_master.channels * 2;
            if (want > SYNC_IO_BUF_BYTES)
            {
                want = SYNC_IO_BUF_BYTES;
            }
            size_t got = xStreamBufferReceive(s_master.ingress, s_io_buf,
                                              want, 0);
            if (got == 0)
            {
                break;
            }
            size_t frames = got / (s_master.channels * 2);
            const int16_t *src = (const int16_t *)s_io_buf;
            int16_t *dst = &s_master.frame_buf[s_master.frame_filled];
            if (s_master.channels == 2)
            {
                for (size_t i = 0; i < frames; i++)
                {
                    dst[i] = (int16_t)(((int32_t)src[2*i] + src[2*i+1]) >> 1);
                }
            }
            else
            {
                memcpy(dst, src, frames * sizeof(int16_t));
            }
            s_master.frame_filled += (int32_t)frames;
        }

        /* ---- 帧满且到点时：发送 + 入本地延迟队列 ---- */
        if (s_master.frame_filled >= (int32_t)nxt_frame)
        {
            int64_t now_us = esp_timer_get_time();
            if (s_master.next_slot_us == 0)
            {
                s_master.next_slot_us = (uint64_t)now_us;   /* 首帧立即发 */
            }
            if (now_us >= (int64_t)s_master.next_slot_us)
            {
                sync_delay_entry_t ent;
                int64_t t_tx = esp_timer_get_time();

                memset(&ent, 0, sizeof(ent));
                ent.stream_id    = s_master.stream_id;
                ent.seq          = s_master.tx_seq;
                ent.t_tx_us      = (uint64_t)t_tx;
                ent.frame_samples= (uint16_t)nxt_frame;
                memcpy(ent.pcm, s_master.frame_buf,
                       nxt_frame * sizeof(int16_t));

                /* 组播发送（音频通道） */
                uint8_t pkt[SYNC_PKT_BUF_BYTES];
                sync_audio_pkt_hdr_t *hdr = (sync_audio_pkt_hdr_t *)pkt;
                size_t pkt_len;
                if (sizeof(pkt) < SYNC_AUDIO_HDR_SIZE +
                    nxt_frame * sizeof(int16_t))
                {
                    ESP_LOGE(TAG, "packet too large, drop");
                    s_master.dropped_pkts++;
                    s_master.frame_filled = 0;
                    continue;   /* 不应发生 */
                }
                memset(pkt, 0, sizeof(pkt));
                hdr->magic        = SYNC_MAGIC;
                hdr->stream_id    = s_master.stream_id;
                hdr->seq          = s_master.tx_seq;
                hdr->timestamp_us = (uint64_t)t_tx;
                hdr->data_len     = (uint16_t)(nxt_frame * sizeof(int16_t));
                memcpy(pkt + SYNC_AUDIO_HDR_SIZE, s_master.frame_buf,
                       nxt_frame * sizeof(int16_t));
                pkt_len = SYNC_AUDIO_HDR_SIZE + nxt_frame * sizeof(int16_t);
                if (wifi_manager_mcast_send(s_master.audio_chan, pkt, pkt_len, 20) != ESP_OK)
                {
                    s_master.dropped_pkts++;
                }

                /* 入本地延迟队列 */
                if (sync_delay_push(&ent) == false)
                {
                    s_master.dropped_pkts++;
                }

                s_master.tx_pkts++;
                if (s_master.start_sent == false)
                {
                    s_master.start_sent = true;
                    s_master.first_tx_us = (uint64_t)t_tx;
                    sync_send_start();
                }
                s_master.frame_filled = 0;
                s_master.tx_seq++;
                s_master.frame_target = (s_master.tx_seq & 1u) ? 662u : 661u;
                s_master.next_slot_us += (uint64_t)(s_master.cfg_frame_ms * 1000u);
            }
            else
            {
                /* 未到发送时刻：睡一小段再等（保持 15ms 节奏） */
                int64_t wait_ms = ((int64_t)s_master.next_slot_us - now_us) / 1000;
                vTaskDelay(pdMS_TO_TICKS(wait_ms > 5 ? wait_ms : 1));
            }
        }
        else
        {
            /* 数据不足：如缺口超 1 帧时长则记录一次欠载（不阻塞上游） */
            vTaskDelay(pdMS_TO_TICKS(1));
        }

        /* ---- 心跳：每秒下发（从节点靠 master_pts 做漂移校正） ---- */
        if (s_master.started && s_master.state == SYNC_STATE_PLAYING)
        {
            int64_t now = esp_timer_get_time();
            if (s_master.last_heartbeat_us == 0 ||
                (uint64_t)now - s_master.last_heartbeat_us >=
                    (uint64_t)SYNC_HEARTBEAT_PERIOD_MS * 1000u)
            {
                s_master.last_heartbeat_us = (uint64_t)now;
                sync_send_heartbeat();
            }
        }
    }

    ESP_LOGI(TAG, "SyncTx_Task exit");
    s_master.tx_task = NULL;   /* 自删前清句柄，deinit 不再重复删除 */
    vTaskDelete(NULL);
}

/**
 * @brief SyncLocalPlay_Task：本地延迟队列到点出队，
 *        单声道 → 立体声（L/R 写同一份）后经 amplifier 落 I2S
 */
static void sync_play_task(void *arg)
{
    (void)arg;

    for (;;)
    {
        if (s_master.deinit_pending)
        {
            break;
        }
        if (!s_master.started || s_master.state == SYNC_STATE_PAUSED)
        {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        uint32_t cnt = s_master.delay_count;
        if (cnt == 0)
        {
            xSemaphoreGive(s_lock);
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        /* PREPARING → PLAYING：灌足 D 时长才允许出声 */
        if (s_master.state == SYNC_STATE_PREPARING &&
            cnt * s_master.cfg_frame_ms >= s_master.cfg_delay_ms)
        {
            s_master.state = SYNC_STATE_PLAYING;
            ESP_LOGI(TAG, "state -> PLAYING (D=%u ms buffered)",
                     s_master.cfg_delay_ms);
            xSemaphoreGive(s_lock);
            sync_emit_event(SYNC_EVT_STATE_CHANGED);
            continue;
        }

        /* PREPARING 阶段只攒 D 缓冲、不出队播放，否则队列永远灌不满、
         * 状态卡在 PREPARING（第九阶段自环测试 [D] 暴露） */
        if (s_master.state == SYNC_STATE_PREPARING)
        {
            xSemaphoreGive(s_lock);
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        /* 取队首快照（不在此出队，避免提前失队；t_play 后真正 pop） */
        sync_delay_entry_t ent;
        memcpy(&ent, &s_master.delay_q[s_master.delay_head], sizeof(ent));
        int64_t play_at = (int64_t)ent.t_tx_us +
                          (int64_t)s_master.cfg_delay_ms * 1000;
        xSemaphoreGive(s_lock);

        int64_t now_us = esp_timer_get_time();
        if (now_us < play_at - SYNC_PLAY_DELTA_TOLERATE_US)
        {
            int64_t gap_us = play_at - now_us;
            if (gap_us > 5000)
            {
                vTaskDelay(pdMS_TO_TICKS((gap_us - 5000) / 1000));
            }
            else
            {
                vTaskDelay(pdMS_TO_TICKS(1));
            }
            continue;
        }
        /* 距离目标 < 2ms：忙等到时刻，提高与从节点同时出声的精度 */
        while (esp_timer_get_time() < play_at)
        {
        }

        /* 到点出队 */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_master.delay_count == 0)
        {
            xSemaphoreGive(s_lock);
            continue;
        }
        sync_delay_pop(&ent);
        xSemaphoreGive(s_lock);

        /* 单声道 → 立体声（L/R 写同一份），喂 amplifier */
        uint16_t n = ent.frame_samples;
        if (n > 0 && n <= SYNC_FRAME_MAX_SAMPLES && s_master.play_buf != NULL)
        {
            int16_t *pb = s_master.play_buf;
            for (uint16_t i = 0; i < n; i++)
            {
                pb[2*i]     = ent.pcm[i];
                pb[2*i + 1] = ent.pcm[i];
            }
            esp_err_t r = Amplifier_Play_Buffer((const uint8_t *)pb, n * 4,
                                                 NULL, SYNC_PLAY_WRITE_TIMEOUT_MS);
            if (r != ESP_OK)
            {
                s_master.dropped_pkts++;
            }
        }
        s_master.last_played_pts_us = (int64_t)ent.t_tx_us;
        s_master.play_delta_us      = esp_timer_get_time() - play_at;
    }

    ESP_LOGI(TAG, "SyncLocalPlay_Task exit");
    s_master.play_task = NULL; /* 自删前清句柄，deinit 不再重复删除 */
    vTaskDelete(NULL);
}

/* ======================== 公共 API ======================== */

/** PSRAM 优先分配（与现有中间件约定一致，数据面缓冲尽量放 PSRAM） */
static void *sync_alloc_psram(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == NULL)
    {
        p = heap_caps_malloc(n, MALLOC_CAP_8BIT);
    }
    return p;
}

static void sync_free_all_buffers(void)
{
    if (s_master.ingress_buf != NULL)
    {
        heap_caps_free(s_master.ingress_buf);
        s_master.ingress_buf = NULL;
    }
    if (s_master.delay_q != NULL)
    {
        heap_caps_free(s_master.delay_q);
        s_master.delay_q = NULL;
    }
    if (s_master.frame_buf != NULL)
    {
        heap_caps_free(s_master.frame_buf);
        s_master.frame_buf = NULL;
    }
    if (s_master.play_buf != NULL)
    {
        heap_caps_free(s_master.play_buf);
        s_master.play_buf = NULL;
    }
    s_master.delay_cap = 0;
}

/** 复位单路流的所有运行态（start/stop/pause/resume 换源时调用） */
static void sync_reset_stream(void)
{
    s_master.frame_filled    = 0;
    s_master.frame_target    = 661;   /* 首帧 661 采样，之后 661/662 交替 */
    s_master.next_slot_us    = 0;
    s_master.first_tx_us     = 0;
    s_master.tx_seq          = 0;
    s_master.tx_pkts         = 0;
    s_master.dropped_pkts    = 0;
    s_master.last_played_pts_us = 0;
    s_master.play_delta_us   = 0;
    s_master.start_sent      = false;
    s_master.last_heartbeat_us = 0;
    if (s_master.ingress != NULL)
    {
        xStreamBufferReset(s_master.ingress);
    }
    sync_delay_clear();
}

esp_err_t sync_protocol_master_init(const sync_protocol_master_cfg_t *cfg)
{
    uint32_t need_pkts;

    if (cfg == NULL || cfg->audio_chan == NULL || cfg->control_chan == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL)
    {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_master.inited)
    {
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "already initialized, re-init ignored");
        return ESP_OK;
    }

    memset(&s_master, 0, sizeof(s_master));
    s_master.cfg_delay_ms      = cfg->sync_delay_ms ? cfg->sync_delay_ms
                                                    : SYNC_CFG_DEFAULT_DELAY_MS;
    s_master.cfg_frame_ms      = cfg->frame_duration_ms ? cfg->frame_duration_ms
                                                        : SYNC_CFG_DEFAULT_FRAME_MS;
    s_master.cfg_ingress_bytes = cfg->ingress_bytes ? cfg->ingress_bytes
                                                    : SYNC_CFG_DEFAULT_INGRESS_BYTES;
    s_master.cfg_delay_pkts    = cfg->delay_queue_pkts ? cfg->delay_queue_pkts
                                                       : SYNC_CFG_DEFAULT_DELAY_PKTS;
    s_master.audio_chan        = cfg->audio_chan;
    s_master.control_chan      = cfg->control_chan;

    /* 本地延迟队列容量下限：必须装下一整段 D（再加 2 帧余量） */
    need_pkts = (s_master.cfg_delay_ms + s_master.cfg_frame_ms - 1)
                / s_master.cfg_frame_ms + 2;
    if (s_master.cfg_delay_pkts < need_pkts)
    {
        ESP_LOGW(TAG, "delay_queue_pkts %u < needed %u, clamp up",
                 (unsigned)s_master.cfg_delay_pkts, (unsigned)need_pkts);
        s_master.cfg_delay_pkts = need_pkts;
    }

    /* 缓冲分配（PSRAM 优先） */
    s_master.ingress_buf = sync_alloc_psram(s_master.cfg_ingress_bytes);
    s_master.delay_q     = sync_alloc_psram((size_t)s_master.cfg_delay_pkts *
                                            sizeof(sync_delay_entry_t));
    s_master.frame_buf   = sync_alloc_psram(SYNC_FRAME_MAX_SAMPLES * sizeof(int16_t));
    s_master.play_buf    = sync_alloc_psram(SYNC_FRAME_MAX_SAMPLES * 2 * sizeof(int16_t));
    if (s_master.ingress_buf == NULL || s_master.delay_q == NULL ||
        s_master.frame_buf == NULL || s_master.play_buf == NULL)
    {
        ESP_LOGE(TAG, "buffer alloc failed");
        sync_free_all_buffers();
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }
    s_master.ingress = xStreamBufferCreateStatic(s_master.cfg_ingress_bytes,
                                                 s_master.cfg_ingress_bytes,
                                                 s_master.ingress_buf, &s_master.ingress_sb);
    if (s_master.ingress == NULL)
    {
        sync_free_all_buffers();
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }
    s_master.delay_cap = s_master.cfg_delay_pkts;

    s_master.state   = SYNC_STATE_IDLE;
    s_master.inited  = true;
    s_master.started = false;

    if (xTaskCreatePinnedToCore(sync_tx_task, "SyncTx_Task",
                                SYNC_TX_TASK_STACK, NULL, SYNC_TX_TASK_PRIO,
                                &s_master.tx_task, SYNC_TX_TASK_CORE) != pdPASS)
    {
        ESP_LOGE(TAG, "SyncTx_Task create failed");
    }
    if (xTaskCreatePinnedToCore(sync_play_task, "SyncLocalPlay_Task",
                                SYNC_PLAY_TASK_STACK, NULL, SYNC_PLAY_TASK_PRIO,
                                &s_master.play_task, SYNC_PLAY_TASK_CORE) != pdPASS)
    {
        ESP_LOGE(TAG, "SyncLocalPlay_Task create failed");
    }
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "init ok: D=%u ms frame=%u ms dq_pkts=%u ingress=%u B",
             s_master.cfg_delay_ms, s_master.cfg_frame_ms,
             s_master.cfg_delay_pkts, s_master.cfg_ingress_bytes);
    return ESP_OK;
}

esp_err_t sync_protocol_master_deinit(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_master.inited)
    {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    s_master.deinit_pending = true;
    s_master.started = false;
    xSemaphoreGive(s_lock);

    /* 两个内部任务看到 deinit_pending 后自行退出并清句柄；
     * 这里只能等待，不能再 vTaskDelete（句柄已被自删，重复删除会
     * 在 uxListRemove 崩溃）。最多等 1s（Play 任务写 I2S 超时上限 500ms） */
    for (int wait = 0; wait < 100 &&
                        (s_master.tx_task != NULL || s_master.play_task != NULL);
         wait++)
    {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    s_master.tx_task   = NULL;
    s_master.play_task = NULL;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    sync_free_all_buffers();
    if (s_master.ingress != NULL)
    {
        s_master.ingress = NULL;
    }
    s_master.inited = false;
    s_master.deinit_pending = false;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "deinit ok");
    return ESP_OK;
}

esp_err_t sync_protocol_master_register_event_cb(sync_protocol_event_cb_t cb,
                                                 void *user_ctx)
{
    if (!s_master.inited)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_master.event_cb  = cb;
    s_master.event_ctx = user_ctx;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t sync_protocol_master_start(uint16_t stream_id,
                                     uint32_t sample_rate,
                                     uint8_t channels)
{
    bool stream_changed = false;

    if (!s_master.inited)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (sample_rate == 0 || channels == 0 || channels > 2)
    {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_master.sample_rate = sample_rate;
    s_master.channels    = channels;
    if (stream_id == 0)
    {
        s_master.stream_id = (uint16_t)(s_master.auto_stream_seq + 1);
        s_master.auto_stream_seq++;
    }
    else if (stream_id != s_master.stream_id)
    {
        s_master.stream_id  = stream_id;
        stream_changed = true;
    }
    sync_reset_stream();
    s_master.started = true;
    s_master.state   = SYNC_STATE_PREPARING;
    xSemaphoreGive(s_lock);

    if (stream_changed)
    {
        sync_emit_event(SYNC_EVT_STREAM_CHANGED);
    }
    sync_emit_event(SYNC_EVT_STATE_CHANGED);
    ESP_LOGI(TAG, "start: stream=%u %luHz %uch", s_master.stream_id,
             (unsigned long)s_master.sample_rate, s_master.channels);
    return ESP_OK;
}

esp_err_t sync_protocol_master_stop(void)
{
    if (!s_master.inited)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_master.started)
    {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    sync_reset_stream();
    s_master.started = false;
    s_master.state   = SYNC_STATE_STOPPED;
    xSemaphoreGive(s_lock);
    sync_emit_event(SYNC_EVT_STATE_CHANGED);
    ESP_LOGI(TAG, "stop");
    return ESP_OK;
}

esp_err_t sync_protocol_master_push_pcm(const int16_t *pcm, size_t samples)
{
    size_t bytes;

    if (!s_master.inited)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (pcm == NULL || samples == 0 || s_master.ingress == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    bytes = samples * sizeof(int16_t);
    if (xStreamBufferSend(s_master.ingress, pcm, bytes, 0) != bytes)
    {
        /* 非阻塞：上游太快时丢帧并计数，保持“绝不阻塞调用方”语义 */
        s_master.dropped_pkts++;
    }
    return ESP_OK;
}

esp_err_t sync_protocol_master_pause(void)
{
    if (!s_master.inited)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_master.started)
    {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    sync_delay_clear();
    s_master.frame_filled = 0;
    s_master.state   = SYNC_STATE_PAUSED;
    xSemaphoreGive(s_lock);
    sync_send_ctrl_basic(SYNC_CMD_PAUSE, 0);
    sync_emit_event(SYNC_EVT_STATE_CHANGED);
    ESP_LOGI(TAG, "paused");
    return ESP_OK;
}

esp_err_t sync_protocol_master_resume(void)
{
    if (!s_master.inited)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_master.started)
    {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    sync_reset_stream();
    s_master.state = SYNC_STATE_PREPARING;
    xSemaphoreGive(s_lock);
    sync_send_ctrl_basic(SYNC_CMD_PLAY, 0);
    sync_emit_event(SYNC_EVT_STATE_CHANGED);
    ESP_LOGI(TAG, "resumed");
    return ESP_OK;
}

esp_err_t sync_protocol_master_set_delay_ms(uint32_t delay_ms)
{
    if (!s_master.inited)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_master.cfg_delay_ms = delay_ms ? delay_ms : SYNC_CFG_DEFAULT_DELAY_MS;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "delay_ms -> %u", s_master.cfg_delay_ms);
    return ESP_OK;
}

esp_err_t sync_protocol_master_set_sample_rate(uint32_t sample_rate)
{
    if (!s_master.inited)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (sample_rate == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_master.sample_rate = sample_rate;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t sync_protocol_master_set_channels(uint8_t channels)
{
    if (!s_master.inited)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (channels == 0 || channels > 2)
    {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_master.channels = channels;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t sync_protocol_master_broadcast_cmd(sync_protocol_cmd_t cmd,
                                             uint8_t param)
{
    if (!s_master.inited || s_master.control_chan == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (cmd < SYNC_CMD_PLAY || cmd > SYNC_CMD_RESET)
    {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t r = sync_send_ctrl_basic(cmd, param);
    if (r == ESP_OK)
    {
        ESP_LOGI(TAG, "broadcast cmd 0x%02X param %u", cmd, param);
    }
    return r;
}

esp_err_t sync_protocol_master_get_status(sync_protocol_master_status_t *status)
{
    if (!s_master.inited || status == NULL)
    {
        return status == NULL ? ESP_ERR_INVALID_ARG : ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    sync_snapshot(status);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

uint32_t sync_protocol_master_get_tx_pkts(void)
{
    return s_master.tx_pkts;
}

uint32_t sync_protocol_master_get_dropped_pkts(void)
{
    return s_master.dropped_pkts;
}
