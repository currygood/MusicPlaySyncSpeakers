/**
 * @file sync_protocol.c
 * @brief 音频同步协议模块实现（从节点侧）：组播收 PCM → Jitter Buffer →
 *        PING/PONG 时钟同步 → 按主节点时间轴出队写 I2S
 *
 * 实现要点（对应《从音频节点软件架构分层设计》）：
 *   - SyncReceiver_Task：优先级 11 / 栈 8192 / Core 0。**唯一**持有音频组播
 *     通道（239.0.0.1:5678）FIFO 的任务；只做"收包 → 校验 → 入 Jitter Buffer"。
 *   - SyncHeartbeat_Task：优先级 8 / 栈 4096 / Core 0。**唯一**持有控制组播通道
 *     （239.0.0.1:5679）FIFO 的任务，同时负责发 PING。PONG / SYNC_START /
 *     SYNC_HEARTBEAT / 基础命令全部在这里串行解析 —— wifi_manager 的接收 FIFO
 *     是 NOSPLIT ringbuffer，多读者会把包随机分给不同任务，所以控制通道
 *     有且只有一个读者。
 *   - SyncPlayback_Task：优先级 10 / 栈 6144 / Core 1。按
 *     t_play_local = t_tx + D + θ 到点出队，写 amplifier。
 *
 * 铁律：**绝不在持有 s_lock 时调用 Amplifier_Play_Buffer()（最长阻塞 500ms）、
 *       wifi_manager_mcast_send()（最长阻塞 100ms）或 ESP_LOG*（串口阻塞）。
 *       锁内只做内存快照，动作一律放到锁外。**
 */

#include "sync_protocol.h"

#include "amplifier.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <inttypes.h>
#include <stddef.h>
#include <string.h>

/* ======================== 协议节奏常量 =========================================== */

/** 帧节奏：主节点每 15ms 发一帧，t_tx 落在这条 15ms 栅格上 */
#define SP_FRAME_MS                 (15)

/** 单帧最大 mono 采样数（帧长 661/662 交替，44.1k×15ms=661.5 非整数） */
#define SP_FRAME_SAMPLES_MAX        (662)

/** Jitter Buffer 默认槽数（24×15ms = 360ms，容得下 D=200ms + 抖动余量） */
#define SP_JB_SLOTS_DEF             (24)
#define SP_JB_SLOTS_MAX             (64)

/** 默认同步缓冲 D（ms），必须与主节点一致 */
#define SP_D_DEF_MS                 (200)

/* ======================== 时钟同步常量 =========================================== */

#define SP_PING_INTERVAL_DEF_MS     (1000)  /**< 稳态 PING 维护周期 */
#define SP_PING_BURST_COUNT         (10)    /**< 启动探测：连发次数 */
#define SP_PING_BURST_MS            (100)   /**< 启动探测：连发间隔 */
#define SP_PING_PENDING_MAX         (8)     /**< 未决 PING 环形表容量 */

/**
 * PONG 微秒级轮询步长。未决 PING 期间控制通道用忙等收紧到 200µs 一轮：
 * t4 的量化误差会以 1/2 权重直接变成 θ 的偏差（θ 误差 = t4 误差 / 2），
 * 而 FreeRTOS tick 是 10ms，单靠 vTaskDelay 根本压不下去。
 */
#define SP_PONG_SPIN_US             (200)
/** PONG 最长等待（超过就认为这个 PING 丢了，停止忙等） */
#define SP_PONG_WAIT_US             (30000)

/** 控制通道空闲时的阻塞等待（ms）；< 0 视为无效 */
#define SP_CTRL_IDLE_TIMEOUT_MS     (20)
/** 控制通道单次接收缓冲 */
#define SP_CTRL_RX_BUF              (256)
/** 控制通道发送超时 */
#define SP_CTRL_SEND_TIMEOUT_MS     (100)

/** 偏移样本滑动窗口；取其中 RTT 最小的若干个求中值 */
#define SP_CLOCK_WIN                (16)
#define SP_CLOCK_BEST               (5)
/** 累计接受这么多个样本后认为时钟已锁定 */
#define SP_CLOCK_LOCK_MIN           (5)
/** RTT 上限，超过认为该样本不可信（主节点轮询量化 + 网络抖动）
 *
 *  原值 50000（50ms）是按「同一局域网内往返应该只有几毫秒」估的。
 *  2026-10-18 实测主节点处理时间已降到 **4 µs**（pbuf 修复生效），但空口
 *  往返是 **137~192 ms**，于是每一个 PONG 都被这条门限拒掉，clock_accepted
 *  永远攒不到 5 个，lock 一直是 0。
 *
 *  先放宽到 300ms 让它能锁上、先听到声音，**这是权宜之计不是修复**：
 *  真实链路往返不该是百毫秒级，怀疑是 91.5 KB/s 的音频组播按 802.11
 *  基本速率发送，把空口占满、控制帧只能排队（见 wifi-multicast 相关笔记）。
 *  等确认能出声后，再回头查这一段延迟的来源。
 *
 *  ⚠️ 放宽门限的代价：θ 的精度取决于链路往返的**对称性**。RTT 越大、
 *  抖动越大（这里 137~192ms，抖动 55ms），θ 的偏差就越大 ——
 *  按一半估算，单向误差可能在 ±10~30ms 量级，会出现主从不同步（回声）。
 *  先用它换「能出声」，精度靠后续的漂移/相位环去修。 */
#define SP_CLOCK_RTT_MAX_US         (300000)
/** 单次滤波最多滑动多少（防止 θ 阶跃引起可闻跳变） */
#define SP_CLOCK_SLEW_MAX_US        (500)
/** 与当前 θ 偏离超过这个量级视为可疑 */
#define SP_CLOCK_JUMP_US            (50000)
/** 连续这么多次可疑 → 判定主节点重启/时钟阶跃，直接跳变接受 */
#define SP_CLOCK_JUMP_CNT           (3)

/* ======================== 播放常量 =============================================== */

/**
 * 写提前量 WA（µs）。**取 0 即时写**，与主节点 sync_play_task 的做法一致：
 * 弹性缓冲由软件 Jitter Buffer 承担，DMA 占用保持接近 0，于是
 * "写进去的时刻" 就等于 "出声的时刻"。若人为加大 WA，DMA 会恒定积压
 * WA 时长，所有帧统一晚 WA 出声 —— 这是等值常值滞后，不改善同步。
 */
#define SP_WRITE_AHEAD_US           (0)

/** 写 I2S 超时：写满等 DMA 时最长等 500ms */
#define SP_PLAY_WRITE_TIMEOUT_MS    (500)
/** 到点前最后这段时间忙等，保证出声精度（对齐主节点 sync_play_task） */
#define SP_PLAY_PRECISE_US          (2000)

/**
 * 重锚门限：读游标落后写游标这么**多槽**才重锚（重连/长抖动后追不回来的场景）。
 *
 * ⚠️ 这里绝不能用一个拍脑袋的小常数（曾经是 8）。sp_anchor_locked() 是把
 * 游标放在「最新包往前 D/frame_ms 帧」处 —— 也就是说**锚定动作本身**
 * 就把 lead 定成了 delay_ms/frame_ms（D=200、帧 15ms 时是 13 帧）。
 * 门限一旦比它小，sp_play_next_locked() 每次进来都会立刻判定「落后太多」
 * → 重锚 → return false，而重锚又把 lead 设回同一个值 ——
 * **死循环，一帧都出不了队**。串口上表现为 state=PLAYING、jb 满、
 * under=0ms、完全没有声音。
 *
 * 正确的门限是「缓冲装不下」：lead >= jb_slots 时新到的包会被
 * sp_in_window_locked() 判为超前而全部丢弃，此时才是真的追不回来了。
 *
 * 必须窄化回 int16_t 再比较：jb_slots 是 uint32_t，若让它参与比较，
 * lead 为负（读游标跑到写游标前面 = 真空载）时会被提升成无符号的巨大
 * 正数，把「该补静音」错判成「落后太多」。
 */
#define SP_REANCHOR_LEAD            ((int16_t)(s_slave.jb_slots))
/** 连续补静音超过这么多帧（33×15ms≈500ms）→ 回 PREPARING 重新灌缓冲 */
#define SP_UNDERRUN_RESYNC_FRAMES   (33)
/** 音频流静默判定（与《项目文档》一致：500ms 无包） */
#define SP_STREAM_TIMEOUT_MS        (500)

/** 状态日志周期 */
#define SP_STATUS_LOG_MS            (5000)

/* ======================== 漂移校正开关 =========================================== */

/**
 * v1 默认关闭漂移校正：只测量 sync_error_us 并打日志，不调 set_sample_rate。
 *
 * 原因见《从音频节点软件架构分层设计》与 amplifier.h 的 @warning：
 * i2s_bus_phy_set_sample_rate() 内部是 disable → reconfig → enable，会把
 * DMA 里尚未播放的音频全部丢弃（本工程 8×256 帧 ≈ 46ms），每次调速都伴随
 * 一次可闻空窗。先确认「WiFi → 时钟锁定 → 同步出声」这条基础链路通了，
 * 再把这个宏翻成 1。
 */
