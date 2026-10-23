/**
 * @file udp_audio.c
 * @brief 单播 UDP 音频接收 + 主从同步播放（协议 v2）实现
 *
 * 两个任务，职责单向依赖：
 *
 *   ua_rx_task    recvfrom → 校验 → 按 seq 落进环形槽位 → 更新时钟偏移窗口
 *                      │
 *                      ▼  （槽位 + 互斥锁）
 *   ua_play_task  取下一帧 → 按 t_play_us 换算本地时刻 → 写 I2S
 *                 └─ 丢包补 PLC、漂移补 sample slip
 *
 * 为什么播放任务不是"收到就写"：
 *   写 I2S 是阻塞的，DMA 排空的速度就是从机晶振的速度。如果直接把收到的帧
 *   塞进去，出声时刻完全由网络到达时刻决定，主从会差出一段随机的网络延迟，
 *   同一间屋子听就是回声。所以必须先按 t_play_us 对齐，再用 jitter buffer
 *   吸收抖动。
 *
 * 时钟偏移为什么不需要 PING/PONG：
 *   `d(i) = 本地收到时刻 − t_play_us` 里的固定部分就是
 *   (从机时钟 − 主机时钟) − D。取最近一个窗口的最小值，就得到
 *   "主机时钟 + 最小单程延迟"。发送端把 D 通过协议头 `lead_ms` 告诉我们，
 *   把它和抖动余量一起加回去，两端就对齐了。残差只有最小单程延迟 L_min
 *   （局域网通常 1~3ms），听不出来。
 *   好处：从节点**不需要向主节点发任何回包**，主节点也不必处理 PING。
 *
 * 漂移怎么办：
 *   两块板子的晶振差几十 ppm，播放速度必然缓慢失配。这里把时钟换算偏移
 *   冻结在开始播放的那一刻，让误差自然累积，再用 sample slip（丢/插一个
 *   样本）把误差拉回来。观测量是抖动缓冲的"跑道"——只有它同时跨越了
 *   主从两个晶振，能看见漂移（播放任务里有详细说明）。
 *   不去调 I2S 采样率 —— 那会 disable→reconfig→enable，
 *   把 DMA 里未播的音频整个丢掉（见 amplifier.c 的注释）。
 */

#include "udp_audio.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "amplifier.h"

#define TAG "UDP_AUDIO"

/* ======================== 可调参数 ======================== */

/** 单次 recvfrom 缓冲。UDP 不分片时最大 1472B，留足余量 */
#define UA_RX_BUF_BYTES             (4096)

/** socket 接收缓冲。64KB ≈ 740ms 的 44.1k/16bit 单声道音频 */
#define UA_SOCK_RCVBUF              (64 * 1024)

/** recvfrom 超时，只用来定期查看链路状态，不参与播放节奏 */
#define UA_RECV_TIMEOUT_MS          (200)

/** 状态打印间隔 */
#define UA_STATS_INTERVAL_MS        (5000)

#define UA_RX_TASK_STACK            (4096)
#define UA_RX_TASK_PRIO             (11)

#define UA_PLAY_TASK_STACK          (5120)
#define UA_PLAY_TASK_PRIO           (10)

/** 单帧最大采样数。主节点是 661/662，PC 端默认 441，这里留到 1024 */
#define UA_MAX_SAMPLES              (1024)

/** 环形槽位数（必须是 2 的幂，用 seq % UA_SLOTS 定位）。
 *  64 槽 × 1024 采样 ≈ 132KB PSRAM：10ms 包能存 640ms，远大于
 *  UA_LEVEL_MAX_US（450ms）；32 槽在 10ms 包时只有 320ms，会被"过深"阈值
 *  追平，发送端稍快就会覆盖还没播的槽。 */
#define UA_SLOTS                    (64)

/** 时钟偏移窗口长度（包数）。100 包/秒时约 640ms */
#define UA_OFFSET_WIN               (64)

/** 开始出声前至少攒够多少帧（与"提前量的 3/4"取与） */
#define UA_PREROLL_PKTS             (8)

/** 判定断流的时间：超过这个时间没有新包就清盘重新等 */
#define UA_UNDERRUN_RESET_US        (500000)

/** 缓冲深度超过这个值就跳到较新的位置（防止延迟无限累积） */
#define UA_LEVEL_MAX_US             (450000)

/** 跳帧后希望保留的缓冲深度 */
#define UA_LEVEL_TARGET_US          (150000)

/** sample slip 的死区与最小间隔。
 *  控制量是**抖动缓冲的跑道**：最近收到的 t_play − 正在播放帧的 t_play。
 *  从机晶振慢 → DMA 消耗慢 → 跑道变长；快 → 变短。基准在开播 2~3 秒后
 *  自己测出来（开播瞬间 DMA 还在填，跑道有个瞬态），不用知道 DMA 深度。
 *  死区 4ms：低于这个的偏差听不出来，EMA 之后剩下的噪声也远小于它。 */
#define UA_SLIP_DEADBAND_US         (4000)
#define UA_SLIP_MIN_GAP_US          (20000)

