/**
 * @file sync_protocol.c
 * @brief 音频同步协议模块实现（主节点侧）：PCM 汇聚 → 分包 → **单播**发送 +
 *        本地延迟播放 + 控制通道处理
 *
 * 实现要点（对应《主音频节点软件架构分层设计.md》3.2.8 / 第 6 节任务表）：
 *   - SyncTx_Task：优先级 11 / 栈 4096 / Core 0。轮询入口环攒够一帧（约 15ms），
 *     算出基准播放时刻 t_play = t_tx + D → 逐台从机**单播** → 推入本地延迟队列；
 *     同时处理控制通道（PING → PONG 应答、SYNC_START 一次下发、
 *      SYNC_HEARTBEAT 每秒下发、从节点在线超时检测）。
 *     本模块不持有 UDP socket（wifi_manager 通道句柄传入）。
 *
 * 为什么数据面用单播：802.11 的组播帧没有 ACK、没有重传、强制基本速率、
 * 还要被 AP 按 DTIM 缓存；单播三者都有。8 台从机的总带宽约 5.6 Mbps，
 * 在这个规模上组播省下的那点带宽毫无意义。
 *     栈内最大局部为延迟队列入队快照 sync_delay_entry_t（含 pcm[700] ≈ 1.4KB），
 *     组包缓冲已移到模块 PSRAM（tx_pkt），故 4KB 足够（第十一阶段内部 DRAM
 *     紧张，由 8KB 降下；帧/组包缓冲均在 PSRAM）。
 *   - SyncLocalPlay_Task：优先级 10 / 栈 6144 / Core 1。按
 *     t_play + SYNC_RX_MARGIN_US 出队（与从节点的换算公式一致，否则会比
 *     从机早出声 60ms），单声道 → 立体声（L/R 写入同一份）后经 amplifier 写 I2S。
 *     栈内最大局部为出队快照 sync_delay_entry_t（含 pcm[700] ≈ 1.4KB），
 *     6144 足够（第十一阶段由 10240 降栈省内部 DRAM）。
 *   - 帧节奏：每帧 15ms，帧大小在 661/662 采样之间交替，保证平均 44.1kHz
 *     （44.1k×0.015=661.5 非整数）；
 *   - 独立于音源：push_pcm 线程安全入环；播放中环满最多阻塞 30ms 做背压
 *     （让解码端能跑在消费速率之前形成缓冲），暂停/停止时非阻塞丢弃。
 *
 * 本地最终出声使用 Amplifier_Play_Buffer()（第九阶段讨论决定），因此
 * 本模块不注册任何写者：经 Amplifier_Play_Buffer() 直接写 I2S。
 */

#include "sync_protocol.h"

#include "amplifier.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#define TAG "SYNC_PROTOCOL"

#if SYNC_PROTOCOL_DEBUG_LOGS
#define SYNC_LOGE(...) ESP_LOGE(TAG, __VA_ARGS__)
#define SYNC_LOGW(...) ESP_LOGW(TAG, __VA_ARGS__)
#define SYNC_LOGI(...) ESP_LOGI(TAG, __VA_ARGS__)
#define SYNC_LOGD(...) ESP_LOGD(TAG, __VA_ARGS__)
#else
#define SYNC_LOGE(...) do { } while (0)
#define SYNC_LOGW(...) do { } while (0)
#define SYNC_LOGI(...) do { } while (0)
#define SYNC_LOGD(...) do { } while (0)
#endif

/* ======================== 默认参数 ======================== */

/** 统一同步缓冲 D（cfg->sync_delay_ms 为 0 时使用） */
#define SYNC_CFG_DEFAULT_DELAY_MS        (200)
/** 每包 PCM 时长（cfg->frame_duration_ms 为 0 时使用） */
#define SYNC_CFG_DEFAULT_FRAME_MS        (15)
/** 本地延迟队列默认容量。
 *  队列里压着的帧 = 提前量 D + 接收余量（260ms）/ 每包时长 ≈ 18 帧 */
#define SYNC_CFG_DEFAULT_DELAY_PKTS      (24)
/** PCM 入口环默认字节容量。
 *  44.1kHz×16bit mono：24KB≈278ms，64KB≈741ms。
 *  加大到 64KB 用于吸收 UI 切页/滚动整屏重绘抢占解码任务造成的欠载窗口
 *  （约几十~200ms），播放节奏不再"变慢→追帧"抖动。缓冲经 sync_alloc_psram
 *  落 8MB PSRAM，不占内部 DRAM，也不影响主从同步误差（误差由 D 决定）。 */
#define SYNC_CFG_DEFAULT_INGRESS_BYTES   (65536)

/**
 * 单播目标从机的**编译期默认列表**。
 *
 * 单播必须知道对端 IP。新从节点固件会每 3 秒在控制通道发 ANNOUNCE，
 * 主节点收到后按 MAC 维护运行时从机表，并自动用"在线从机 IP"覆盖这里；
 * 静态列表只在**还没收到过任何公告**时兜底（兼容旧固件 / 从机没上电）。
 *
 * 列表为空时音频只在本机播放，不往网络上发。
 */
static const char *const SYNC_DEFAULT_SLAVE_IPS[] = {
    "192.168.31.45",
};
#define SYNC_DEFAULT_SLAVE_COUNT \
    ((uint8_t)(sizeof(SYNC_DEFAULT_SLAVE_IPS) / sizeof(SYNC_DEFAULT_SLAVE_IPS[0])))

/** SyncTx_Task：优先级/栈/核（任务表） */
#define SYNC_TX_TASK_PRIO   (11)
#define SYNC_TX_TASK_STACK  (4096)
#define SYNC_TX_TASK_CORE   (0)