#define SP_DRIFT_ENABLE             (0)

#define SP_DRIFT_DEADBAND_US        (30000)  /**< 死区 ±30ms，以内不动 */
#define SP_DRIFT_CORRECT_SEC        (20)     /**< 期望用多少个调速周期把误差消掉 */
#define SP_DRIFT_MAX_PPM            (500)    /**< 限幅 ±0.5% */
#define SP_DRIFT_MIN_GAP_MS         (5000)   /**< 两次调速最小间隔 */
#define SP_DRIFT_MAX_STEP_HZ        (15)     /**< 单次最多改 15Hz */

/* ======================== Jitter Buffer 槽位 ===================================== */

/** 槽位状态 */
enum {
    SP_SLOT_EMPTY = 0,  /**< 空槽，可写 */
    SP_SLOT_READY = 1,  /**< 已填充（写侧最后才置位，读侧见 READY 即可安全读） */
};

typedef struct {
    uint8_t  state;
    uint16_t seq;
    uint16_t samples;      /**< 本帧 mono 采样数（661 或 662） */
    uint64_t t_tx_us;      /**< 主节点发送时刻（主节点时间轴），出队时现算 t_play */
    int16_t  pcm[SP_FRAME_SAMPLES_MAX];
} sp_jb_slot_t;            /* ≈1340B/槽 */

/* ======================== 模块状态 =============================================== */

typedef struct {
    /* ---- 配置（init 后只读，无需加锁） ---- */
    sync_protocol_slave_cfg_t cfg;
    bool     inited;

    /* ---- 生命周期 ---- */
    volatile bool started;
    volatile bool deinit_pending;
    TaskHandle_t  rx_task;
    TaskHandle_t  hb_task;
    TaskHandle_t  play_task;

    /* ---- 状态机 ---- */
    sync_protocol_state_t state;

    /* ---- 流标识 ---- */
    uint16_t stream_id;
    bool     stream_known;

    /* ---- 时钟估计 ---- */
    bool     offset_valid;
    int64_t  offset_us;         /**< θ = 从节点时钟 − 主节点时钟 */
    int64_t  net_delay_us;
    int64_t  last_rtt_us;
    uint32_t clock_accepted;    /**< 累计接受样本数 */
    bool     clock_locked;
    bool     lock_evt_sent;
    int      jump_cnt;
    int64_t  win_offset[SP_CLOCK_WIN];
    int64_t  win_rtt[SP_CLOCK_WIN];
    uint32_t win_cnt;           /**< 累计推入样本数（决定有效长度） */
    uint32_t win_idx;           /**< 下一个待写位置 */

    /* ---- 未决 PING（回环/多从机会收到别人的 PONG，必须靠 t1 回显匹配） ---- */
    uint64_t pend_t1[SP_PING_PENDING_MAX];
    bool     pend_used[SP_PING_PENDING_MAX];
    uint32_t pend_idx;

    /* ---- 流参数 ---- */
    uint32_t delay_ms;          /**< D */
    uint32_t frame_ms;          /**< 15 */

    /* ---- Jitter Buffer ---- */
    sp_jb_slot_t *jb;
    uint32_t jb_slots;
    uint16_t next_seq;          /**< 读游标 */
    uint64_t next_t_tx_us;      /**< 读游标对应的 t_tx（缺失帧靠 15ms 栅格推算） */
    bool     cursor_valid;
    uint16_t newest_seq;        /**< 已收到的最新 seq */
    uint64_t newest_t_tx_us;
    bool     has_newest;
    uint32_t filled_cnt;        /**< 当前有效槽数 */
    int64_t  last_rx_us;

    /* ---- 播放统计 ---- */
    int64_t  last_written_t_tx; /**< 最近写出的帧的 t_tx（漂移测量用） */
    bool     has_last_written;
    uint32_t rx_pkts;
    uint32_t ping_sent;         /**< 累计发出的 PING 数 */
    uint32_t pong_recv;         /**< 累计 t1 匹配成功的 PONG 数 */
    uint32_t lost_pkts;         /**< 确认丢包补静音的帧数 */
    uint32_t late_pkts;         /**< 迟到丢弃的包数 */
    uint32_t dropped_pkts;      /**< 写 I2S 失败丢弃的帧数 */
    uint32_t underrun_ms;       /**< 累计欠载静音时长 */
    uint32_t underrun_run;      /**< 当前连续欠载帧数 */

    /* ---- 漂移校正 ---- */
    int64_t  drift_error_us;
    uint32_t cur_sample_rate;
    int64_t  last_drift_adj_us;

    /* ---- 事件 ---- */
    sync_protocol_event_cb_t event_cb;
    void                    *event_ctx;

    /* ---- 日志 ---- */
    int64_t  last_status_log_us;
    int64_t  last_silent_warn_us;
} sp_slave_t;

static const char *TAG = "SP_SLAVE";

static sp_slave_t        s_slave;
static SemaphoreHandle_t s_lock = NULL;

/** 有 PING 未决（heartbeat 任务私有，不需要锁）：用来收紧控制通道轮询 */
static volatile bool s_ping_outstanding = false;
static int64_t       s_ping_deadline_us = 0;

/** 补静音用的全零 PCM（661/662 采样共用一份） */
static const int16_t s_silence[SP_FRAME_SAMPLES_MAX];

/* ======================== 小工具 ================================================= */

static inline int64_t sp_now_us(void)
{
    return esp_timer_get_time();
}

/** 主节点时间轴 → 本节点时间轴（全局只保留这一个换算，避免符号写反） */
static inline int64_t sp_master_to_local_locked(int64_t t_master_us)
{
    return t_master_us + s_slave.offset_us;
}

/** seq 之间的带符号距离（处理 16bit 回绕）：正 = a 比 b 新 */
static inline int16_t sp_seq_diff(uint16_t a, uint16_t b)
{
    return (int16_t)(a - b);
}

/**
 * @brief 睡到目标时刻；最后 SP_PLAY_PRECISE_US 内忙等
 *
 * FreeRTOS tick 是 10ms（CONFIG_FREERTOS_HZ=100），单靠 vTaskDelay 只能做到
 * 10ms 粒度，而同步目标是 ±15ms —— 所以必须留一段忙等把精度拉回来。
 */
static void sp_sleep_until(int64_t target_us)
{
    for (;;)
    {
        int64_t gap = target_us - sp_now_us();
        if (gap <= 0)
        {
            return;
        }
        if (gap <= SP_PLAY_PRECISE_US)
        {
            continue;   /* 忙等 */
        }
        vTaskDelay(pdMS_TO_TICKS((uint32_t)((gap - SP_PLAY_PRECISE_US) / 1000)));
    }
}

/* ======================== Jitter Buffer（调用方须持锁） ========================== */

/** 判断某个 seq 是否在当前可接收窗口内 */
static bool sp_in_window_locked(uint16_t seq)
{
    if (!s_slave.cursor_valid)
    {
        return true;    /* 尚未锚定读游标：全部收下，PREPARING 结束时再定起点 */
    }
    int16_t d = sp_seq_diff(seq, s_slave.next_seq);
    /* d < 0：比读游标旧 → 迟到；d >= 槽数：绕回来会覆盖未播帧 → 收不下 */
    return (d >= 0) && (d < (int16_t)s_slave.jb_slots);
}

/** 清空缓冲与读游标（换流 / RESET / stop 用） */
static void sp_jb_clear_locked(void)
{
    if (s_slave.jb != NULL)
    {
        memset(s_slave.jb, 0, sizeof(sp_jb_slot_t) * (size_t)s_slave.jb_slots);
    }
    s_slave.filled_cnt    = 0;
    s_slave.has_newest    = false;
    s_slave.cursor_valid  = false;
    s_slave.underrun_run  = 0;
    s_slave.has_last_written = false;
}

/**
 * @brief 把读游标锚定到「最新包往前 lead 帧」处
 *
 * 进入 PLAYING、以及落后过多需要重锚时调用。锚点选在 newest 之前
 * lead 帧，是为了让前面留出正好 D 时长的缓冲，后面留出抗抖动余量。
 */
static void sp_anchor_locked(void)
{
    uint32_t lead;

    if (!s_slave.has_newest)
    {
        s_slave.cursor_valid = false;
        return;
    }

    lead = s_slave.delay_ms / s_slave.frame_ms;       /* D 折成帧数 */
    if (lead >= s_slave.jb_slots)
    {
        lead = s_slave.jb_slots - 1;
    }

    uint64_t back = (uint64_t)lead * s_slave.frame_ms * 1000ull;

    s_slave.next_seq     = (uint16_t)(s_slave.newest_seq - lead);
    /* 主节点刚开机时 newest_t_tx_us 还不到 lead 帧的量，别让 uint64 下溢 */
    s_slave.next_t_tx_us = (s_slave.newest_t_tx_us > back) ? (s_slave.newest_t_tx_us - back)
                                                           : 0;
    s_slave.cursor_valid = true;
    s_slave.underrun_run = 0;
}