/** 跑道观测值的滑动平均：1/32，约 0.3~0.5 秒的等效时间常数 */
#define UA_LEVEL_EMA_SHIFT          (5)

/** 开播后积累多少帧才算 DMA/缓冲瞬态过去，此时把当前跑道均值定为漂移基准 */
#define UA_SLIP_WARMUP              (200)

/** sample slip 拼接处的平滑点数。直接丢/插一个样本会在波形上留下一个台阶
 *  （连续样本差可达上千 LSB），每秒几次就是"咔哒"声。用前一个输出样本对
 *  拼接点附近做几点的线性过渡，把台阶摊开成 ~0.2ms 的缓变。 */
#define UA_SPLICE_TAP               (8)

/** 每次（重新）开播时第一帧的淡入点数。DMA 从静音切到音频，如果首样本
 *  不在零点附近，会有一个直流台阶（"啪"一声）。128 点 ≈ 2.9ms，听不出。 */
#define UA_FADE_IN_SAMPLES          (128)

/** 非播放态喂静音用的帧长：441 采样 = 10ms @44.1k。
 *  与 UDIO_SAMPLE_RATE 无关地取 10ms，是为了在 preroll 期间也能及时
 *  响应"数据到齐、可以开播"（最多晚 10ms）。 */
#define UA_SILENCE_SAMPLES          (441)
#define UA_SILENCE_US               (10000)

/* ======================== 环形槽位 ======================== */

typedef struct {
    uint64_t t_play_us;             /* 该帧的基准播放时刻（发送端时钟） */
    uint16_t seq;
    uint16_t nsamples;
    uint8_t  valid;
    int16_t  pcm[UA_MAX_SAMPLES];
} ua_slot_t;

/* ======================== 模块状态 ======================== */

static TaskHandle_t   s_rx_task   = NULL;
static TaskHandle_t   s_play_task = NULL;
static volatile bool  s_running   = false;
static uint16_t       s_port      = UDIO_DEFAULT_PORT;

/** 保护下面所有跨任务字段与槽位内容。
 *  约定：**持锁期间绝不调用 Amplifier_Play_Buffer()**（可能阻塞十几毫秒）。 */
static SemaphoreHandle_t s_lock = NULL;

/** 环形槽位（PSRAM）。64 × (2048B PCM + 16B 元数据) ≈ 132KB */
static ua_slot_t *s_slots = NULL;

/* ---- 接收侧 ---- */
static bool     s_have_stream;      /* 已经收到过至少一个包 */
static uint16_t s_stream_id;        /* 当前流标识 */
static uint16_t s_first_seq;        /* 当前缓冲区里最早的 seq（未播放期间维护） */
static uint16_t s_write_seq;        /* 最近一次写入的 seq */
static int64_t  s_last_t_play_us;   /* 最近一帧的 t_play（发送端时钟） */
static int64_t  s_last_rx_us;       /* 最近一次收到包的时刻（本地时钟） */
static uint16_t s_lead_ms;          /* 发送端提前量 D（来自协议头） */

static int64_t  s_d_win[UA_OFFSET_WIN];
static int      s_d_idx;
static int      s_d_cnt;
static int64_t  s_d_min;

/* ---- 播放侧 ---- */
static bool     s_playing;          /* 已完成 preroll，正在出声 */
static uint16_t s_read_seq;         /* 下一帧要播的 seq */
static int64_t  s_offset_us;        /* 冻结的时刻换算偏移（见文件头推导） */
static int64_t  s_last_slip_us;

/* 漂移控制：抖动缓冲"跑道"的滑动平均，以及稳定后的基准值。
 *
 * 跑道 = 最近收到的 t_play − 正在播放帧的 t_play。从机晶振慢 → DMA 消耗慢
 * → 跑道单调变长；快 → 变短。晶振差就体现在它的变化率上。
 *
 * 为什么不用"写入时刻 − 时刻表"当控制量：写入时刻由 I2S DMA 的排空节奏
 * 决定，而 DMA 和 esp_timer 共用同一个晶振。每写一帧，now 前进的微秒数
 * 正好等于 t_play 前进的微秒数（都是 1e6/44100 × 采样数），所以那个差值
 * 恒为 −DMA 深度，晶振差在里面完全看不见。 */
static int64_t  s_obs_avg_us;    /* 跑道观测值的 EMA */
static int64_t  s_obs_ref_us;    /* 稳定后的跑道基准 */
static uint32_t s_obs_samples;
static bool     s_slip_ready;

/* ---- 统计（允许极小概率的竞争丢失，不值得为它加锁） ---- */
static volatile uint32_t s_rx_pkts;
static volatile uint32_t s_bad_pkts;
static volatile uint32_t s_lost_pkts;   /* 用 PLC 补过的帧 */
static volatile uint32_t s_late_pkts;
static volatile uint32_t s_dup_pkts;
static volatile uint32_t s_slip_cnt;

/* ======================== 小工具 ======================== */

/** 16 位 seq 的环形比较：a 在 b 之后返回正数 */
static inline int16_t ua_seq_diff(uint16_t a, uint16_t b)
{
    return (int16_t)(a - b);
}