/** SyncLocalPlay_Task：优先级/栈/核（任务表）
 *  优先级 12：高于 Audio_Decoder(10)，保证按时喂 I2S DMA，避免解码忙时
 *  play task 被饿死在同核产生 I2S underrun → 嗡嗡嗡 */
#define SYNC_PLAY_TASK_PRIO  (12)
#define SYNC_PLAY_TASK_STACK (4096)
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

/** 本地播放写入 I2S 超时。
 *  一帧只有 15ms 音频（约 2.6KB），DMA 有 8×256 帧 ≈ 46ms 的余量，
 *  正常情况下写立即完成、最坏也就等十几毫秒。原来的 500ms 一旦 I2S
 *  侧停转（DMA 不开/功放无时钟），每帧都要白等半秒，听感上就是
 *  "卡死"，而且会把 Watchdog 的判定拖进噪声里。100ms 足够，异常时快速失败。 */
#define SYNC_PLAY_WRITE_TIMEOUT_MS 100

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
    uint64_t t_play_us;            /* 基准播放时刻 = t_tx + D，发给从节点的就是这个 */
    uint16_t frame_samples;        /* 本帧 mono 采样数（661/662） */
    int16_t  pcm[SYNC_FRAME_MAX_SAMPLES];
} sync_delay_entry_t;

/** 从节点表条目：对外快照 + 内部单调时间基准 */
typedef struct {
    sync_slave_info_t info;
    uint64_t          last_seen_us;   /* 最近一次合法 ANNOUNCE 的 esp_timer */
} sync_slave_entry_t;

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
    uint32_t stream_gen;           /* 流代号：sync_reset_stream 自增（stop/start/
                                    * resume 复位本流），SyncTx_Task 提交校验用 */

    uint32_t cfg_delay_ms;
    uint32_t cfg_frame_ms;
    uint32_t cfg_delay_pkts;
    uint32_t cfg_ingress_bytes;
    wifi_mcast_handle_t audio_chan;
    wifi_mcast_handle_t control_chan;

    /* 单播目标（拷贝进模块自己的缓冲，调用方的字符串不必常驻） */
    char     slave_ips[SYNC_MAX_SLAVES][16];
    uint8_t  slave_count;

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
    uint8_t *tx_pkt;              /* 组播组包缓冲（PSRAM，避免压 sync_tx_task 栈） */

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
    uint32_t silence_frames;   /* 露底时写的静音帧数（每帧=1个 frame_ms）：
                                * 增长即"延迟队列被耗尽"，是卡顿的最终判据 */

    /* 时间同步 */
    bool     slave_online;
    uint64_t last_ping_us;
    uint64_t last_heartbeat_us;
    bool     start_sent;

    /* 从节点发现（ANNOUNCE，控制通道 5679） */
    sync_slave_entry_t slaves[SYNC_MAX_SLAVES];
    uint8_t  slave_known;          /* 表内条目数（0~SYNC_MAX_SLAVES） */
    bool     announce_seen;        /* 收到过合法公告后，单播目标改为按在线表重建 */
    uint64_t last_sweep_us;        /* 离线扫描节流（1s） */

    /* 事件 */
    sync_protocol_event_cb_t event_cb;
    void                    *event_ctx;

    TaskHandle_t tx_task;
    TaskHandle_t play_task;
} sync_master_t;

static sync_master_t s_master;
static SemaphoreHandle_t s_lock = NULL;   /* 保护 state/参数/队列元数据 */
static SemaphoreHandle_t s_push_lock = NULL; /* 串行化入口环的生产者（解码/蓝牙） */

/* 内部 IO 中转（避免任务栈过大） */
static uint8_t  s_io_buf[SYNC_IO_BUF_BYTES];

/* ======================== 内部工具 ======================== */

/** 把调用方的字符串列表拷进模块自己的缓冲（调用方必须持锁） */
static esp_err_t sync_set_slaves_locked(const char *const *ips, uint8_t count)
{
    if (ips == NULL || count == 0)
    {
        s_master.slave_count = 0;
        return ESP_OK;
    }
    if (count > SYNC_MAX_SLAVES)
    {
        count = SYNC_MAX_SLAVES;
    }

    s_master.slave_count = 0;
    for (uint8_t i = 0; i < count; i++)
    {
        if (ips[i] == NULL || ips[i][0] == '\0')
        {
            continue;
        }
        strncpy(s_master.slave_ips[s_master.slave_count], ips[i],
                sizeof(s_master.slave_ips[0]) - 1);
        s_master.slave_ips[s_master.slave_count][sizeof(s_master.slave_ips[0]) - 1] = '\0';
        s_master.slave_count++;
    }
    return ESP_OK;
}

/* ---------------- 从节点发现（ANNOUNCE） ---------------- */