/**
 * @brief 记录音频包/控制帧里的 stream_id；变化时重建缓冲
 * @return true 表示 stream 发生了变化（调用方需在锁外补发事件/日志）
 * @note 锁内不打日志、不发事件。
 */
static bool sp_note_stream_locked(uint16_t sid)
{
    if (!s_slave.stream_known)
    {
        s_slave.stream_known = true;
        s_slave.stream_id    = sid;
        return false;
    }
    if (s_slave.stream_id == sid)
    {
        return false;
    }

    s_slave.stream_id = sid;
    sp_jb_clear_locked();
    if (s_slave.state == SYNC_STATE_PLAYING || s_slave.state == SYNC_STATE_PREPARING)
    {
        s_slave.state = SYNC_STATE_PREPARING;   /* 重新灌 D 缓冲 */
    }
    return true;
}

/* ======================== 状态快照 / 事件 ========================================= */

/** 取状态快照（自行加锁；调用方不得持锁） */
static void sp_snapshot(sync_protocol_slave_status_t *st)
{
    memset(st, 0, sizeof(*st));

    xSemaphoreTake(s_lock, portMAX_DELAY);
    st->state           = s_slave.state;
    st->stream_id       = s_slave.stream_id;
    st->sample_rate     = AMPLIFIER_SAMPLE_RATE;
    st->clock_locked    = s_slave.clock_locked;
    st->offset_us       = s_slave.offset_us;
    st->net_delay_us    = s_slave.net_delay_us;
    st->sync_error_us   = s_slave.drift_error_us;
    st->cur_sample_rate = s_slave.cur_sample_rate;
    st->rx_pkts         = s_slave.rx_pkts;
    st->ping_sent       = s_slave.ping_sent;
    st->pong_recv       = s_slave.pong_recv;
    st->lost_pkts       = s_slave.lost_pkts;
    st->late_pkts       = s_slave.late_pkts;
    st->underrun_ms     = s_slave.underrun_ms;
    st->jitter_used     = s_slave.filled_cnt;
    st->jitter_cap      = s_slave.jb_slots;
    xSemaphoreGive(s_lock);
}

/** 事件回调（自行加锁取快照；调用方**不得**持锁） */
static void sp_emit_event(sync_protocol_event_id_t id)
{
    sync_protocol_event_t evt;

    if (s_slave.event_cb == NULL)
    {
        return;
    }
    evt.id = id;
    sp_snapshot(&evt.status);
    s_slave.event_cb(&evt, s_slave.event_ctx);
}

/** 状态机迁移 + 事件（调用方不得持锁） */
static void sp_set_state(sync_protocol_state_t next)
{
    bool changed = false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_slave.state != next)
    {
        s_slave.state = next;
        changed = true;
    }
    xSemaphoreGive(s_lock);

    if (changed)
    {
        ESP_LOGI(TAG, "state -> %d", (int)next);
        sp_emit_event(SYNC_EVT_STATE_CHANGED);
    }
}

/* ======================== 时钟同步 =============================================== */

/**
 * @brief 从窗口里挑出 RTT 最小的若干个样本，取其中值作为 θ 目标
 *
 * 为什么取最小 RTT：RTT 里唯一的可观测量就是排队/调度延迟，而它**只会让
 * 样本偏大**（主节点的 PONG 生成被它自己 15ms 的发送循环量化）。
 * 所以最小 RTT 的样本最接近真实偏移。
 */
static int64_t sp_clock_pick_target_locked(bool *ok)
{
    int64_t best_off[SP_CLOCK_BEST];
    int64_t best_rtt[SP_CLOCK_BEST];
    uint32_t valid = (s_slave.win_cnt < SP_CLOCK_WIN) ? s_slave.win_cnt : SP_CLOCK_WIN;
    uint32_t picked = 0;

    for (uint32_t i = 0; i < SP_CLOCK_BEST; i++)
    {
        best_rtt[i] = INT64_MAX;
        best_off[i] = 0;
    }

    for (uint32_t i = 0; i < valid; i++)
    {
        /* 从最老的样本开始遍历环形窗口 */
        uint32_t idx = (s_slave.win_idx + SP_CLOCK_WIN - valid + i) % SP_CLOCK_WIN;
        int64_t  rtt = s_slave.win_rtt[idx];
        int64_t  off = s_slave.win_offset[idx];

        /* 插入排序：维持 best_rtt 升序，挤掉当前最大的那个 */
        if (picked < SP_CLOCK_BEST || rtt < best_rtt[SP_CLOCK_BEST - 1])
        {
            uint32_t j = (picked < SP_CLOCK_BEST) ? picked : (SP_CLOCK_BEST - 1);
            while (j > 0 && best_rtt[j - 1] > rtt)
            {
                best_rtt[j] = best_rtt[j - 1];
                best_off[j] = best_off[j - 1];
                j--;
            }
            best_rtt[j] = rtt;
            best_off[j] = off;
            if (picked < SP_CLOCK_BEST)
            {
                picked++;
            }
        }
    }

    if (picked == 0)
    {
        *ok = false;
        return 0;
    }

    /* 5 个元素的中值：简单插入排序后取中间 */
    for (uint32_t i = 1; i < picked; i++)
    {
        int64_t o = best_off[i];
        uint32_t j = i;
        while (j > 0 && best_off[j - 1] > o)
        {
            best_off[j] = best_off[j - 1];
            j--;
        }
        best_off[j] = o;
    }

    *ok = true;
    return best_off[picked / 2];
}

/**
 * @brief 喂入一个 PONG 样本（heartbeat 任务上下文，不得持锁）
 *
 * 三级处理：样本入窗 → 择优取中值 → 限速滑动逼近。
 * 限速的原因：θ 一变，所有未播帧的 t_play_local 一起平移，阶跃 = 一次可闻
 * 跳变；500µs/次 @1Hz ≈ 500ppm，人耳无感。
 */
static void sp_clock_feed(int64_t offset_us, int64_t rtt_us)
{
    int64_t target = 0;
    int64_t net_delay = rtt_us / 2;
    int64_t offset_before = 0;
    bool    ok = false;
    bool    locked_now = false;
    bool    jumped = false;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    offset_before = s_slave.offset_us;

    s_slave.win_offset[s_slave.win_idx] = offset_us;
    s_slave.win_rtt[s_slave.win_idx]    = rtt_us;
    s_slave.win_idx                     = (s_slave.win_idx + 1) % SP_CLOCK_WIN;
    s_slave.win_cnt++;
    s_slave.clock_accepted++;
    s_slave.last_rtt_us   = rtt_us;
    s_slave.net_delay_us  = net_delay;

    target = sp_clock_pick_target_locked(&ok);
    if (!ok)
    {
        xSemaphoreGive(s_lock);
        return;
    }

    if (!s_slave.offset_valid)
    {
        s_slave.offset_us      = target;    /* 首次：直接建立基准 */
        s_slave.offset_valid   = true;
        s_slave.jump_cnt       = 0;
    }
    else
    {
        int64_t d = target - s_slave.offset_us;

        if (d > SP_CLOCK_JUMP_US || d < -SP_CLOCK_JUMP_US)
        {
            /* 与当前 θ 差了一个数量级：先怀疑是脏样本 */
            s_slave.jump_cnt++;
            if (s_slave.jump_cnt >= SP_CLOCK_JUMP_CNT)
            {
                /* 连续多次都这样 → 主节点重启/时钟阶跃，接受并重新收敛 */
                s_slave.offset_us = target;
                s_slave.jump_cnt  = 0;
                jumped            = true;
            }
            d = 0;      /* 可疑样本本轮不滑动 */
        }
        else
        {
            s_slave.jump_cnt = 0;
            if (d > SP_CLOCK_SLEW_MAX_US)
            {
                d = SP_CLOCK_SLEW_MAX_US;
            }
            else if (d < -SP_CLOCK_SLEW_MAX_US)
            {
                d = -SP_CLOCK_SLEW_MAX_US;
            }
        }
        s_slave.offset_us += d;
    }

    if (!s_slave.clock_locked && s_slave.clock_accepted >= SP_CLOCK_LOCK_MIN)
    {
        s_slave.clock_locked = true;
        locked_now = true;
    }

    int64_t theta = s_slave.offset_us;
    int64_t step  = theta - offset_before;
    xSemaphoreGive(s_lock);

    if (jumped)
    {
        ESP_LOGW(TAG, "clock step accepted: offset=%+" PRId64 "us (step=%+" PRId64 "us)",
                 theta, step);
    }
    if (locked_now)
    {
        ESP_LOGI(TAG, "clock locked: offset=%+" PRId64 "us net_delay=%" PRId64 "us rtt=%" PRId64 "us",
                 (int64_t)theta, net_delay, rtt_us);
        sp_emit_event(SYNC_EVT_CLOCK_LOCKED);
    }
}