/** 清空缓冲并重置一次流的全部状态（调用方必须持锁） */
static void ua_reset_stream_locked(uint16_t stream_id, uint16_t first_seq, uint16_t lead_ms)
{
    for (int i = 0; i < UA_SLOTS; i++)
    {
        s_slots[i].valid = 0;
    }

    s_have_stream    = true;
    s_stream_id      = stream_id;
    s_first_seq      = first_seq;
    s_write_seq      = first_seq;
    s_lead_ms        = lead_ms;
    s_last_t_play_us = 0;
    s_last_rx_us     = esp_timer_get_time();

    s_d_idx = 0;
    s_d_cnt = 0;
    s_d_min = INT64_MAX;

    s_playing   = false;    /* 播放任务会重新走 preroll */
    s_offset_us = 0;

    /* 漂移控制的基准要重新测（新流的 offset 会重算） */
    s_obs_avg_us  = 0;
    s_obs_ref_us  = 0;
    s_obs_samples = 0;
    s_slip_ready  = false;
}

/** 偏移窗口：保留最近 UA_OFFSET_WIN 个 d(i)，取最小值（调用方必须持锁） */
static void ua_offset_update_locked(int64_t d)
{
    s_d_win[s_d_idx] = d;
    s_d_idx = (s_d_idx + 1) % UA_OFFSET_WIN;
    if (s_d_cnt < UA_OFFSET_WIN)
    {
        s_d_cnt++;
    }

    int64_t m = INT64_MAX;
    for (int i = 0; i < s_d_cnt; i++)
    {
        if (s_d_win[i] < m)
        {
            m = s_d_win[i];
        }
    }
    s_d_min = m;
}

/** 槽位是否有效且确实属于这个 seq（调用方必须持锁） */
static bool ua_slot_valid_locked(uint16_t seq)
{
    ua_slot_t *sl = &s_slots[seq % UA_SLOTS];
    return sl->valid && sl->seq == seq;
}

/** 统计当前有效槽位数量（调用方必须持锁） */
static int ua_count_valid_locked(void)
{
    int n = 0;
    for (int i = 0; i < UA_SLOTS; i++)
    {
        if (s_slots[i].valid)
        {
            n++;
        }
    }
    return n;
}

/** 找缓冲区里最早的有效 seq（调用方必须持锁）。
 *  从 s_first_seq 往后扫 UA_SLOTS 个，只在"还没开始播"的阶段使用。 */
static bool ua_oldest_valid_locked(uint16_t *out)
{
    for (int i = 0; i < UA_SLOTS; i++)
    {
        uint16_t seq = (uint16_t)(s_first_seq + i);
        if (ua_slot_valid_locked(seq))
        {
            *out = seq;
            return true;
        }
    }
    return false;
}

/** 缓冲过深时，挑一个既有效、又比 last_t_play 早不超过 target_us 的 seq。
 *  从最新往回扫，命中的第一个就是窗口内最老的帧（调用方必须持锁）。 */
static bool ua_target_seq_locked(int64_t target_us, uint16_t *out)
{
    int64_t want = s_last_t_play_us - target_us;

    for (int i = 0; i < UA_SLOTS; i++)
    {
        uint16_t seq = (uint16_t)(s_write_seq - i);   /* i=0 是最新 */
        if (!ua_slot_valid_locked(seq))
        {
            continue;
        }
        if ((int64_t)s_slots[seq % UA_SLOTS].t_play_us >= want)
        {
            *out = seq;
            return true;
        }
    }
    return false;
}

/** 建一个纯单播 UDP socket 并绑定端口；失败返回 -1（调用方退避重试） */
static int ua_open_socket(uint16_t port)
{
    struct sockaddr_in addr;
    int                sock;
    int                rcvbuf = UA_SOCK_RCVBUF;
    struct timeval     tv;

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0)
    {
        ESP_LOGW(TAG, "socket() failed errno=%d", errno);
        return -1;
    }

    /* 绑定到 INADDR_ANY：从机 IP 由 DHCP 给，任何本地地址上来 5680 的包都收 */
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        ESP_LOGW(TAG, "bind(%u) failed errno=%d", port, errno);
        close(sock);
        return -1;
    }

    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    tv.tv_sec  = UA_RECV_TIMEOUT_MS / 1000;
    tv.tv_usec = (UA_RECV_TIMEOUT_MS % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ESP_LOGI(TAG, "listening on UDP %u (unicast, proto v%d, margin=%dms)",
             port, UDIO_VERSION, UDIO_MARGIN_US / 1000);
    return sock;
}

/* ======================== 接收任务 ======================== */