/** 是否有在线从机（须持 s_lock） */
static bool sync_any_slave_online_locked(void)
{
    for (uint8_t i = 0; i < s_master.slave_known; i++)
    {
        if (s_master.slaves[i].info.online)
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief 用"在线从机表"重建单播目标（须持 s_lock）
 *
 * 收到第一份合法公告之前保持编译期/运行时配置的静态列表，保证旧固件
 * （不发公告）和没插从机的场景行为不变；收到公告之后，发现表成为唯一
 * 权威——离线从机不再浪费单播带宽，回来后由下一份公告自动加回。
 */
static void sync_rebuild_targets_locked(void)
{
    const char *ips[SYNC_MAX_SLAVES];
    uint8_t count = 0;

    if (!s_master.announce_seen)
    {
        return;
    }
    for (uint8_t i = 0; i < s_master.slave_known; i++)
    {
        if (s_master.slaves[i].info.online)
        {
            ips[count++] = s_master.slaves[i].info.ip;
        }
    }
    (void)sync_set_slaves_locked(count ? ips : NULL, count);
}

/**
 * @brief 处理一条 SYNC_MSG_ANNOUNCE（在 SyncTx_Task 上下文调用）
 *
 * 按 MAC upsert：IP 变化时直接覆盖（DHCP 换地址后仍能继续收音频）。
 * 表满时顶掉"最久没消息的离线条目"；全部在线则忽略新设备，避免把
 * 正在播的从机挤掉。
 */
static void sync_announce_handle(const sync_ctrl_ext_t *ext, size_t pkt_len)
{
    sync_announce_payload_t a;
    char ip[16];
    char name[sizeof(s_master.slaves[0].info.name)];
    int64_t now_us;
    int idx = -1;
    bool became_online = false;

    if (ext->len != sizeof(a) ||
        pkt_len < sizeof(sync_ctrl_ext_t) + sizeof(a))
    {
        return;
    }
    memcpy(&a, ext->data, sizeof(a));
    if (a.ver != SYNC_ANNOUNCE_VERSION || a.token != SYNC_ANNOUNCE_TOKEN)
    {
        return;
    }
    if (a.audio_port == 0 ||
        (a.ip[0] | a.ip[1] | a.ip[2] | a.ip[3]) == 0)
    {
        return;
    }

    snprintf(ip, sizeof(ip), "%u.%u.%u.%u",
             (unsigned)a.ip[0], (unsigned)a.ip[1],
             (unsigned)a.ip[2], (unsigned)a.ip[3]);

    /* 设备名：载荷里是定长字段，先补 NUL；空名字回退到 MAC 后缀 */
    memset(name, 0, sizeof(name));
    memcpy(name, a.name, sizeof(a.name));
    if (name[0] == '\0')
    {
        snprintf(name, sizeof(name), "从机-%02X%02X", a.mac[4], a.mac[5]);
    }

    now_us = esp_timer_get_time();

    xSemaphoreTake(s_lock, portMAX_DELAY);

    for (uint8_t i = 0; i < s_master.slave_known; i++)
    {
        if (memcmp(s_master.slaves[i].info.mac, a.mac, sizeof(a.mac)) == 0)
        {
            idx = (int)i;
            break;
        }
    }

    if (idx < 0)
    {
        if (s_master.slave_known < SYNC_MAX_SLAVES)
        {
            idx = (int)s_master.slave_known++;
        }
        else
        {
            /* 表满：挑最久没消息的离线条目复用；全在线则不接纳新设备 */
            uint64_t oldest = UINT64_MAX;
            for (uint8_t i = 0; i < s_master.slave_known; i++)
            {
                if (!s_master.slaves[i].info.online &&
                    s_master.slaves[i].last_seen_us < oldest)
                {
                    oldest = s_master.slaves[i].last_seen_us;
                    idx = (int)i;
                }
            }
            if (idx < 0)
            {
                xSemaphoreGive(s_lock);
                SYNC_LOGW("slave table full (%u online), ignore %s",
                          (unsigned)s_master.slave_known, ip);
                return;
            }
            memset(&s_master.slaves[idx], 0, sizeof(s_master.slaves[idx]));
        }
    }

    sync_slave_entry_t *e = &s_master.slaves[idx];
    memcpy(e->info.mac, a.mac, sizeof(a.mac));
    snprintf(e->info.name, sizeof(e->info.name), "%s", name);
    snprintf(e->info.ip, sizeof(e->info.ip), "%s", ip);
    e->info.audio_port = a.audio_port;
    e->info.volume     = (a.volume > 100) ? 100 : a.volume;
    e->info.rssi       = a.rssi;
    became_online      = !e->info.online;
    e->info.online     = true;
    e->last_seen_us    = (uint64_t)now_us;

    s_master.announce_seen = true;
    s_master.slave_online  = sync_any_slave_online_locked();
    /* 每份公告都重建：从机可能在保持在线的情况下换了 IP（DHCP 续租），
     * 只在新上线时重建会让主节点继续往旧地址发。8 条字符串，开销可忽略。 */
    sync_rebuild_targets_locked();
    uint8_t volume  = e->info.volume;
    uint8_t targets = s_master.slave_count;

    xSemaphoreGive(s_lock);

    if (became_online)
    {
        SYNC_LOGI("slave online: %s ip=%s rssi=%d vol=%u (targets=%u)",
                  name, ip, (int)a.rssi, (unsigned)volume,
                  (unsigned)targets);
    }
}

/** 离线扫描：1s 一次，超时标离线并重建单播目标（SyncTx_Task 上下文） */
static void sync_slaves_sweep(void)
{
    int64_t now_us = esp_timer_get_time();
    bool changed = false;

    if (now_us - (int64_t)s_master.last_sweep_us < 1000000)
    {
        return;
    }
    s_master.last_sweep_us = (uint64_t)now_us;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (uint8_t i = 0; i < s_master.slave_known; i++)
    {
        sync_slave_entry_t *e = &s_master.slaves[i];
        if (e->info.online &&
            (uint64_t)now_us - e->last_seen_us >
                (uint64_t)SYNC_SLAVE_OFFLINE_TIMEOUT_MS * 1000u)
        {
            e->info.online = false;
            changed = true;
            SYNC_LOGW("slave lost: %s (%s)", e->info.name, e->info.ip);
        }
    }
    if (changed)
    {
        sync_rebuild_targets_locked();
    }
    if (s_master.announce_seen)
    {
        s_master.slave_online = sync_any_slave_online_locked();
    }
    xSemaphoreGive(s_lock);
}

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
    out->silence_frames = s_master.silence_frames;
    out->play_delta_us = s_master.play_delta_us;
    out->slave_online  = s_master.slave_online;
    out->last_slave_offset_us = 0;   /* 主节点不能单边计算 offset */
    out->slave_targets = s_master.slave_count;
    out->slaves_online = 0;
    for (uint8_t i = 0; i < s_master.slave_known; i++)
    {
        if (s_master.slaves[i].info.online)
        {
            out->slaves_online++;
        }
    }
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

/** 提交校验（须持 s_lock 调用）：本帧所属流未被复位，且仍在放音态 */
static bool sync_stream_valid_locked(uint32_t gen)
{
    return (s_master.started &&
            (s_master.state == SYNC_STATE_PLAYING ||
             s_master.state == SYNC_STATE_PREPARING) &&
            s_master.stream_gen == gen);
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
        SYNC_LOGW("SYNC_START send failed");
    }
    else
    {
        SYNC_LOGI("SYNC_START sent: stream=%u t0=%lld d=%u ms",
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
        if (ext->type == SYNC_MSG_ANNOUNCE)
        {
            sync_announce_handle(ext, len);
            continue;
        }
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
            if (!s_master.announce_seen && s_master.slave_online == false)
            {
                s_master.slave_online = true;
                SYNC_LOGI("slave online (first PING)");
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
                SYNC_LOGW("PONG send failed");
            }
            SYNC_LOGD("PING(t1=%" PRId64 ") -> PONG(t2=%" PRId64 ", t3=%" PRId64 ")",
                     (int64_t)ping.t1_us, (int64_t)pong.t2_us, (int64_t)pong.t3_us);
        }
    }

    /* 从节点在线超时检测（如已超过一个 PING 周期未收到）。
     * 收到过 ANNOUNCE 之后，在线状态由发现表的超时逻辑接管，
     * 旧的 PING 超时不能再来改 slave_online。 */
    if (!s_master.announce_seen && s_master.slave_online &&
        ((uint64_t)esp_timer_get_time() - s_master.last_ping_us) >
            (uint64_t)SYNC_SLAVE_ONLINE_TIMEOUT_MS * 1000u)
    {
        s_master.slave_online = false;
        SYNC_LOGW("slave lost (no PING in %u ms)", SYNC_SLAVE_ONLINE_TIMEOUT_MS);
        sync_emit_event(SYNC_EVT_SLAVE_LOST);
    }

    /* 公告超时扫描（1s 节流，在 TX 任务上下文执行） */
    sync_slaves_sweep();
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

    /* 订阅任务看门狗：它一卡，从机立刻断流、本地延迟队列也会被抽干。
     * 卡在 s_lock（含锁内的 SYNC_START 网络发送）或忙等时由 TWDT 报出。 */
    (void)esp_task_wdt_add(NULL);

    for (;;)
    {
        (void)esp_task_wdt_reset();

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
        uint32_t stream_gen = s_master.stream_gen;  /* 本帧所属流代号（提交校验用） */
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
            size_t unit = (size_t)s_master.channels * 2;
            if (unit != 0 && (got % unit) != 0)
            {
                /* 环里读出了半个采样：采样边界已无法恢复。与其把错位字节
                 * 拼成满量程噪声、一直响到用户手动暂停再播放，不如丢掉
                 * 整条流重新积累（最多一次 D 的静音缺口）。
                 * 生产端已按整采样写入，正常情况不会走到这里。 */
                SYNC_LOGE("ingress misaligned (got=%u unit=%u), reset buffer",
                          (unsigned)got, (unsigned)unit);
                xSemaphoreTake(s_push_lock, portMAX_DELAY);
                xStreamBufferReset(s_master.ingress);
                xSemaphoreGive(s_push_lock);
                s_master.frame_filled = 0;
                break;
            }
            size_t frames = got / unit;
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
                int64_t t_tx   = esp_timer_get_time();
                int64_t t_play = t_tx + (int64_t)s_master.cfg_delay_ms * 1000;

                /* 发送前校验：stop/start/resume 会 sync_reset_stream（流代号自增），
                 * 若本帧所属的流已被复位则直接丢弃。否则会把上一路流的帧补发到
                 * 网络，并在 start_sent 已被清零时补发一条 t0 属于旧流的
                 * SYNC_START（第十一阶段实测：stop 之后仍补发一帧 + SYNC_START）
                 *
                 * 顺便把单播目标表拷到栈上：发送在锁外做，不能持锁发。 */
                char    slave_ips[SYNC_MAX_SLAVES][16];
                uint8_t slave_count;
                xSemaphoreTake(s_lock, portMAX_DELAY);
                bool stream_ok = sync_stream_valid_locked(stream_gen);
                slave_count = s_master.slave_count;
                memcpy(slave_ips, s_master.slave_ips, sizeof(slave_ips));
                xSemaphoreGive(s_lock);
                if (stream_ok == false)
                {
                    s_master.frame_filled = 0;
                    continue;
                }

                memset(&ent, 0, sizeof(ent));
                ent.stream_id     = s_master.stream_id;
                ent.seq           = s_master.tx_seq;
                ent.t_tx_us       = (uint64_t)t_tx;
                ent.t_play_us     = (uint64_t)t_play;
                ent.frame_samples = (uint16_t)nxt_frame;
                memcpy(ent.pcm, s_master.frame_buf,
                       nxt_frame * sizeof(int16_t));

                /* 组包：缓冲在模块 PSRAM（tx_pkt），不占 sync_tx_task 栈 */
                uint8_t *pkt = s_master.tx_pkt;
                sync_audio_pkt_hdr_t *hdr = (sync_audio_pkt_hdr_t *)pkt;
                size_t pkt_len;
                if (SYNC_PKT_BUF_BYTES < SYNC_AUDIO_HDR_SIZE +
                    nxt_frame * sizeof(int16_t))
                {
                    SYNC_LOGE("packet too large, drop");
                    s_master.dropped_pkts++;
                    s_master.frame_filled = 0;
                    continue;   /* 不应发生 */
                }
                memset(pkt, 0, SYNC_PKT_BUF_BYTES);
                hdr->magic       = SYNC_MAGIC;
                hdr->version     = SYNC_PROTO_VERSION;
                hdr->flags       = (s_master.tx_seq == 0)
                                       ? SYNC_AUDIO_FLAG_STREAM_START : 0;
                hdr->stream_id   = s_master.stream_id;
                hdr->seq         = s_master.tx_seq;
                hdr->t_play_us   = (uint64_t)t_play;
                hdr->payload_len = (uint16_t)(nxt_frame * sizeof(int16_t));
                hdr->lead_ms     = (uint16_t)s_master.cfg_delay_ms;
                memcpy(pkt + SYNC_AUDIO_HDR_SIZE, s_master.frame_buf,
                       nxt_frame * sizeof(int16_t));
                pkt_len = SYNC_AUDIO_HDR_SIZE + nxt_frame * sizeof(int16_t);

                /* 单播给每一台从机：同一份 PCM，一份一份发。
                 * 单播在 802.11 上有 ACK/重传/速率自适应；组播都没有。
                 * timeout 传 0：只尝试一次。帧周期只有 15ms，某台从机离线时
                 * 不能为它反复重试而拖住整条流的节奏；丢的这一帧由从机 PLC 补。 */
                if (slave_count == 0)
                {
                    if (s_master.tx_pkts == 0)
                    {
                        SYNC_LOGW("no slave configured, audio is local only");
                    }
                }
                for (uint8_t i = 0; i < slave_count; i++)
                {
                    if (wifi_manager_unicast_send(s_master.audio_chan,
                                                  slave_ips[i],
                                                  SYNC_UNICAST_PORT_AUDIO,
                                                  pkt, pkt_len, 0) != ESP_OK)
                    {
                        s_master.dropped_pkts++;
                    }
                }

                /* 单播发送期间 stop/start/resume 仍可能复位本流：入延迟队列 +
                 * 起始帧判定必须在锁内再校验一次；SYNC_START 也在锁内发出，
                 * 保证与 stop 串行（stop 之后不再补发） */
                xSemaphoreTake(s_lock, portMAX_DELAY);
                if (sync_stream_valid_locked(stream_gen) == false)
                {
                    xSemaphoreGive(s_lock);
                    s_master.frame_filled = 0;
                    continue;
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
                    sync_send_start();   /* 锁内发送：与 stop 串行，避免补发 */
                }
                s_master.frame_filled = 0;
                s_master.tx_seq++;
                s_master.frame_target = (s_master.tx_seq & 1u) ? 662u : 661u;
                s_master.next_slot_us += (uint64_t)(s_master.cfg_frame_ms * 1000u);
                xSemaphoreGive(s_lock);
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

    SYNC_LOGI("SyncTx_Task exit");
    (void)esp_task_wdt_delete(NULL);   /* 自删前退订，否则 TWDT 会报"任务不存在" */
    s_master.tx_task = NULL;   /* 自删前清句柄，deinit 不再重复删除 */
    vTaskDelete(NULL);
}

/**
 * @brief 非播放态喂一帧全零 PCM，保证 I2S DMA 永远有数据可播
 *
 * 为什么不能"写一帧静音 + 固定 vTaskDelay(15ms)"：
 *   Amplifier_Play_Buffer() 在 DMA 满时本身就会阻塞约一帧（15.8ms），
 *   再睡 15ms 会让喂入速度只有实时的一半 —— DMA 缓冲（约 46ms）被抽干后
 *   数据脚浮空，NS4168 放大成嗡嗡声。这个缺陷会让"喂静音"的修复在
 *   换歌/解码间隙较长时失效（实测一曲将尽时持续嗡嗡）。
 *
 * 这里把**阻塞写本身当作节拍源**：
 *   - 写满一帧（耗时接近一帧时长）→ 说明 DMA 已被喂饱，不再睡；
 *   - 写得很快（DMA 还有余量）→ 让出 1 tick，避免高优先级任务空转。
 */
static void sync_feed_silence(void)
{
    int64_t t0;
    int64_t frame_us = (int64_t)s_master.cfg_frame_ms * 1000;
    int64_t used_us;

    if (s_master.play_buf == NULL)
    {
        vTaskDelay(1);
        return;
    }

    memset(s_master.play_buf, 0, SYNC_FRAME_MAX_SAMPLES * 2 * sizeof(int16_t));
    t0 = esp_timer_get_time();
    (void)Amplifier_Play_Buffer((const uint8_t *)s_master.play_buf,
                                SYNC_FRAME_MAX_SAMPLES * 4, NULL, 100);
    s_master.silence_frames++;   /* 诊断：露底时播了多少帧静音（见 status） */

    /* 必须严格按"一帧 15ms"的节拍补足，而不是写完一帧就立刻写下一帧。
     *
     * 原来的写法是"耗时不足半帧就 vTaskDelay(1)"：DMA 有空位时 2~3ms 就
     * 写完一帧静音，于是静音以数倍实时速度灌进 I2S DMA。DMA 只有
     * 8 × 256 帧 ≈ 46ms，被静音灌满之后，真实 PCM 回来还要先把这 46ms
     * 静音播完——听感就是"卡一下,声音还拖一拍才回来"。按节拍写则让 DMA
     * 保持半满，数据一到立刻就能播出去。 */
    used_us = esp_timer_get_time() - t0;
    if (used_us < frame_us)
    {
        uint32_t wait_ms = (uint32_t)((frame_us - used_us) / 1000);
        vTaskDelay(pdMS_TO_TICKS(wait_ms > 0 ? wait_ms : 1));
    }
}

/**
 * @brief SyncLocalPlay_Task：本地延迟队列到点出队，
 *        单声道 → 立体声（L/R 写同一份）后经 amplifier 落 I2S
 */
static void sync_play_task(void *arg)
{
    (void)arg;

    /* 订阅任务看门狗：本任务是本地出声的最后一环，它一卡就是"音乐停、
     * 串口全静"。订阅后卡住 5s 会由 TWDT 打印所有任务状态，能直接看出
     * 是卡在等锁、等 I2S 还是忙等。 */
    (void)esp_task_wdt_add(NULL);

    for (;;)
    {
        (void)esp_task_wdt_reset();

        if (s_master.deinit_pending)
        {
            break;
        }
        if (!s_master.started || s_master.state == SYNC_STATE_PAUSED)
        {
            sync_feed_silence();
            continue;
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        uint32_t cnt = s_master.delay_count;
        if (cnt == 0)
        {
            xSemaphoreGive(s_lock);
            sync_feed_silence();
            continue;
        }
        /* PREPARING → PLAYING：灌足 D 时长才允许出声 */
        if (s_master.state == SYNC_STATE_PREPARING &&
            cnt * s_master.cfg_frame_ms >= s_master.cfg_delay_ms)
        {
            s_master.state = SYNC_STATE_PLAYING;
            SYNC_LOGI("state -> PLAYING (D=%u ms buffered)",
                     s_master.cfg_delay_ms);
            xSemaphoreGive(s_lock);
            sync_emit_event(SYNC_EVT_STATE_CHANGED);
            continue;
        }

        /* PREPARING 阶段只攒 D 缓冲、不出队播放，否则队列永远灌不满、
         * 状态卡在 PREPARING（第九阶段自环测试 [D] 暴露）。
         * 但攒缓冲期间 I2S 不能断粮：解码/开卡可能要几百 ms，DMA 46ms
         * 就放空了，所以这里同样喂静音帧。 */
        if (s_master.state == SYNC_STATE_PREPARING)
        {
            xSemaphoreGive(s_lock);
            sync_feed_silence();
            continue;
        }

        /* 取队首快照（不在此出队，避免提前失队；t_play 后真正 pop）
         *
         * 出声时刻 = t_play_us + SYNC_RX_MARGIN_US，与从节点同一公式：
         * 从节点为了让抖动最大的包也能按时到齐，会把它算出的偏移再往后
         * 推 UDIO_MARGIN_US；主节点不跟着推，就会比从节点早出声 60ms，
         * 同一间屋子能听出回声。两边的余量宏必须保持一致。 */
        sync_delay_entry_t ent;
        memcpy(&ent, &s_master.delay_q[s_master.delay_head], sizeof(ent));
        int64_t play_at = (int64_t)ent.t_play_us + SYNC_RX_MARGIN_US;
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
        /* 距离目标 < 2ms：忙等到时刻，提高与从节点同时出声的精度。
         *
         * 必须带硬上限：play_at 由延迟队列里的 t_play_us 换算而来，正常只在
         * "差 2ms 以内"才会进这个循环；但如果队列里混进一条异常时间戳
         * （换流残留/被写坏），无上限的忙等在 prio 12 上会把整个 core 1
         * 饿死 —— 外部表现就是"放着放着突然卡死，串口再无任何输出，手机
         * 却仍显示已连接"。加界之后最多多等 10ms 就放弃这一帧。 */
        int64_t busy_deadline = esp_timer_get_time() + 10000;
        while (esp_timer_get_time() < play_at &&
               esp_timer_get_time() < busy_deadline)
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
        s_master.last_played_pts_us = (int64_t)ent.t_play_us;
        s_master.play_delta_us      = esp_timer_get_time() - play_at;
    }

    SYNC_LOGI("SyncLocalPlay_Task exit");
    (void)esp_task_wdt_delete(NULL);   /* 自删前退订 */
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
    if (s_master.tx_pkt != NULL)
    {
        heap_caps_free(s_master.tx_pkt);
        s_master.tx_pkt = NULL;
    }
    s_master.delay_cap = 0;
}

/** 复位单路流的所有运行态（start/stop/pause/resume 换源时调用） */
static void sync_reset_stream(void)
{
    s_master.stream_gen++;         /* 流代号自增：SyncTx_Task 据此丢弃上一路流的帧 */
    s_master.frame_filled    = 0;
    s_master.frame_target    = 661;   /* 首帧 661 采样，之后 661/662 交替 */
    s_master.next_slot_us    = 0;
    s_master.first_tx_us     = 0;
    s_master.tx_seq          = 0;
    s_master.tx_pkts         = 0;
    s_master.dropped_pkts    = 0;
    s_master.last_played_pts_us = 0;
    s_master.play_delta_us   = 0;
    s_master.silence_frames  = 0;
    s_master.start_sent      = false;
    s_master.last_heartbeat_us = 0;
    if (s_master.ingress != NULL)
    {
        /* 复位必须与 push_pcm 的"查空间 + 写"互斥：否则正在写的生产者会带着
         * 复位前的 xHead 缓存继续写，复位后 xHead/xTail 关系被破坏，环里可能
         * 出现几万个字节的陈旧数据被当成 PCM 播出去（听感为一段满量程噪声）。 */
        if (s_push_lock != NULL)
        {
            xSemaphoreTake(s_push_lock, portMAX_DELAY);
        }
        xStreamBufferReset(s_master.ingress);
        if (s_push_lock != NULL)
        {
            xSemaphoreGive(s_push_lock);
        }
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
    if (s_push_lock == NULL)
    {
        s_push_lock = xSemaphoreCreateMutex();
        if (s_push_lock == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_master.inited)
    {
        xSemaphoreGive(s_lock);
        SYNC_LOGW("already initialized, re-init ignored");
        return ESP_OK;
    }

    memset(&s_master, 0, sizeof(s_master));

    /* 单播目标从机表：cfg 里给了就用 cfg 的，否则用编译期默认列表
     * （见文件头的 SYNC_DEFAULT_SLAVE_IPS 说明） */
    if (cfg->slave_ips != NULL && cfg->slave_count > 0)
    {
        (void)sync_set_slaves_locked(cfg->slave_ips, cfg->slave_count);
    }
    else
    {
        (void)sync_set_slaves_locked(SYNC_DEFAULT_SLAVE_IPS,
                                     SYNC_DEFAULT_SLAVE_COUNT);
    }

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

    /* 本地延迟队列容量下限：必须装下 D + 接收余量（再加 2 帧余量）。
     * 主节点的出声时刻是 t_play + MARGIN，比"t_play + D 的时刻"还晚 60ms，
     * 少算这 60ms 队列会在稳态下溢，表现为周期性丢帧。 */
    need_pkts = (s_master.cfg_delay_ms + SYNC_RX_MARGIN_US / 1000
                 + s_master.cfg_frame_ms - 1) / s_master.cfg_frame_ms + 2;
    if (s_master.cfg_delay_pkts < need_pkts)
    {
        SYNC_LOGW("delay_queue_pkts %u < needed %u, clamp up",
                 (unsigned)s_master.cfg_delay_pkts, (unsigned)need_pkts);
        s_master.cfg_delay_pkts = need_pkts;
    }

    /* 缓冲分配（PSRAM 优先） */
    s_master.ingress_buf = sync_alloc_psram(s_master.cfg_ingress_bytes);
    s_master.delay_q     = sync_alloc_psram((size_t)s_master.cfg_delay_pkts *
                                            sizeof(sync_delay_entry_t));
    s_master.frame_buf   = sync_alloc_psram(SYNC_FRAME_MAX_SAMPLES * sizeof(int16_t));
    s_master.play_buf    = sync_alloc_psram(SYNC_FRAME_MAX_SAMPLES * 2 * sizeof(int16_t));
    s_master.tx_pkt      = sync_alloc_psram(SYNC_PKT_BUF_BYTES);
    if (s_master.ingress_buf == NULL || s_master.delay_q == NULL ||
        s_master.frame_buf == NULL || s_master.play_buf == NULL ||
        s_master.tx_pkt == NULL)
    {
        SYNC_LOGE("buffer alloc failed");
        sync_free_all_buffers();
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }
    /* trigger level 必须是 1（不能传整个环容量）：stream buffer 只有在
     * "已存字节数 >= trigger level" 时才通知等待中的接收方，而环里最多只能
     * 存 xLength-1 字节，传满容量等于**永不通知**——目前接收方都用超时 0
     * 轮询所以没暴露，但任何一次改成阻塞接收都会直接卡死。 */
    s_master.ingress = xStreamBufferCreateStatic(s_master.cfg_ingress_bytes,
                                                 1,
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

    BaseType_t tx_ok = xTaskCreatePinnedToCore(sync_tx_task, "SyncTx_Task",
                                               SYNC_TX_TASK_STACK, NULL, SYNC_TX_TASK_PRIO,
                                               &s_master.tx_task, SYNC_TX_TASK_CORE);
    BaseType_t play_ok = xTaskCreatePinnedToCore(sync_play_task, "SyncLocalPlay_Task",
                                                 SYNC_PLAY_TASK_STACK, NULL, SYNC_PLAY_TASK_PRIO,
                                                 &s_master.play_task, SYNC_PLAY_TASK_CORE);
    xSemaphoreGive(s_lock);

    /* 任一内部任务建不出来 = 同步播放不可用，必须回滚并上报失败，
     * 否则上层（MusicPlay）会误判 sync 可用。
     * s_lock 是普通互斥量（非递归），故先放锁再复用 deinit 收尾 */
    if (tx_ok != pdPASS || play_ok != pdPASS)
    {
        SYNC_LOGE("task create failed (tx=%d play=%d), rollback init",
                 (int)tx_ok, (int)play_ok);
        (void)sync_protocol_master_deinit();
        return ESP_ERR_NO_MEM;
    }

    SYNC_LOGI("init ok: D=%u ms frame=%u ms dq_pkts=%u ingress=%u B slaves=%u",
             s_master.cfg_delay_ms, s_master.cfg_frame_ms,
             s_master.cfg_delay_pkts, s_master.cfg_ingress_bytes,
             (unsigned)s_master.slave_count);
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
    SYNC_LOGI("deinit ok");
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
    SYNC_LOGI("start: stream=%u %luHz %uch", s_master.stream_id,
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
    SYNC_LOGI("stop");
    return ESP_OK;
}

esp_err_t sync_protocol_master_push_pcm(const int16_t *pcm, size_t samples)
{
    size_t   unit;
    size_t   bytes;
    size_t   total;
    size_t   off = 0;
    bool     playing;
    uint32_t gen;
    int64_t  deadline_us;

    if (!s_master.inited)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (pcm == NULL || samples == 0 || s_master.ingress == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    playing = (s_master.started && s_master.state == SYNC_STATE_PLAYING);
    unit    = (size_t)s_master.channels * sizeof(int16_t);
    gen     = s_master.stream_gen;
    xSemaphoreGive(s_lock);

    if (unit == 0)
    {
        unit = sizeof(int16_t);   /* 还没 start()：按单声道对齐即可 */
    }

    total = samples * sizeof(int16_t);
    /*
     * 入口环是**字节流**，没有消息边界。只要写进去的字节数不是通道整数倍
     * （比如立体声写进 3 字节），环里所有采样的边界就永久错位一格，之后
     * 消费端把相邻两字节拼成 int16，主节点和从节点都会持续播放满量程噪声，
     * 只能靠暂停→播放（sync_reset_stream → xStreamBufferReset）恢复。
     *
     * xStreamBufferSend() 带超时返回时**只写 xSpace 字节**（部分写入），而
     * 空闲空间 = xLength + xTail - xHead - 1：缓冲区长度是偶数、读写游标
     * 一直按整采样走，所以空闲空间恒为**奇数**——一旦走这条部分写入路径，
     * 就必然写进非整采样字节。因此这里绝不能依赖 xStreamBufferSend 的
     * 超时语义：先把空闲空间向下取整到整采样，再按整采样块发送。
     */
    bytes = total - (total % unit);
    if (bytes == 0)
    {
        return ESP_OK;
    }

    /* 播放中给上游 30ms 背压窗口：解码端允许跑在消费速率之前形成缓冲，
     * 被抢占/读卡慢时用缓冲垫掉，避免 I2S/从节点欠载（卡顿根因）；
     * 暂停/停止等非播放态保持"绝不阻塞调用方"语义，直接丢帧。 */
    deadline_us = esp_timer_get_time() + (playing ? 30 * 1000 : 0);

    while (off < bytes)
    {
        size_t sent  = 0;
        size_t space;
        size_t room;

        /* 换流/复位后旧通道的尾巴不能再混进新流。
         *
         * 这里**不能**用 portMAX_DELAY：本函数的调用方之一是 bt_audio_task，
         * 它一旦被永久堵在锁上就再也无法消费 A2DP FIFO，上游随即溢出，
         * 外部表现就是"放着放着突然卡死、串口全静、手机却还显示连着"。
         * 拿不到锁只说明有异常的长持锁者，放弃这一块（丢帧）比整条链路
         * 僵死好得多 —— 丢帧最多听一声咔，僵死是不可恢复的。 */
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) != pdTRUE)
        {
            s_master.dropped_pkts++;
            break;
        }
        bool stale = (s_master.stream_gen != gen);
        xSemaphoreGive(s_lock);
        if (stale)
        {
            break;
        }

        /* 生产者互斥：free-space 判定和写入必须是原子的，否则两个生产者
         * （本地解码 / 蓝牙）交错时仍可能写出非整采样字节。同样加超时。 */
        if (xSemaphoreTake(s_push_lock, pdMS_TO_TICKS(20)) != pdTRUE)
        {
            s_master.dropped_pkts++;
            break;
        }
        space = xStreamBufferSpacesAvailable(s_master.ingress);
        room  = space - (space % unit);
        if (room > 0)
        {
            size_t chunk = bytes - off;
            if (chunk > room)
            {
                chunk = room;
            }
            sent = xStreamBufferSend(s_master.ingress,
                                     (const uint8_t *)pcm + off, chunk, 0);
        }
        xSemaphoreGive(s_push_lock);

        if (sent == 0)
        {
            if (!playing || esp_timer_get_time() >= deadline_us)
            {
                break;   /* 背压超时/非播放态：丢弃剩余部分 */
            }
            vTaskDelay(1);
            continue;
        }
        off += sent;
    }

    if (off != total)
    {
        /* 背压超时/非播放态丢帧计数（上游过快或下游长时间不消费） */
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
    SYNC_LOGI("paused");
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
    SYNC_LOGI("resumed");
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
    SYNC_LOGI("delay_ms -> %u", s_master.cfg_delay_ms);
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

esp_err_t sync_protocol_master_set_slaves(const char *const *ips, uint8_t count)
{
    if (!s_master.inited)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (ips == NULL && count != 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t r;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    r = sync_set_slaves_locked(ips, count);
    SYNC_LOGI("slave list updated: %u target(s)", (unsigned)s_master.slave_count);
    xSemaphoreGive(s_lock);
    return r;
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
        SYNC_LOGI("broadcast cmd 0x%02X param %u", cmd, param);
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

esp_err_t sync_protocol_master_get_slaves(sync_slave_info_t *out,
                                          uint8_t capacity,
                                          uint8_t *count)
{
    int64_t now_us;
    uint8_t n;

    if (out == NULL || capacity == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_master.inited)
    {
        return ESP_ERR_INVALID_STATE;
    }

    now_us = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    n = s_master.slave_known;
    if (n > capacity)
    {
        n = capacity;
    }
    for (uint8_t i = 0; i < n; i++)
    {
        uint64_t age_us;
        out[i] = s_master.slaves[i].info;
        age_us = (uint64_t)now_us - s_master.slaves[i].last_seen_us;
        out[i].last_seen_ms = (age_us > (uint64_t)UINT32_MAX * 1000u)
                                  ? UINT32_MAX
                                  : (uint32_t)(age_us / 1000u);
    }
    xSemaphoreGive(s_lock);

    if (count != NULL)
    {
        *count = n;
    }
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