/** 撤销一个未决 PING 的登记（调用方不得持锁） */
static void sp_pending_drop(uint64_t t1)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < SP_PING_PENDING_MAX; i++)
    {
        if (s_slave.pend_used[i] && s_slave.pend_t1[i] == t1)
        {
            s_slave.pend_used[i] = false;
            break;
        }
    }
    xSemaphoreGive(s_lock);
}

/** 清理超时未决的 PING（heartbeat 任务上下文） */
static void sp_pending_expire(void)
{
    int64_t now = sp_now_us();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < SP_PING_PENDING_MAX; i++)
    {
        if (s_slave.pend_used[i] &&
            (now - (int64_t)s_slave.pend_t1[i]) > (int64_t)SP_PONG_WAIT_US * 10)
        {
            s_slave.pend_used[i] = false;
        }
    }
    xSemaphoreGive(s_lock);
}

/* ======================== 控制通道：发送 ========================================= */

/**
 * @brief 发一个 PING（heartbeat 任务上下文，不得持锁）
 *
 * 这就是"从节点 → 主节点"这个方向上唯一的报文；主节点收到后回 PONG。
 * 注意组播回环是开着的（wifi_manager 里 loop = rx_enable），所以本机也会
 * 收到自己发的这个 PING，控制通道处理里直接忽略 type == PING 即可。
 */
static void sp_send_ping(void)
{
    uint8_t pkt[sizeof(sync_ctrl_ext_t) + sizeof(sync_ping_payload_t)];
    sync_ctrl_ext_t     *ext = (sync_ctrl_ext_t *)pkt;
    sync_ping_payload_t  pl;
    uint64_t             t1 = (uint64_t)sp_now_us();
    esp_err_t            ret;

    pl.t1_us   = t1;
    ext->magic = SYNC_MAGIC;
    ext->type  = SYNC_MSG_PING;
    ext->len   = (uint8_t)sizeof(pl);
    memcpy(ext->data, &pl, sizeof(pl));

    /* 先登记再发：PONG 的 t1 回显靠这张表匹配 */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_slave.pend_t1[s_slave.pend_idx]   = t1;
    s_slave.pend_used[s_slave.pend_idx] = true;
    s_slave.pend_idx = (s_slave.pend_idx + 1) % SP_PING_PENDING_MAX;
    xSemaphoreGive(s_lock);

    ret = wifi_manager_mcast_send(s_slave.cfg.control_chan, pkt, sizeof(pkt),
                                  SP_CTRL_SEND_TIMEOUT_MS);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "PING send failed: %s", esp_err_to_name(ret));
        sp_pending_drop(t1);
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_slave.ping_sent++;
    xSemaphoreGive(s_lock);

    s_ping_outstanding = true;
    s_ping_deadline_us = sp_now_us() + SP_PONG_WAIT_US;
}

/* ======================== 控制通道：接收处理 ===================================== */

/** 处理 PONG：匹配 → 剔除 → 喂窗口 */
static void sp_handle_pong(const sync_pong_payload_t *pong)
{
    int64_t t4  = sp_now_us();
    int64_t rtt;
    int64_t off;
    bool    matched = false;
    uint64_t pend_new = 0;      /* 诊断用：未决表里最新的 t1 */
    int      pend_n   = 0;      /* 诊断用：未决表里还有几个 */

    /* 诊断计数：只要 PONG 上了线就计，不管是回给谁的。
     * 和 ping_sent 对照就能一眼分开两类故障：
     *   ping 在涨、pong 恒 0 → PONG 根本没回来（主节点没收到 PING / 组播被过滤）
     *   ping 和 pong 一起涨、lock 仍为 0 → PONG 回来了但被下面的过滤条件丢了 */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_slave.pong_recv++;
    xSemaphoreGive(s_lock);

    /* 回显匹配：组播回环会让我们收到自己的 PING，多从机场景下还会收到
     * 别人的 PONG —— 只有 t1 与某个未决 PING 完全相等的才是回给自己的 */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < SP_PING_PENDING_MAX; i++)
    {
        if (s_slave.pend_used[i] && s_slave.pend_t1[i] == pong->t1_us)
        {
            s_slave.pend_used[i] = false;
            matched = true;
            break;
        }
        /* 只有走到「没匹配上」这条支路时这两个值才被打印出去。
         * matched 时提前 break，统计不全也无所谓。 */
        if (s_slave.pend_used[i])
        {
            pend_n++;
            if (s_slave.pend_t1[i] > pend_new)
            {
                pend_new = s_slave.pend_t1[i];
            }
        }
    }
    xSemaphoreGive(s_lock);

    if (!matched)
    {
        /* 升到 WARN：这条以前是 LOGD，默认级别下完全看不见，
         * 串口上只剩一个「lock=0」的结论，查不出原因。
         * 带上未决表的状态，好一眼分开两种故障：
         *   未决 0 个      → PONG 回来得太晚，登记已经过期/被顶掉
         *   未决有、t1 都对不上 → PONG 是回给别人的（多从机/串了别的板子） */
        ESP_LOGW(TAG, "PONG t1=%" PRIu64 " 无匹配未决 PING，丢弃（未决 %d 个，最新 t1=%" PRIu64 "）",
                 pong->t1_us, pend_n, pend_new);
        return;
    }
    s_ping_outstanding = false;

    /* RTT  = (t4 - t1) - (t3 - t2)：剔除主节点的处理时间
     * θ    = ((t1 - t2) + (t4 - t3)) / 2 = 从节点时钟 − 主节点时钟 */
    rtt = (t4 - (int64_t)pong->t1_us) -
          ((int64_t)pong->t3_us - (int64_t)pong->t2_us);
    off = (((int64_t)pong->t1_us - (int64_t)pong->t2_us) +
           (t4 - (int64_t)pong->t3_us)) / 2;

    if (rtt < 0 || rtt > SP_CLOCK_RTT_MAX_US)
    {
        /* 同样升到 WARN，并把四个时间戳原样打出来。rtt 为负几乎必然是
         * 两边时间基不同尺（或 t2/t3 取反了），看数字就能直接断定。 */
        ESP_LOGW(TAG, "PONG rtt=%" PRId64 "us 超范围，丢弃"
                      "（t1=%" PRIu64 " t2=%" PRIu64 " t3=%" PRIu64 " t4=%" PRId64 "）",
                 rtt, pong->t1_us, pong->t2_us, pong->t3_us, t4);
        return;
    }

    ESP_LOGD(TAG, "PONG t1=%" PRId64 " t2=%" PRId64 " t3=%" PRId64 " t4=%" PRId64
                  " rtt=%" PRId64 " off=%" PRId64,
             (int64_t)pong->t1_us, (int64_t)pong->t2_us, (int64_t)pong->t3_us,
             t4, rtt, off);

    sp_clock_feed(off, rtt);
}

/** 处理 SYNC_START */
static void sp_handle_start(const sync_start_payload_t *st)
{
    bool changed;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    changed = sp_note_stream_locked(st->stream_id);
    if (st->d_ms > 0 && st->d_ms <= 5000)
    {
        s_slave.delay_ms = st->d_ms;    /* 以主节点下发的 D 为准 */
    }
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "SYNC_START: stream=%u t0=%" PRIu64 "us D=%" PRIu32 "ms",
             st->stream_id, st->t0_us, st->d_ms);
    if (changed)
    {
        sp_emit_event(SYNC_EVT_STREAM_CHANGED);
    }
}