static void ua_rx_task(void *arg)
{
    uint8_t *buf        = NULL;
    int      sock       = -1;
    char     src_ip[16] = {0};

    (void)arg;

    buf = (uint8_t *)malloc(UA_RX_BUF_BYTES);
    if (buf == NULL)
    {
        ESP_LOGE(TAG, "rx buffer alloc failed");
        s_running = false;
        s_rx_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    while (s_running)
    {
        struct sockaddr_in from;
        socklen_t          fromlen = sizeof(from);
        int                len;

        if (sock < 0)
        {
            sock = ua_open_socket(s_port);
            if (sock < 0)
            {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        }

        len = recvfrom(sock, buf, UA_RX_BUF_BYTES, 0,
                       (struct sockaddr *)&from, &fromlen);
        if (len < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                continue;       /* 超时很正常，播放任务有自己的状态机 */
            }
            ESP_LOGW(TAG, "recvfrom errno=%d, reopening socket", errno);
            close(sock);
            sock = -1;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (len < (int)UDIO_HDR_SIZE)
        {
            s_bad_pkts++;
            continue;
        }

        const udio_hdr_t *h = (const udio_hdr_t *)buf;
        if (h->magic != UDIO_MAGIC || h->version != UDIO_VERSION)
        {
            s_bad_pkts++;
            continue;
        }

        size_t pcm_len  = (size_t)len - UDIO_HDR_SIZE;
        pcm_len        &= ~(size_t)1;
        size_t nsamples = pcm_len / 2;
        if (nsamples == 0 || nsamples > UA_MAX_SAMPLES)
        {
            s_bad_pkts++;
            continue;
        }

        /* 首次见到发送端时打印一次，方便排查"发到哪去了" */
        if (src_ip[0] == 0)
        {
            inet_ntoa_r(from.sin_addr, src_ip, sizeof(src_ip));
            ESP_LOGI(TAG, "first packet from %s:%u",
                     src_ip, (unsigned)ntohs(from.sin_port));
        }

        int64_t now = esp_timer_get_time();
        int64_t d   = now - (int64_t)h->t_play_us;

        xSemaphoreTake(s_lock, portMAX_DELAY);

        /* 新流 / 发送端重启：整盘清零，等播放任务重新 preroll。
         * 重启后 seq 从 0 重新数，但 stream_id 有可能不变（发送端显式指定
         * 同一个 id），所以还要看首帧标志；否则新包会被当成"迟到的旧包"
         * 全部丢掉。两个附加条件用来放过"首帧的重复包"：
         *   - 距上一个包超过 100ms（真正的停顿）；
         *   - 或 seq 比当前写游标退了超过一整圈槽位。 */
        int16_t back = ua_seq_diff(h->seq, s_write_seq);
        bool seq_restart = (h->flags & UDIO_FLAG_STREAM_START) &&
                           s_have_stream &&
                           ((now - s_last_rx_us) > 100000 ||
                            back < -(int16_t)UA_SLOTS);
        if (!s_have_stream || h->stream_id != s_stream_id || seq_restart)
        {
            ua_reset_stream_locked(h->stream_id, h->seq, h->lead_ms);
            ESP_LOGI(TAG, "stream %u start (lead=%ums)",
                     (unsigned)h->stream_id, (unsigned)h->lead_ms);
        }

        /* 已经播过的包直接丢，别让它把槽位里的新数据覆盖掉 */
        if (s_playing && ua_seq_diff(h->seq, s_read_seq) < 0)
        {
            s_late_pkts++;
            xSemaphoreGive(s_lock);
            continue;
        }

        ua_slot_t *slot = &s_slots[h->seq % UA_SLOTS];
        if (slot->valid && slot->seq == h->seq)
        {
            s_dup_pkts++;
            xSemaphoreGive(s_lock);
            continue;
        }

        slot->t_play_us = h->t_play_us;
        slot->seq       = h->seq;
        slot->nsamples  = (uint16_t)nsamples;
        memcpy(slot->pcm, buf + UDIO_HDR_SIZE, pcm_len);
        slot->valid     = 1;

        /* 维护"缓冲区里最早的 seq"：只在还没开始播时推进，
         * 避免把乱序到达的头几包丢掉 */
        if (!s_playing && ua_seq_diff(h->seq, s_first_seq) < 0)
        {
            s_first_seq = h->seq;
        }

        /* 乱序到达时只让"最新"前进，不要被迟到的包拉回去：
         * s_write_seq 决定跳帧扫描起点，s_last_t_play_us 用来算缓冲深度 */
        if (ua_seq_diff(h->seq, s_write_seq) > 0)
        {
            s_write_seq = h->seq;
        }
        if ((int64_t)h->t_play_us > s_last_t_play_us)
        {
            s_last_t_play_us = (int64_t)h->t_play_us;
        }
        s_last_rx_us = now;
        ua_offset_update_locked(d);

        xSemaphoreGive(s_lock);
        s_rx_pkts++;
    }

    if (sock >= 0)
    {
        close(sock);
    }
    free(buf);
    s_rx_task = NULL;
    vTaskDelete(NULL);
}

/* ======================== 播放任务 ======================== */

/** 饱和到 int16（平滑过渡时可能出现越界的中间值） */
static inline int16_t ua_sat16(int32_t v)
{
    if (v > 32767)
    {
        return 32767;
    }
    if (v < -32768)
    {
        return -32768;
    }
    return (int16_t)v;
}

/** 把一帧按 slip 方向处理后写进 I2S。
 *
 *  slip > 0：丢一个样本（**输出 n-1 个**，播放时间轴加快一个样本）
 *  slip < 0：插一个样本（输出 n+1 个，播放时间轴放慢一个样本）
 *  slip = 0：原样
 *
 *  ⚠️ 关键：输出样本数必须真的变化，时间轴才会变化。I2S DMA 是按
 *  **从机晶振**消耗输出样本的，写 n-1 个样本，下一帧就提前 1/44100 秒
 *  被写入 DMA，播放头相对发送端时刻表就前进了一个样本。如果丢了样本却
 *  又把长度补齐到 n（早期版本就是这样），时间轴一点没变，等于白丢一个
 *  样本、还在拼接处制造一次台阶 —— 跑道会一直偏，slip 会以最大速率
 *  反复触发。
 *
 *  prev_last 是上一次真正写进 DMA 的最后一个样本，用来在拼接点做短过渡，
 *  避免硬跳变产生"咔哒"声。have_prev=false 表示这是新流的第一帧。
 *
 *  src / dst 必须是两个独立缓冲（slip 时源和目的会错开一个样本）。
 *  返回实际写入的采样数，0 表示写失败。 */
static size_t ua_write_frame(const int16_t *src, size_t n, int slip,
                             int16_t prev_last, bool have_prev,
                             int16_t *dst, uint32_t timeout)
{
    size_t out_n = n;

    if (slip > 0 && n > 1)
    {
        out_n = n - 1;
        memcpy(dst, src + 1, out_n * sizeof(int16_t));

        if (have_prev)
        {
            /* 把 src[1] 与上一个输出样本之间的落差均摊到前 UA_SPLICE_TAP 点：
             * dst[i] = src[i+1] + delta*(k-i)/k，i=0 时正好接上 prev_last。 */
            int32_t delta = (int32_t)prev_last - (int32_t)src[1];
            size_t  k     = (out_n < UA_SPLICE_TAP) ? out_n : UA_SPLICE_TAP;

            for (size_t i = 0; i < k; i++)
            {
                dst[i] = ua_sat16((int32_t)src[i + 1] +
                                  delta * (int32_t)(k - i) / (int32_t)k);
            }
        }
    }
    else if (slip < 0)
    {
        /* 插入的样本取"上一个输出样本"和 src[0] 的中点，拼接点两侧各半个台阶 */
        dst[0] = have_prev ? ua_sat16(((int32_t)prev_last + src[0]) / 2)
                           : src[0];
        memcpy(dst + 1, src, n * sizeof(int16_t));
        out_n = n + 1;
    }
    else
    {
        memcpy(dst, src, n * sizeof(int16_t));
    }

    if (Amplifier_Play_Buffer((const uint8_t *)dst,
                              out_n * sizeof(int16_t), NULL, timeout) != ESP_OK)
    {
        return 0;
    }
    return out_n;
}

/** 丢包隐藏：把上一帧按 3/4 衰减后重复播放，比直接补零听感柔和 */
static void ua_write_plc(int16_t *last_good, size_t n, uint32_t timeout)
{
    if (n == 0)
    {
        return;
    }
    for (size_t i = 0; i < n; i++)
    {
        last_good[i] = (int16_t)((int32_t)last_good[i] * 3 / 4);
    }
    Amplifier_Play_Buffer((const uint8_t *)last_good, n * sizeof(int16_t),
                          NULL, timeout);
}

/**
 * @brief 非播放态喂一帧静音，避免 I2S DMA 放空
 *
 * 从机 I2S 通道在 Amplifier_Init() 后就一直使能，BCLK/WS 持续运行。
 * 播放任务在"还没 preroll 完 / 断流后重新等"的阶段如果什么都不写，
 * DMA 46ms 内就会放空，数据脚浮空被 NS4168 放大成**持续嗡嗡声**——
 * 主节点暂停、换歌、从机刚上电但主节点还没发流时都会遇到。
 *
 * 这里和主节点用同一套节拍策略：阻塞写本身就是节拍源。
 * 写满一帧（耗时接近 10ms）就不再睡；写很快（DMA 有余量）才让出 1 tick。
 */
static void ua_write_silence(int16_t *buf, uint32_t timeout)
{
    int64_t t0;

    if (buf == NULL)
    {
        vTaskDelay(1);
        return;
    }

    memset(buf, 0, UA_SILENCE_SAMPLES * sizeof(int16_t));
    t0 = esp_timer_get_time();
    Amplifier_Play_Buffer((const uint8_t *)buf,
                          UA_SILENCE_SAMPLES * sizeof(int16_t), NULL, timeout);
    if (esp_timer_get_time() - t0 < UA_SILENCE_US / 2)
    {
        vTaskDelay(1);
    }
}

/** 尝试完成 preroll。成功时输出起始 seq、冻结用的 offset、以及该帧的写入时刻 */
static bool ua_try_start(uint16_t *read_seq, int64_t *offset_us, int64_t *start_at)
{
    bool    ok  = false;
    int64_t now = esp_timer_get_time();

    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (s_have_stream && s_d_cnt > 0)
    {
        uint16_t seq;
        int      valid   = ua_count_valid_locked();
        bool     stalled = (now - s_last_rx_us) > 100000;   /* 100ms 没有新包 */

        if (ua_oldest_valid_locked(&seq))
        {
            /* 缓冲深度（时间）：最新包的 t_play − 最老包的 t_play。
             * 发送端只提前 lead_ms 发，所以深度最多就是 lead 那么多，
             * 按包数判（20 包）在 15ms 帧、200ms 提前量下永远凑不齐。 */
            int64_t depth_us = s_last_t_play_us -
                               (int64_t)s_slots[seq % UA_SLOTS].t_play_us;
            int64_t need_us  = (int64_t)s_lead_ms * 1000 * 3 / 4;

            /* 攒到提前量的 3/4 就开始；发送端短暂停手但缓冲区有货时也允许 */
            if ((valid >= UA_PREROLL_PKTS && s_d_cnt >= 10 &&
                 depth_us >= need_us) ||
                (valid > 0 && s_d_cnt >= 5 && stalled))
            {
                ua_slot_t *sl = &s_slots[seq % UA_SLOTS];

                /* d_min 的固定部分 = (从机时钟 − 主机时钟) − D，
                 * 把 D 加回来，再加抖动余量 */
                *offset_us = s_d_min + (int64_t)s_lead_ms * 1000 + UDIO_MARGIN_US;
                *start_at  = (int64_t)sl->t_play_us + *offset_us;
                *read_seq  = seq;
                ok = true;
            }
        }
    }

    xSemaphoreGive(s_lock);
    return ok;
}

static void ua_play_task(void *arg)
{
    /* 三块工作缓冲都放 PSRAM：内部 RAM 要留给 WiFi/DMA */
    int16_t *frame     = heap_caps_malloc((UA_MAX_SAMPLES + 1) * sizeof(int16_t),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *slip_buf  = heap_caps_malloc((UA_MAX_SAMPLES + 1) * sizeof(int16_t),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *last_good = heap_caps_malloc(UA_MAX_SAMPLES * sizeof(int16_t),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t   last_n    = 0;
    bool     first_frame = true;    /* 本次开播后还没写过任何一帧 */
    bool     start_logged = false;  /* 本次开播的首帧诊断是否已打印 */
    int64_t  start_at_local = 0;    /* 本次开播的目标出声时刻（本地时钟） */
    uint32_t last_stats_ms = 0;

    (void)arg;

    if (frame == NULL)
    {
        frame = malloc((UA_MAX_SAMPLES + 1) * sizeof(int16_t));
    }
    if (slip_buf == NULL)
    {
        slip_buf = malloc((UA_MAX_SAMPLES + 1) * sizeof(int16_t));
    }
    if (last_good == NULL)
    {
        last_good = malloc(UA_MAX_SAMPLES * sizeof(int16_t));
    }
    if (frame == NULL || slip_buf == NULL || last_good == NULL)
    {
        ESP_LOGE(TAG, "play buffer alloc failed");
        s_running = false;
        s_play_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    while (s_running)
    {
        bool playing;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        playing = s_playing;
        xSemaphoreGive(s_lock);

        /* ---------- 阶段一：preroll + 对齐起点 ---------- */
        if (!playing)
        {
            uint16_t read_seq;
            int64_t  offset_us;
            int64_t  start_at;
            size_t   first_n;

            if (!ua_try_start(&read_seq, &offset_us, &start_at))
            {
                /* 还没攒够 preroll（或主节点暂停/换歌中）：继续喂静音。
                 * 这里千万不能只 sleep —— DMA 放空后 BCLK 还在跑，
                 * 数据脚浮空就是持续嗡嗡声（从机侧"一直噪音"的根因）。 */
                ua_write_silence(slip_buf, 100);
                continue;
            }

            /* 时钟换算偏移在这里冻结，播放过程中不再改 —— 改偏移等于改出声
             * 时刻，会产生阶跃；漂移交给 sample slip 慢慢补。 */
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_offset_us    = offset_us;
            s_read_seq     = read_seq;
            s_playing      = true;
            s_last_slip_us = esp_timer_get_time();
            first_n        = s_slots[read_seq % UA_SLOTS].nsamples;
            xSemaphoreGive(s_lock);

            ESP_LOGI(TAG, "playing: seq=%u nsamples=%u offset=%lldms",
                     (unsigned)read_seq, (unsigned)first_n,
                     (long long)(offset_us / 1000));

            /* 等到该出声的时刻。最后 20ms 忙等，保证对齐精度 */
            for (;;)
            {
                int64_t now = esp_timer_get_time();
                int64_t gap = start_at - now;

                if (!s_running || gap <= 0)
                {
                    break;
                }
                if (gap > 20000)
                {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
            }
            last_n      = 0;
            first_frame = true;
            start_logged = false;
            start_at_local = start_at;
            continue;
        }

        /* ---------- 阶段二：取下一帧 ---------- */
        uint16_t seq;
        size_t   n      = 0;
        bool     have   = false;
        int64_t  t_play = 0;
        int64_t  level_us = 0;
        int64_t  obs_avg = 0;
        int64_t  obs_ref = 0;
        bool     slip_ready = false;
        int64_t  last_rx;
        int64_t  now = esp_timer_get_time();

        xSemaphoreTake(s_lock, portMAX_DELAY);
        seq     = s_read_seq;
        last_rx = s_last_rx_us;

        if (ua_slot_valid_locked(seq))
        {
            ua_slot_t *sl = &s_slots[seq % UA_SLOTS];
            n      = sl->nsamples;
            t_play = (int64_t)sl->t_play_us;
            memcpy(frame, sl->pcm, n * sizeof(int16_t));
            have     = true;
            level_us = s_last_t_play_us - t_play;

            /* 跑道观测值；开播 2~3 秒后把均值定为基准，之后只看它相对
             * 基准的变化量（见文件头"漂移怎么办"）。 */
            int64_t obs_us = level_us;

            if (s_obs_samples == 0)
            {
                s_obs_avg_us = obs_us;
            }
            else
            {
                s_obs_avg_us += (obs_us - s_obs_avg_us) >> UA_LEVEL_EMA_SHIFT;
            }
            s_obs_samples++;
            if (!s_slip_ready && s_obs_samples >= UA_SLIP_WARMUP)
            {
                s_slip_ready = true;
                s_obs_ref_us = s_obs_avg_us;
                ESP_LOGI(TAG, "drift reference: level=%lldms",
                         (long long)(s_obs_ref_us / 1000));
            }
        }

        /* 统计始终反映最近一次有效观测（PLC 帧不更新 EMA） */
        obs_avg    = s_obs_avg_us;
        obs_ref    = s_obs_ref_us;
        slip_ready = s_slip_ready;
        xSemaphoreGive(s_lock);

        /* ---------- 断流太久：清盘重新等 ---------- */
        if (!have && (now - last_rx) > UA_UNDERRUN_RESET_US)
        {
            ESP_LOGW(TAG, "no data for %lldms, re-preroll",
                     (long long)((now - last_rx) / 1000));

            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_playing   = false;
            s_first_seq = s_write_seq;      /* 新数据从下一个包重新算起 */
            s_obs_samples = 0;              /* 重新 preroll 会重算 offset，基准作废 */
            s_slip_ready  = false;
            for (int i = 0; i < UA_SLOTS; i++)
            {
                s_slots[i].valid = 0;
            }
            xSemaphoreGive(s_lock);

            last_n = 0;
            continue;
        }

        /* ---------- 该帧没到：PLC 补一帧，seq 照常前进 ---------- */
        if (!have)
        {
            if (last_n > 0)
            {
                ua_write_plc(last_good, last_n, UINT32_MAX);
            }
            else
            {
                /* 还没有任何有效帧可参考：补 10ms 静音 */
                memset(slip_buf, 0, 441 * sizeof(int16_t));
                Amplifier_Play_Buffer((const uint8_t *)slip_buf,
                                      441 * sizeof(int16_t), NULL, UINT32_MAX);
            }
            s_lost_pkts++;

            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_read_seq = (uint16_t)(s_read_seq + 1);
            xSemaphoreGive(s_lock);
            continue;
        }

        /* ---------- 缓冲太深：跳到较新的位置，别让延迟无限涨 ---------- */
        if (level_us > UA_LEVEL_MAX_US)
        {
            uint16_t target;

            xSemaphoreTake(s_lock, portMAX_DELAY);
            bool found = ua_target_seq_locked(UA_LEVEL_TARGET_US, &target);
            if (found)
            {
                s_read_seq = target;
                /* 跳帧把跑道整体砍短，基准要重新测 */
                s_obs_samples = 0;
                s_slip_ready  = false;
            }
            xSemaphoreGive(s_lock);

            if (found)
            {
                ESP_LOGW(TAG, "level %lldms too deep, jump to seq %u",
                         (long long)(level_us / 1000), (unsigned)target);
                continue;
            }
        }

        /* ---------- 开播淡入：压掉 DMA 从静音切到音频时的直流台阶 ----------
         * 只作用在本次开播的第一帧；之后 first_frame 一直为 false，
         * 直到下一次 preroll（换流 / 断流恢复）。 */
        if (first_frame)
        {
            size_t k = (n < UA_FADE_IN_SAMPLES) ? n : UA_FADE_IN_SAMPLES;

            for (size_t i = 0; i < k; i++)
            {
                frame[i] = (int16_t)((int32_t)frame[i] * (int32_t)i / (int32_t)k);
            }
            first_frame = false;
        }

        /* ---------- 漂移补偿：缓冲跑道相对"稳定基准"的变化 ----------
         * 从机晶振和发送端差几十 ppm，播放头会一点点脱离发送端的时刻表：
         *   跑道比基准长 → 从机消耗慢 → 丢一个样本，加快消耗
         *   跑道比基准短 → 从机消耗快 → 插一个样本，放慢消耗
         * 基准在开播 2~3 秒后自动测定（见上面的 warmup），不用去算 DMA 深度。
         * 控制量必须是"两个不同晶振的节拍之差"：写入时刻由从机 DMA 的
         * 节拍决定，每一帧 t_play 前进的数值又恰好等于 DMA 播完这一帧用的
         * 从机微秒数，所以"写入时刻 − 时刻表"恒为 −DMA 深度，看不见漂移。
         * 不去调 I2S 采样率（disable→reconfig→enable 会清空 DMA）。 */
        int slip = 0;
        if (slip_ready && (now - s_last_slip_us) > UA_SLIP_MIN_GAP_US)
        {
            if (obs_avg > obs_ref + UA_SLIP_DEADBAND_US)
            {
                slip = +1;      /* 跑道变长：丢一个样本追上去 */
            }
            else if (obs_avg < obs_ref - UA_SLIP_DEADBAND_US)
            {
                slip = -1;      /* 跑道变短：插一个样本等一等 */
            }
        }
        if (slip != 0)
        {
            s_last_slip_us = now;
            s_slip_cnt++;
        }

        /* ---------- 真正出声 ----------
         * timeout 传 UINT32_MAX：一直等到 DMA 有空位。这条阻塞就是
         * 从机晶振的节拍来源，比任何软件定时都准。 */
        int16_t prev_last = 0;
        bool    have_prev = (last_n > 0);
        if (have_prev)
        {
            prev_last = last_good[last_n - 1];   /* 上一次真正写进 DMA 的末样本 */
        }

        if (ua_write_frame(frame, n, slip, prev_last, have_prev,
                           slip_buf, UINT32_MAX) > 0)
        {
            memcpy(last_good, frame, n * sizeof(int16_t));
            last_n = n;

            if (!start_logged)
            {
                start_logged = true;
                ESP_LOGI(TAG, "first frame: seq=%u n=%u write=%lldms "
                              "level=%lldms",
                         (unsigned)seq, (unsigned)n,
                         (long long)((esp_timer_get_time() - start_at_local) / 1000),
                         (long long)(level_us / 1000));
            }
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_read_seq = (uint16_t)(s_read_seq + 1);
        xSemaphoreGive(s_lock);

        /* ---------- 周期状态 ---------- */
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (now_ms - last_stats_ms >= UA_STATS_INTERVAL_MS)
        {
            last_stats_ms = now_ms;
            ESP_LOGI(TAG, "rx=%lu lost=%lu late=%lu bad=%lu dup=%lu slip=%lu "
                          "level=%lldms dev=%lldms offset=%lldms",
                     (unsigned long)s_rx_pkts,
                     (unsigned long)s_lost_pkts,
                     (unsigned long)s_late_pkts,
                     (unsigned long)s_bad_pkts,
                     (unsigned long)s_dup_pkts,
                     (unsigned long)s_slip_cnt,
                     (long long)(obs_avg / 1000),
                     (long long)(slip_ready ? (obs_avg - obs_ref) / 1000 : 0),
                     (long long)(s_offset_us / 1000));
        }
    }

    free(frame);
    free(slip_buf);
    free(last_good);
    s_play_task = NULL;
    vTaskDelete(NULL);
}

/* ======================== API ======================== */

esp_err_t udp_audio_start(uint16_t port)
{
    if (s_running)
    {
        return ESP_OK;      /* 已经在跑了，重复调用当成功 */
    }
    if (Amplifier_Get_Volume() == 0)
    {
        ESP_LOGW(TAG, "volume is 0, output will be silent");
    }

    s_port = (port == 0) ? UDIO_DEFAULT_PORT : port;

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL)
    {
        ESP_LOGE(TAG, "mutex create failed");
        return ESP_ERR_NO_MEM;
    }

    s_slots = heap_caps_malloc(sizeof(ua_slot_t) * UA_SLOTS,
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_slots == NULL)
    {
        s_slots = malloc(sizeof(ua_slot_t) * UA_SLOTS);
    }
    if (s_slots == NULL)
    {
        ESP_LOGE(TAG, "jitter buffer alloc failed (%u bytes)",
                 (unsigned)(sizeof(ua_slot_t) * UA_SLOTS));
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    memset(s_slots, 0, sizeof(ua_slot_t) * UA_SLOTS);

    s_have_stream = false;
    s_playing     = false;
    s_rx_pkts = s_bad_pkts = s_lost_pkts = s_late_pkts = s_dup_pkts = s_slip_cnt = 0;
    s_running = true;

    if (xTaskCreate(ua_play_task, "udp_audio_play", UA_PLAY_TASK_STACK, NULL,
                    UA_PLAY_TASK_PRIO, &s_play_task) != pdPASS)
    {
        ESP_LOGE(TAG, "play task create failed");
        s_running = false;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(ua_rx_task, "udp_audio_rx", UA_RX_TASK_STACK, NULL,
                    UA_RX_TASK_PRIO, &s_rx_task) != pdPASS)
    {
        ESP_LOGE(TAG, "rx task create failed");
        s_running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void udp_audio_stop(void)
{
    s_running = false;
    /* 两个任务分别在 recvfrom / vTaskDelay 上超时后自行退出 */
}