/** 处理 SYNC_HEARTBEAT：唯一作用是拿主节点的写游标做漂移测量 */
static void sp_handle_heartbeat(const sync_heartbeat_payload_t *hb)
{
    int64_t err;
    int64_t last_written;
    bool    usable;
    uint16_t sid = hb->stream_id;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    usable = s_slave.has_last_written &&
             s_slave.state == SYNC_STATE_PLAYING &&
             s_slave.stream_known && sid == s_slave.stream_id;
    last_written = s_slave.last_written_t_tx;
    xSemaphoreGive(s_lock);

    if (!usable)
    {
        return;
    }

    /* 两边都是"最近出队帧的 t_tx"，口径天然一致（都是主节点时间轴），
     * 所以差值就是位置误差：正 = 从节点领先，需要放慢。 */
    err = last_written - (int64_t)hb->master_pts_us;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_slave.drift_error_us = err;
    xSemaphoreGive(s_lock);

#if SP_DRIFT_ENABLE
    {
        int64_t  now = sp_now_us();
        bool     do_adj;
        uint32_t cur_hz;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        cur_hz = s_slave.cur_sample_rate;
        do_adj = ((now - s_slave.last_drift_adj_us) >= (int64_t)SP_DRIFT_MIN_GAP_MS * 1000) &&
                 (err > SP_DRIFT_DEADBAND_US || err < -SP_DRIFT_DEADBAND_US);
        if (do_adj)
        {
            s_slave.last_drift_adj_us = now;
        }
        xSemaphoreGive(s_lock);

        if (do_adj)
        {
            /* 1ppm 跑 1 秒 = 1µs 位置差。要在 SP_DRIFT_CORRECT_SEC 秒内把 err µs
             * 消掉，速率偏移就是 -err / SP_DRIFT_CORRECT_SEC（ppm）。
             * 正 err = 从机领先 → 降速（负 ppm）。 */
            int64_t  ppm = -(err / SP_DRIFT_CORRECT_SEC);
            uint32_t target_hz;

            if (ppm > SP_DRIFT_MAX_PPM)
            {
                ppm = SP_DRIFT_MAX_PPM;
            }
            else if (ppm < -SP_DRIFT_MAX_PPM)
            {
                ppm = -SP_DRIFT_MAX_PPM;
            }

            target_hz = (uint32_t)((int64_t)AMPLIFIER_SAMPLE_RATE +
                                   ((int64_t)AMPLIFIER_SAMPLE_RATE * ppm) / 1000000);

            /* 单次步长限制：与当前速率之差不超过 MAX_STEP_HZ */
            if (target_hz > cur_hz + SP_DRIFT_MAX_STEP_HZ)
            {
                target_hz = cur_hz + SP_DRIFT_MAX_STEP_HZ;
            }
            else if (target_hz + SP_DRIFT_MAX_STEP_HZ < cur_hz)
            {
                target_hz = cur_hz - SP_DRIFT_MAX_STEP_HZ;
            }

            if (target_hz != cur_hz)
            {
                ESP_LOGW(TAG, "drift adj: err=%+" PRId64 "us %luHz -> %luHz (代价：丢一次 DMA 空洞)",
                         err, (unsigned long)cur_hz, (unsigned long)target_hz);
                if (Amplifier_Set_Sample_Rate(target_hz) == ESP_OK)
                {
                    xSemaphoreTake(s_lock, portMAX_DELAY);
                    s_slave.cur_sample_rate = target_hz;
                    xSemaphoreGive(s_lock);
                }
                /* 失败就不改 cur_hz：硬件速率实际没变，状态须与硬件一致 */
            }
        }
    }
#else
    ESP_LOGD(TAG, "[drift] err=%+" PRId64 "us (校正未启用，仅测量)", err);
#endif
}

/** 处理基础控制命令（cmd 0x01~0x06） */
static void sp_handle_basic_cmd(uint8_t cmd, uint8_t param)
{
    switch (cmd)
    {
    case SYNC_CMD_PLAY:
        xSemaphoreTake(s_lock, portMAX_DELAY);
        /* 恢复出队：重锚游标，避免把暂停期间积压的旧帧一股脑倒出来 */
        sp_anchor_locked();
        /* 只有「游标锚得住」且「时钟已锁」才直接进 PLAYING；否则一律回
         * PREPARING，交给播放任务那道门（clock_locked && 缓冲 ≥ D）再放行。
         * 少了 clock_locked 这一条，时钟没锁时 state 也会变成 PLAYING，
         * 而 θ=0 会把出声时刻算成几十分钟之后 ——
         * 串口上就是「state=2 lock=0，而且没声音」。
         * 缓冲是空的（刚换流/刚断过）同样回 PREPARING 重新灌 D，别空转补静音 */
        s_slave.state = (s_slave.cursor_valid && s_slave.clock_locked)
                            ? SYNC_STATE_PLAYING
                            : SYNC_STATE_PREPARING;
        sync_protocol_state_t next = s_slave.state;
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "cmd PLAY -> state %d", (int)next);
        break;

    case SYNC_CMD_PAUSE:
        sp_set_state(SYNC_STATE_PAUSED);
        ESP_LOGI(TAG, "cmd PAUSE");
        break;

    case SYNC_CMD_NEXT:
    case SYNC_CMD_PREV:
        /* 从节点没有本地曲库（MusicPlay 是空壳），只能清缓冲等新流 */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        sp_jb_clear_locked();
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "cmd %s: buffer flushed, waiting for new stream",
                 (cmd == SYNC_CMD_NEXT) ? "NEXT" : "PREV");
        break;

    case SYNC_CMD_VOLUME:
        /* NS4168 无硬件增益，只有软件音量；100% 直通、0% 静音 */
        Amplifier_Set_Volume(param);
        ESP_LOGI(TAG, "cmd VOLUME -> %u%%", param);
        break;

    case SYNC_CMD_RESET:
        xSemaphoreTake(s_lock, portMAX_DELAY);
        sp_jb_clear_locked();
        s_slave.stream_known  = false;
        s_slave.offset_valid  = false;
        s_slave.clock_locked  = false;
        s_slave.clock_accepted = 0;
        s_slave.win_cnt       = 0;
        s_slave.win_idx       = 0;
        s_slave.jump_cnt      = 0;
        /* 缓冲已清空、时钟估计已作废，必须退回 PREPARING 重新走一遍
         * 「锁钟 + 灌 D」的准入流程；停在 PLAYING 的话游标锚不回来，
         * 播放任务会永远空转，再也出不了声。 */
        s_slave.state         = SYNC_STATE_PREPARING;
        xSemaphoreGive(s_lock);
        s_ping_outstanding = false;
        ESP_LOGW(TAG, "cmd RESET: buffer/clock estimate cleared, re-probing");
        break;

    default:
        ESP_LOGW(TAG, "unknown basic cmd 0x%02X", cmd);
        break;
    }
}

/**
 * 控制通道各 type/cmd 的累计接收计数（诊断用）
 *
 * 全部控制报文都由 SyncHeartbeat_Task 串行解析，所以这里不需要加锁。
 * 作用是把「主节点→从节点」这个方向按报文类型摊开：只要 PING/PONG 之外
 * 还有 START/HEARTBEAT 在涨，就说明组播通路本身是通的，问题只出在
 * PING→PONG 这一问一答上。
 */
static uint32_t s_ctrl_rx_cnt[256];

/** 解析并派发一个控制通道报文 */
static void sp_dispatch_ctrl(const uint8_t *buf, size_t len)
{
    if (len < sizeof(sync_ctrl_basic_t))
    {
        return;
    }

    uint16_t magic;
    memcpy(&magic, buf, sizeof(magic));
    if (magic != SYNC_MAGIC)
    {
        return;
    }

    s_ctrl_rx_cnt[buf[2]]++;

    /* 基础帧与扩展帧都从 {u16 magic; u8 x; ...} 开始，靠 x 的取值区分：
     * 基础帧 x = cmd ∈ [0x01, 0x06]，扩展帧 x = type ∈ {0x12,0x13,0x20,0x21}，
     * 两个值域不重叠。 */
    uint8_t kind = buf[2];

    if (kind >= SYNC_CMD_PLAY && kind <= SYNC_CMD_RESET)
    {
        sp_handle_basic_cmd(kind, buf[3]);
        return;
    }

    if (len < sizeof(sync_ctrl_ext_t))
    {
        return;
    }

    switch (kind)
    {
    case SYNC_MSG_PING:
        /* 组播回环：这是我们自己刚发出去的 PING。从节点不应答 PING。 */
        break;

    case SYNC_MSG_PONG:
        if (len >= sizeof(sync_ctrl_ext_t) + sizeof(sync_pong_payload_t))
        {
            sync_pong_payload_t pl;
            memcpy(&pl, buf + sizeof(sync_ctrl_ext_t), sizeof(pl));
            sp_handle_pong(&pl);
        }
        break;

    case SYNC_MSG_START:
        if (len >= sizeof(sync_ctrl_ext_t) + sizeof(sync_start_payload_t))
        {
            sync_start_payload_t pl;
            memcpy(&pl, buf + sizeof(sync_ctrl_ext_t), sizeof(pl));
            sp_handle_start(&pl);
        }
        break;

    case SYNC_MSG_HEARTBEAT:
        if (len >= sizeof(sync_ctrl_ext_t) + sizeof(sync_heartbeat_payload_t))
        {
            sync_heartbeat_payload_t pl;
            memcpy(&pl, buf + sizeof(sync_ctrl_ext_t), sizeof(pl));
            sp_handle_heartbeat(&pl);
        }
        break;

    default:
        ESP_LOGD(TAG, "unknown ctrl type 0x%02X", kind);
        break;
    }
}

/** 非阻塞排空控制通道（返回本轮处理到的报文数） */
static int sp_ctrl_drain(void)
{
    uint8_t buf[SP_CTRL_RX_BUF];
    int     n = 0;

    for (;;)
    {
        size_t    len = 0;
        /* 注意 wifi_manager_mcast_recv 的超时约定与直觉相反：
         *   timeout_ms == 0 → portMAX_DELAY（永久阻塞）
         *   timeout_ms == 1 → pdMS_TO_TICKS(1) == 0 tick（非阻塞）
         * 所以这里必须传 1，绝不能传 0。 */
        esp_err_t ret = wifi_manager_mcast_recv(s_slave.cfg.control_chan,
                                                buf, sizeof(buf), &len, 1);
        if (ret != ESP_OK || len == 0)
        {
            break;
        }
        sp_dispatch_ctrl(buf, len);
        if (++n >= 16)
        {
            break;      /* 单轮限流，避免饿死本任务其它工作 */
        }
    }
    return n;
}

/* ======================== 任务：接收 ============================================= */

/** 音频包入 Jitter Buffer */
static void sp_on_audio_pkt(const uint8_t *buf, size_t len)
{
    sync_audio_pkt_hdr_t hdr;
    uint16_t samples;
    bool     stream_changed;
    uint32_t idx;
    const uint8_t *payload;

    if (len < SYNC_AUDIO_HDR_SIZE)
    {
        return;
    }
    memcpy(&hdr, buf, sizeof(hdr));
    if (hdr.magic != SYNC_MAGIC)
    {
        return;
    }
    if (hdr.data_len == 0 || hdr.data_len > (len - SYNC_AUDIO_HDR_SIZE))
    {
        return;
    }
    if (hdr.data_len & 1u)
    {
        return;     /* 16bit 采样，字节数必须是偶数 */
    }
    samples = (uint16_t)(hdr.data_len / 2);
    if (samples > SP_FRAME_SAMPLES_MAX)
    {
        return;
    }
    payload = buf + SYNC_AUDIO_HDR_SIZE;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    stream_changed = sp_note_stream_locked(hdr.stream_id);

    s_slave.rx_pkts++;
    s_slave.last_rx_us = sp_now_us();

    if (!s_slave.inited || s_slave.jb == NULL)
    {
        xSemaphoreGive(s_lock);
        return;
    }

    if (!sp_in_window_locked(hdr.seq))
    {
        /* 两种丢法：比读游标旧（迟到），或超前到绕回来会覆盖未播帧 */
        bool far_ahead = s_slave.cursor_valid &&
                         (sp_seq_diff(hdr.seq, s_slave.next_seq) >=
                          (int16_t)s_slave.jb_slots);
        s_slave.late_pkts++;
        xSemaphoreGive(s_lock);
        if (far_ahead)
        {
            ESP_LOGD(TAG, "seq=%u 超前超出缓冲窗口，丢弃", hdr.seq);
        }
        return;
    }

    idx = (uint32_t)(hdr.seq % s_slave.jb_slots);
    sp_jb_slot_t *slot = &s_slave.jb[idx];

    if (slot->state == SP_SLOT_EMPTY)
    {
        s_slave.filled_cnt++;
    }
    /* 先失效、后填数据、最后置 READY：读侧只要看到 READY 就能安全读 */
    slot->state = SP_SLOT_EMPTY;
    slot->seq    = hdr.seq;
    slot->samples = samples;
    slot->t_tx_us = hdr.timestamp_us;
    memcpy(slot->pcm, payload, hdr.data_len);
    slot->state = SP_SLOT_READY;

    if (!s_slave.has_newest || sp_seq_diff(hdr.seq, s_slave.newest_seq) > 0)
    {
        s_slave.newest_seq    = hdr.seq;
        s_slave.newest_t_tx_us = hdr.timestamp_us;
        s_slave.has_newest    = true;
    }

    xSemaphoreGive(s_lock);

    if (stream_changed)
    {
        sp_emit_event(SYNC_EVT_STREAM_CHANGED);
    }
}

/**
 * @brief SyncReceiver_Task：音频组播通道的唯一读者
 *
 * 只做"收 → 校验 → 入缓冲"，任何耗时动作（打日志、写 I2S）都不在这里。
 */
static void sp_rx_task(void *arg)
{
    static uint8_t buf[1600];   /* UDP 上限 1472，留余量；static 避免吃任务栈 */

    (void)arg;
    ESP_LOGI(TAG, "SyncReceiver_Task start (prio 11, core %d)", xPortGetCoreID());

    for (;;)
    {
        if (s_slave.deinit_pending)
        {
            break;
        }
        if (!s_slave.started)
        {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        size_t    len = 0;
        esp_err_t ret = wifi_manager_mcast_recv(s_slave.cfg.audio_chan,
                                                buf, sizeof(buf), &len, 100);
        if (ret == ESP_OK && len > 0)
        {
            sp_on_audio_pkt(buf, len);
        }
        /* 超时返回属正常（网络静默），回到循环顶再判一次退出标志 */
    }

    ESP_LOGI(TAG, "SyncReceiver_Task exit");
    s_slave.rx_task = NULL;
    vTaskDelete(NULL);
}

/* ======================== 任务：心跳 / 控制 ====================================== */

/** 周期性状态日志（heartbeat 任务上下文） */
static void sp_maybe_log_status(void)
{
    int64_t now = sp_now_us();
    bool    due;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    due = (now - s_slave.last_status_log_us) >= (int64_t)SP_STATUS_LOG_MS * 1000;
    if (due)
    {
        s_slave.last_status_log_us = now;
    }
    xSemaphoreGive(s_lock);
    if (!due)
    {
        return;
    }

    sync_protocol_slave_status_t st;
    sp_snapshot(&st);

    ESP_LOGI(TAG,
             "[status] state=%d stream=%u lock=%d rx=%lu ping=%lu pong=%lu "
             "lost=%lu late=%lu under=%lums "
             "jb=%lu/%lu offset=%+" PRId64 "us nd=%" PRId64 "us err=%+" PRId64 "us rate=%lu",
             (int)st.state, st.stream_id, (int)st.clock_locked,
             (unsigned long)st.rx_pkts, (unsigned long)st.ping_sent,
             (unsigned long)st.pong_recv,
             (unsigned long)st.lost_pkts, (unsigned long)st.late_pkts,
             (unsigned long)st.underrun_ms,
             (unsigned long)st.jitter_used, (unsigned long)st.jitter_cap,
             st.offset_us, st.net_delay_us, st.sync_error_us,
             (unsigned long)st.cur_sample_rate);

    /* 诊断：控制通道按报文类型的累计收包数。
     *   PING 在涨 = 收到了自己发出去的回环副本（组播回环是开着的）
     *   START / HB 在涨 = 主节点→从节点方向通着
     *   PONG 在涨而 lock 恒 0 = PONG 回来了但没被采纳，看上面两条 WARN */
    ESP_LOGI(TAG, "[ctrl] PING=%lu PONG=%lu START=%lu HB=%lu PLAY=%lu PAUSE=%lu RESET=%lu",
             (unsigned long)s_ctrl_rx_cnt[SYNC_MSG_PING],
             (unsigned long)s_ctrl_rx_cnt[SYNC_MSG_PONG],
             (unsigned long)s_ctrl_rx_cnt[SYNC_MSG_START],
             (unsigned long)s_ctrl_rx_cnt[SYNC_MSG_HEARTBEAT],
             (unsigned long)s_ctrl_rx_cnt[SYNC_CMD_PLAY],
             (unsigned long)s_ctrl_rx_cnt[SYNC_CMD_PAUSE],
             (unsigned long)s_ctrl_rx_cnt[SYNC_CMD_RESET]);
}

/** 长时间收不到音频包 → 每秒记一次（I2S 由播放任务补静音，无需单独动作） */
static void sp_check_stream_timeout(void)
{
    int64_t now = sp_now_us();
    int64_t silent_ms = 0;
    bool    playing = false;
    bool    warn = false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    playing = (s_slave.state == SYNC_STATE_PLAYING) ||
              (s_slave.state == SYNC_STATE_PREPARING);
    if (s_slave.last_rx_us != 0)
    {
        silent_ms = (now - s_slave.last_rx_us) / 1000;
    }
    if (playing && silent_ms >= SP_STREAM_TIMEOUT_MS &&
        (now - s_slave.last_silent_warn_us) >= 1000000)
    {
        s_slave.last_silent_warn_us = now;
        warn = true;
    }
    xSemaphoreGive(s_lock);

    if (warn)
    {
        ESP_LOGW(TAG, "no audio for %" PRId64 "ms (WiFi/主节点?)", silent_ms);
    }
}

/**
 * @brief SyncHeartbeat_Task：控制组播通道的唯一读者 + PING 发送者
 *
 * 控制通道的 PONG / SYNC_START / SYNC_HEARTBEAT / 基础命令全部由本任务串行
 * 处理（wifi_manager 的接收 FIFO 是 NOSPLIT ringbuffer，多读者会把包随机
 * 分给不同任务，语义就散了）。
 */
static void sp_hb_task(void *arg)
{
    int64_t next_ping_us;
    int     burst_left = SP_PING_BURST_COUNT;

    (void)arg;
    ESP_LOGI(TAG, "SyncHeartbeat_Task start (prio 8, core %d)", xPortGetCoreID());

    /* 起始相位按 MAC 末字节错开，避免多从机同刻撞主节点的控制通道 */
    {
        uint8_t mac[6] = {0};
        uint32_t phase_ms = 0;
        if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK)
        {
            phase_ms = mac[5] % (SP_PING_BURST_MS * 2);
        }
        next_ping_us = sp_now_us() + (int64_t)phase_ms * 1000;
    }

    for (;;)
    {
        if (s_slave.deinit_pending)
        {
            break;
        }
        if (!s_slave.started)
        {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        /* ---- 控制通道接收 ---- */
        if (s_ping_outstanding)
        {
            /* 未决 PING：收紧到 SP_PONG_SPIN_US 一轮的非阻塞轮询。
             * t4 的量化误差会以一半的权重直接进 θ，10ms 的 tick 粒度
             * 会把同步精度吃光，所以这一段必须忙等。 */
            while (s_ping_outstanding && sp_now_us() < s_ping_deadline_us)
            {
                sp_ctrl_drain();
                if (s_ping_outstanding)
                {
                    esp_rom_delay_us(SP_PONG_SPIN_US);
                }
            }
            /* PONG 丢包/超时：清标志，回到省 CPU 的阻塞轮询路径 */
            s_ping_outstanding = false;
            sp_pending_expire();
        }
        else
        {
            uint8_t buf[SP_CTRL_RX_BUF];
            size_t  len = 0;
            if (wifi_manager_mcast_recv(s_slave.cfg.control_chan, buf, sizeof(buf),
                                        &len, SP_CTRL_IDLE_TIMEOUT_MS) == ESP_OK && len > 0)
            {
                sp_dispatch_ctrl(buf, len);
            }
            sp_ctrl_drain();
        }

        /* ---- PING 节奏：启动连发 10 次 × 100ms，之后 1000ms 维护 ---- */
        if (sp_now_us() >= next_ping_us)
        {
            sp_send_ping();
            if (burst_left > 0)
            {
                burst_left--;
                next_ping_us += (int64_t)SP_PING_BURST_MS * 1000;
            }
            else
            {
                uint32_t iv = s_slave.cfg.ping_interval_ms ? s_slave.cfg.ping_interval_ms
                                                          : SP_PING_INTERVAL_DEF_MS;
                next_ping_us += (int64_t)iv * 1000;
            }
        }

        sp_maybe_log_status();
        sp_check_stream_timeout();
    }

    ESP_LOGI(TAG, "SyncHeartbeat_Task exit");
    s_slave.hb_task = NULL;
    vTaskDelete(NULL);
}

/* ======================== 任务：播放 ============================================= */

/**
 * @brief 取下一帧并算出出声时刻（锁内只做快照，不出队写 I2S）
 *
 * @param[out] out_samples  本帧采样数（661/662）
 * @param[out] out_play_at  本帧目标出声时刻（本地时间轴，µs）
 * @param[out] out_seq      本帧 seq（写完后由调用方推进游标）
 * @param[out] out_silence  true = 缺帧，写静音
 * @param[out] out_pcm      指向 PCM 数据（写静音时无意义）
 * @return false 表示当前没有可出队的帧（缓冲未就绪 / 未到锚定条件 / 暂停）
 */
static bool sp_play_next_locked(uint16_t *out_samples, int64_t *out_play_at,
                                uint64_t *out_t_tx, bool *out_silence,
                                const int16_t **out_pcm)
{
    sp_jb_slot_t *slot;
    uint32_t      idx;
    int16_t       lead;

    if (!s_slave.delay_ms || s_slave.state != SYNC_STATE_PLAYING)
    {
        return false;
    }
    if (!s_slave.cursor_valid)
    {
        return false;
    }
    /* 时钟没锁 → θ 无意义 → 算出来的出声时刻是垃圾。θ=0 时
     * sp_master_to_local_locked() 会把主节点的开机时长当成本地时间，
     * play_at 直接被甩到几十分钟之后，sp_sleep_until() 一觉睡死，
     * 而且全程不打一行日志。这里宁可不播，等时钟锁上再说。 */
    if (!s_slave.clock_locked)
    {
        return false;
    }

    lead = sp_seq_diff(s_slave.newest_seq, s_slave.next_seq);

    /* 落后到「新到的包会被窗口整批拒绝」才重锚 —— 门限的含义见
     * SP_REANCHOR_LEAD 的定义，它必须严格大于锚定动作自己产生的 lead，
     * 否则重锚与出队判定互相打架，形成死循环。 */
    if (lead >= SP_REANCHOR_LEAD)
    {
        sp_anchor_locked();
        return false;
    }

    idx  = (uint32_t)(s_slave.next_seq % s_slave.jb_slots);
    slot = &s_slave.jb[idx];

    if (slot->state == SP_SLOT_READY && slot->seq == s_slave.next_seq)
    {
        *out_samples = slot->samples;
        *out_play_at = sp_master_to_local_locked((int64_t)slot->t_tx_us) +
                       (int64_t)s_slave.delay_ms * 1000 - SP_WRITE_AHEAD_US;
        *out_t_tx    = slot->t_tx_us;
        *out_silence = false;
        *out_pcm     = slot->pcm;

        /* 标记为已消费：写侧要 ≥ jb_slots 帧之后才会再碰这一槽，而数据在
         * 15ms 内就被读走 —— 所以在锁外读 slot->pcm 是安全的 */
        slot->state = SP_SLOT_EMPTY;
        if (s_slave.filled_cnt > 0)
        {
            s_slave.filled_cnt--;
        }
        s_slave.next_seq = (uint16_t)(s_slave.next_seq + 1);
        /* 用包里的真值重锚栅格，而不是累加推算值，避免预测误差累积 */
        s_slave.next_t_tx_us = slot->t_tx_us +
                               (uint64_t)s_slave.frame_ms * 1000ull;
        return true;
    }

    /* ---- 缺帧：补静音，并且**保持 15ms 栅格**，否则后面的帧会整体错位 ---- */
    {
        bool    starving = (lead <= 0);      /* 流还没到 → 真空载；lead>0 → 这一包丢了 */
        uint16_t n = (uint16_t)((s_slave.next_seq & 1u) ? 662u : 661u);

        *out_samples = n;
        *out_play_at = sp_master_to_local_locked((int64_t)s_slave.next_t_tx_us) +
                       (int64_t)s_slave.delay_ms * 1000 - SP_WRITE_AHEAD_US;
        *out_t_tx    = s_slave.next_t_tx_us;
        *out_silence = true;
        *out_pcm     = s_silence;

        if (starving)
        {
            s_slave.underrun_run++;
            s_slave.underrun_ms += s_slave.frame_ms;
            if (s_slave.underrun_run >= SP_UNDERRUN_RESYNC_FRAMES)
            {
                /* 静音超过 ~500ms：流多半已经断了，回 PREPARING 重新灌缓冲，
                 * 不要继续在错误的游标上补静音 */
                ESP_LOGW(TAG, "underrun %" PRIu32 " frames, back to PREPARING",
                         s_slave.underrun_run);
                sp_jb_clear_locked();
                s_slave.state = SYNC_STATE_PREPARING;
                return false;
            }
        }
        else
        {
            s_slave.lost_pkts++;
            s_slave.underrun_run = 0;
        }

        s_slave.next_seq      = (uint16_t)(s_slave.next_seq + 1);
        s_slave.next_t_tx_us += (uint64_t)s_slave.frame_ms * 1000ull;
    }
    return true;
}

/**
 * @brief SyncPlayback_Task：按绝对时刻出队，写 I2S
 *
 * 出声时刻 = t_play_local = t_tx + D + θ。弹性缓冲由 Jitter Buffer 承担，
 * DMA 占用保持接近 0（SP_WRITE_AHEAD_US = 0），所以"写进去的时刻"就等于
 * "出声的时刻"—— 与主节点 sync_play_task 的做法一致。
 */
static void sp_play_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "SyncPlayback_Task start (prio 10, core %d)", xPortGetCoreID());

    for (;;)
    {
        if (s_slave.deinit_pending)
        {
            break;
        }
        if (!s_slave.started)
        {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        uint16_t        samples = 0;
        int64_t         play_at = 0;
        uint64_t        t_tx = 0;
        bool            silence = false;
        const int16_t  *pcm = NULL;
        bool            became_playing = false;
        uint32_t        jitter_now = 0;

        xSemaphoreTake(s_lock, portMAX_DELAY);

        /* PREPARING → PLAYING：两个条件都满足才出声 —— 时钟锁上了（θ 才有
         * 意义），并且缓冲攒够 D 时长。这里只改状态，日志/事件放到锁外。 */
        if (s_slave.state == SYNC_STATE_PREPARING &&
            s_slave.clock_locked &&
            s_slave.filled_cnt * s_slave.frame_ms >= s_slave.delay_ms)
        {
            sp_anchor_locked();
            s_slave.state  = SYNC_STATE_PLAYING;
            became_playing = true;
            jitter_now     = s_slave.filled_cnt;
        }

        bool got = sp_play_next_locked(&samples, &play_at, &t_tx, &silence, &pcm);
        xSemaphoreGive(s_lock);

        if (became_playing)
        {
            ESP_LOGI(TAG, "state -> PLAYING (jitter=%u pkts, D=%ums)",
                     (unsigned)jitter_now, (unsigned)s_slave.delay_ms);
            sp_emit_event(SYNC_EVT_STATE_CHANGED);
        }
        if (!got)
        {
            /* 缓冲里一个可播的帧都没有（还没进 PLAYING，或刚被清空）。
             * 这里不能写 pdMS_TO_TICKS(2)：本工程 FREERTOS_HZ=100
             * （1 tick = 10ms）时它是 0，vTaskDelay(0) 不会让出 CPU，
             * 整条循环没有阻塞点 —— Core 1 的 IDLE1 会被饿死、5 秒后
             * Task WDT 报错（Core 0 上的 wifi_mcast_rx 就是这么栽的）。
             * 睡满 1 tick，既喂上狗又刚好是重新看一眼缓冲的合理节奏。 */
            vTaskDelay(1);
            continue;
        }

        /* ---- 锁外等到点 ---- */
        sp_sleep_until(play_at);

        /* ---- 锁外写 I2S：Amplifier_Play_Buffer 最长可能阻塞 500ms ---- */
        size_t     bytes = (size_t)samples * sizeof(int16_t);   /* MONO：直接 n*2 */
        esp_err_t  ret   = Amplifier_Play_Buffer((const uint8_t *)pcm, bytes, NULL,
                                                 SP_PLAY_WRITE_TIMEOUT_MS);
        if (ret != ESP_OK)
        {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_slave.dropped_pkts++;
            xSemaphoreGive(s_lock);
            ESP_LOGW(TAG, "play buffer failed: %s", esp_err_to_name(ret));
            continue;
        }

        if (!silence)
        {
            /* 漂移测量用：最近真正写出的帧在主节点时间轴上的位置。
             * 心跳下发的 master_pts_us 是同一个口径（都是"最近出队帧的
             * t_tx"），所以两者相减就是位置误差。 */
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_slave.last_written_t_tx = (int64_t)t_tx;
            s_slave.has_last_written  = true;
            xSemaphoreGive(s_lock);
        }
    }

    ESP_LOGI(TAG, "SyncPlayback_Task exit");
    s_slave.play_task = NULL;
    vTaskDelete(NULL);
}

/* ======================== 生命周期 =============================================== */

/** 等待某个任务句柄归零（任务在自己退出前清句柄） */
static void sp_wait_task(volatile TaskHandle_t *handle)
{
    for (int i = 0; i < 200 && *handle != NULL; i++)
    {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t sync_protocol_slave_init(const sync_protocol_slave_cfg_t *cfg)
{
    if (cfg == NULL || cfg->audio_chan == NULL || cfg->control_chan == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_slave.inited)
    {
        ESP_LOGW(TAG, "already initialized");
        return ESP_OK;
    }

    memset(&s_slave, 0, sizeof(s_slave));

    if (s_lock == NULL)
    {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL)
        {
            ESP_LOGE(TAG, "mutex create failed");
            return ESP_ERR_NO_MEM;
        }
    }

    s_slave.cfg              = *cfg;
    s_slave.frame_ms         = SP_FRAME_MS;
    s_slave.delay_ms         = cfg->sync_delay_ms ? cfg->sync_delay_ms : SP_D_DEF_MS;
    s_slave.cur_sample_rate  = AMPLIFIER_SAMPLE_RATE;
    s_slave.state            = SYNC_STATE_IDLE;
    s_slave.jb_slots         = cfg->jitter_slots ? cfg->jitter_slots : SP_JB_SLOTS_DEF;
    if (s_slave.jb_slots > SP_JB_SLOTS_MAX)
    {
        s_slave.jb_slots = SP_JB_SLOTS_MAX;
    }
    if (!s_slave.cfg.ping_interval_ms)
    {
        s_slave.cfg.ping_interval_ms = SP_PING_INTERVAL_DEF_MS;
    }

    /* Jitter Buffer 优先 PSRAM：24 槽 × ≈1340B ≈ 32KB，内部 RAM 放不下 */
    size_t bytes = sizeof(sp_jb_slot_t) * s_slave.jb_slots;
    s_slave.jb = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_slave.jb == NULL)
    {
        s_slave.jb = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    }
    if (s_slave.jb == NULL)
    {
        ESP_LOGE(TAG, "jitter buffer alloc %u bytes failed", (unsigned)bytes);
        return ESP_ERR_NO_MEM;
    }
    memset(s_slave.jb, 0, bytes);

    s_slave.inited = true;
    ESP_LOGI(TAG, "init: D=%ums jitter=%u slots (%u bytes) ping=%ums",
             (unsigned)s_slave.delay_ms, (unsigned)s_slave.jb_slots, (unsigned)bytes,
             (unsigned)s_slave.cfg.ping_interval_ms);
    return ESP_OK;
}

esp_err_t sync_protocol_slave_deinit(void)
{
    if (!s_slave.inited)
    {
        return ESP_ERR_INVALID_STATE;
    }

    sync_protocol_slave_stop();

    s_slave.deinit_pending = true;

    /* 唤醒可能阻塞在 mcast_recv 里的任务，然后等它们自行退出。
     * 播放任务可能正卡在 Amplifier_Play_Buffer（最长 500ms），所以要等。 */
    sp_wait_task(&s_slave.rx_task);
    sp_wait_task(&s_slave.hb_task);
    sp_wait_task(&s_slave.play_task);

    if (s_slave.jb != NULL)
    {
        heap_caps_free(s_slave.jb);
        s_slave.jb = NULL;
    }
    if (s_lock != NULL)
    {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
    }

    s_slave.inited = false;
    ESP_LOGI(TAG, "deinitialized");
    return ESP_OK;
}

esp_err_t sync_protocol_slave_register_event_cb(sync_protocol_event_cb_t cb, void *user_ctx)
{
    if (!s_slave.inited)
    {
        return ESP_ERR_INVALID_STATE;
    }
    s_slave.event_cb  = cb;
    s_slave.event_ctx = user_ctx;
    return ESP_OK;
}

esp_err_t sync_protocol_slave_start(void)
{
    BaseType_t ok;

    if (!s_slave.inited)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_slave.started)
    {
        return ESP_OK;
    }
    if (s_slave.cfg.control_chan == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    sp_jb_clear_locked();
    s_slave.rx_pkts       = 0;
    s_slave.lost_pkts     = 0;
    s_slave.late_pkts     = 0;
    s_slave.dropped_pkts  = 0;
    s_slave.underrun_ms   = 0;
    s_slave.drift_error_us = 0;
    s_slave.last_rx_us    = sp_now_us();
    s_slave.last_status_log_us = sp_now_us();
    s_slave.state         = SYNC_STATE_PREPARING;
    xSemaphoreGive(s_lock);

    s_ping_outstanding = false;
    s_slave.started    = true;

    /* Core 0：接收与心跳贴着 WiFi 走；Core 1：播放独占，不被网络栈打扰。
     * 优先级严格分层：接收(11) > 播放(10) > 心跳(8)。 */
    ok = xTaskCreatePinnedToCore(sp_rx_task, "SyncReceiver", 8192, NULL, 11,
                                 (TaskHandle_t *)&s_slave.rx_task, 0);
    if (ok != pdPASS)
    {
        ESP_LOGE(TAG, "create SyncReceiver_Task failed");
        s_slave.started = false;
        return ESP_ERR_NO_MEM;
    }

    ok = xTaskCreatePinnedToCore(sp_hb_task, "SyncHeartbeat", 4096, NULL, 8,
                                 (TaskHandle_t *)&s_slave.hb_task, 0);
    if (ok != pdPASS)
    {
        ESP_LOGE(TAG, "create SyncHeartbeat_Task failed");
        return ESP_ERR_NO_MEM;
    }

    ok = xTaskCreatePinnedToCore(sp_play_task, "SyncPlayback", 6144, NULL, 10,
                                 (TaskHandle_t *)&s_slave.play_task, 1);
    if (ok != pdPASS)
    {
        ESP_LOGE(TAG, "create SyncPlayback_Task failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "started: D=%ums, jitter=%u slots, 开始 PING 探测",
             (unsigned)s_slave.delay_ms, (unsigned)s_slave.jb_slots);
    return ESP_OK;
}

esp_err_t sync_protocol_slave_stop(void)
{
    if (!s_slave.inited)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_slave.started = false;
    s_ping_outstanding = false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    sp_jb_clear_locked();
    s_slave.state = SYNC_STATE_STOPPED;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "stopped (rx=%lu lost=%lu late=%lu under=%lums)",
             (unsigned long)s_slave.rx_pkts, (unsigned long)s_slave.lost_pkts,
             (unsigned long)s_slave.late_pkts, (unsigned long)s_slave.underrun_ms);
    return ESP_OK;
}

esp_err_t sync_protocol_slave_get_status(sync_protocol_slave_status_t *status)
{
    if (!s_slave.inited)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    sp_snapshot(status);
    return ESP_OK;
}

bool sync_protocol_slave_is_playing(void)
{
    bool playing;

    if (!s_slave.inited)
    {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    playing = (s_slave.state == SYNC_STATE_PLAYING);
    xSemaphoreGive(s_lock);
    return playing;
}

uint32_t sync_protocol_slave_get_rx_pkts(void)
{
    uint32_t n;

    if (!s_slave.inited)
    {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    n = s_slave.rx_pkts;
    xSemaphoreGive(s_lock);
    return n;
}
