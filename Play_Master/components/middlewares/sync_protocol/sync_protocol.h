/**
 * @file sync_protocol.h
 * @brief 音频同步协议模块（middlewares 层，主节点侧）
 *
 * 架构依据（《主音频节点软件构架分层设计.md》3.2.8）：
 *   - 汇聚 bt_audio（A2DP/HFP 下行）与 audio_decoder（SD 卡）的 PCM，
 *     算出每帧的**基准播放时刻** t_play = t_tx + D，逐台从机单播发出；
 *     同一份 t_play 也决定本机什么时候出声，主从同拍；
 *   - 本模块不持有 UDP socket：音频通道（用于单播）与控制组播通道
 *     （239.0.0.1:5679）由 App 经 wifi_manager_mcast_open() 创建后传入；
 *   - 源无关：不感知蓝牙/SD/解码器类型，只认 stream_id + sample_rate + channels；
 *   - 最终出声（主节点本地播放）直接经 amplifier 的 Amplifier_Play_Buffer()
 *     与从节点同一时刻写 I2S。
 *
 * 数据面协议 v2（与 Play_Slave 的 udp_audio 逐字节一致，主从共用）：
 *   音频包（单播到从机 5680）：
 *     Magic(0xAA55,2) | Ver(1) | Flags(1) | StreamID(2) | Seq(2) |
 *     t_play_us(8) | PayloadLen(2) | LeadMs(2) | PCM
 *   接收端用 t_play_us 直接对齐出声时刻；时钟偏移由"最小单程延迟"单向估计，
 *   音频同步本身不需要从节点回包（PING/PONG 只在控制通道保留，已不参与音频同步）。
 *   t_play_us / 时间戳一律 64bit 微秒，避免 32bit 约 71 分钟回绕。
 *   控制命令（239.0.0.1:5679）：Magic + Cmd + Param
 *   控制扩展帧：SYNC_START(0x12)、SYNC_HEARTBEAT(0x13)、PING(0x20)/PONG(0x21)、
 *   ANNOUNCE(0x30，从节点 → 主节点，用于发现和在线检测)。
 */
#ifndef SYNC_PROTOCOL_H
#define SYNC_PROTOCOL_H

/* ======================== 是否打开同步协议模块调试打印 ======================== */
#define SYNC_PROTOCOL_DEBUG_LOGS    (1)

#include "esp_err.h"
#include "esp_timer.h"
#include "wifi_manager.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ======================== 组播通道常量 ========================================== */

/** 音频组播组（**仅控制通道仍在用**：START/HEARTBEAT/PONG 这类小控制帧） */
#define SYNC_PROTOCOL_MCAST_GROUP       "239.0.0.1"
/** 控制命令/时间同步端口 */
#define SYNC_PROTOCOL_MCAST_PORT_CTRL   5679

/**
 * 音频数据面的单播端口。
 *
 * 音频**不再走组播**：单播在 802.11 上有 ACK、重传和按客户端自适应速率，
 * 组播则无 ACK、强制基本速率、还会被 AP 按 DTIM 缓存。
 * 从节点在 5680 上监听（见 Play_Slave 的 udp_audio.h）。
 */
#define SYNC_UNICAST_PORT_AUDIO         5680

/** 单播从机数量上限。8 台 × 706kbps ≈ 5.6Mbps，再往上就该重新考虑组播了 */
#define SYNC_MAX_SLAVES                 8

/* ======================== 一线格式定义（主从共用，勿改动） ====================== */

/** 所有包共同 Magic 标志：0xAA55，用于过滤干扰 */
#define SYNC_MAGIC 0xAA55u

/** 音频包协议版本。改动线格式必须递增，接收端会丢弃版本不符的包 */
#define SYNC_PROTO_VERSION 2u

/** 音频包头 flags */
#define SYNC_AUDIO_FLAG_STREAM_START 0x01u

/**
 * @brief 音频数据包头部（20 字节，与 Play_Slave 的 udio_hdr_t 逐字节一致）
 *
 * 与 v1 的差别：`timestamp_us`（发送时刻）换成 `t_play_us`（**基准播放时刻**），
 * 并补上 version / flags / lead_ms。接收端靠 t_play_us 直接对齐出声时刻，
 * 不需要再做"发送时刻 + D"的换算。
 *
 * 字段布局保证 t_play_us 落在 8 字节边界上，避免 32 位平台的非对齐访问。
 */
typedef struct __attribute__((packed)) {
    uint16_t magic;         /**< 0xAA55 */
    uint8_t  version;       /**< SYNC_PROTO_VERSION */
    uint8_t  flags;         /**< SYNC_AUDIO_FLAG_* */
    uint16_t stream_id;     /**< 一次音频流唯一标识（主节点重启/切歌检测） */
    uint16_t seq;           /**< 序列号，0~65535 循环 */
    uint64_t t_play_us;     /**< 该帧的基准播放时刻（主节点 esp_timer，微秒） */
    uint16_t payload_len;   /**< PCM 数据字节数（单声道 16bit） */
    uint16_t lead_ms;       /**< 提前量 D（ms）。从节点换算时钟偏移要用，必须填 */
} sync_audio_pkt_hdr_t;

/** 音频包固定头部长度 */
#define SYNC_AUDIO_HDR_SIZE ((uint16_t)sizeof(sync_audio_pkt_hdr_t))

/**
 * 接收端抖动余量（微秒），**必须与 Play_Slave 的 UDIO_MARGIN_US 相同**。
 *
 * 主节点本地播放也要加这个值：从节点为了让抖动最大的包也能按时到齐，
 * 必然比"最早到达时刻"再晚一段；主节点不加，就会比从节点早出声，
 * 同一间屋子能听出回声。
 */
#define SYNC_RX_MARGIN_US 60000

/** 基础控制命令包（与控制通道 5679） */
typedef struct __attribute__((packed)) {
    uint16_t magic;  /**< 0xAA55 */
    uint8_t  cmd;    /**< sync_protocol_cmd_t */
    uint8_t  param;  /**< 参数（VOLUME 0~100 等） */
} sync_ctrl_basic_t;

/**
 * @brief 控制通道扩展帧（时间同步用）
 *
 * 与基础帧的关系：基础帧固定为 4 字节（Magic+Cmd+Param），扩展帧固定以
 * 本头开始，靠 Type 值区分（Type 不会与 cmd 0x01~0x06 重叠）。
 */
typedef struct __attribute__((packed)) {
    uint16_t magic;  /**< 0xAA55 */
    uint8_t  type;   /**< sync_protocol_msg_type_t */
    uint8_t  len;    /**< 后续载荷字节数（0~255） */
    uint8_t  data[]; /**< 载荷，见下方 PING/PONG/START/HEARTBEAT */
} sync_ctrl_ext_t;

/** 控制扩展帧类型 */
typedef enum {
    SYNC_MSG_START      = 0x12,  /**< 基准《项目文档》命令码建议：SYNC_START */
    SYNC_MSG_HEARTBEAT  = 0x13,  /**< 基准：SYNC_HEARTBEAT */
    SYNC_MSG_PING       = 0x20,  /**< 从节点 → 主节点（时钟探测） */
    SYNC_MSG_PONG       = 0x21,  /**< 主节点 → 从节点（PING 应答） */
    SYNC_MSG_ANNOUNCE   = 0x30,  /**< 从节点 → 主节点（上线公告 / 周期心跳） */
} sync_protocol_msg_type_t;

/* ======================== 从节点发现（ANNOUNCE） ================================= */

/** ANNOUNCE 载荷版本。线格式改动时递增，主节点丢弃版本不符的公告 */
#define SYNC_ANNOUNCE_VERSION   1u

/**
 * 配对令牌：主节点只接受令牌相同的从节点。
 *
 * 作用不是安全加密，而是防止同一局域网里"别人家的同款从节点"被自动
 * 拉进本机的播放组。默认值必须与 Play_Slave 的 ctl_rx.c 保持一致；
 * 将来做配网/配对 UI 时改成 NVS 字段即可。
 */
#define SYNC_ANNOUNCE_TOKEN     0x53594E43u   /* "SYNC" */

/** 多久收不到公告判定从节点离线（公告周期 3s，连续丢 3 包） */
#define SYNC_SLAVE_OFFLINE_TIMEOUT_MS   10000u

/**
 * @brief 从节点公告载荷（SYNC_MSG_ANNOUNCE，控制通道 5679）
 *
 * 从节点每 3 秒发一次，主节点据此维护"已连接从机"表：
 *   - MAC 是设备身份，IP 可能因 DHCP 变化，所以按 MAC 做 upsert；
 *   - IP 放在载荷里而不是取 recvfrom 源地址，是因为 wifi_manager 的
 *     组播接收接口为了兼容旧调用没有暴露源地址；
 *   - name 是 UTF-8 设备名（默认 "从机-XXXX"，由 MAC 末两字节生成）。
 */
typedef struct __attribute__((packed)) {
    uint8_t  ver;           /**< SYNC_ANNOUNCE_VERSION */
    uint8_t  mac[6];        /**< STA MAC，设备唯一身份 */
    uint8_t  ip[4];         /**< 当前 IPv4 地址（网络字节序，和 inet_pton 一致） */
    uint16_t audio_port;    /**< 音频接收端口（当前 5680） */
    uint8_t  volume;        /**< 当前音量 0~100 */
    int8_t   rssi;          /**< 从节点侧 WiFi RSSI（dBm），主节点无法单边测量 */
    uint8_t  flags;         /**< 保留，当前为 0 */
    uint32_t token;         /**< SYNC_ANNOUNCE_TOKEN */
    char     name[16];      /**< UTF-8 设备名，不足补 0，不保证 NUL 结尾 */
} sync_announce_payload_t;

/** 主节点维护的一台从节点（UI / 诊断用快照） */
typedef struct {
    uint8_t  mac[6];        /**< 设备 MAC */
    char     name[20];      /**< 设备名（已保证 NUL 结尾） */
    char     ip[16];        /**< 点分十进制 IP */
    uint16_t audio_port;    /**< 音频接收端口 */
    uint8_t  volume;        /**< 从节点上报的音量 */
    int8_t   rssi;          /**< 从节点上报的 RSSI */
    bool     online;        /**< 最近 SYNC_SLAVE_OFFLINE_TIMEOUT_MS 内收到过公告 */
    uint32_t last_seen_ms;  /**< 距最近一次公告的毫秒数（离线后继续增长） */
} sync_slave_info_t;

/** PING 载荷：t1 = 从节点发送时刻（从节点时间轴） */
typedef struct __attribute__((packed)) {
    uint64_t t1_us;
} sync_ping_payload_t;

/** PONG 载荷：回显 t1、t2=主节点收到 PING、t3=主节点发出 PONG（主节点时间轴） */
typedef struct __attribute__((packed)) {
    uint64_t t1_us;
    uint64_t t2_us;
    uint64_t t3_us;
} sync_pong_payload_t;

/** SYNC_START 载荷：主节点流启动通知 */
typedef struct __attribute__((packed)) {
    uint16_t stream_id;
    uint64_t t0_us;  /**< 第一个音频包的发送时间 */
    uint32_t d_ms;   /**< 统一同步缓冲 D（默认 200ms） */
} sync_start_payload_t;

/** SYNC_HEARTBEAT 载荷：每秒下发，从节点据此做漂移校正 */
typedef struct __attribute__((packed)) {
    uint16_t stream_id;
    uint64_t master_pts_us;    /**< 主节点正在播放的音频 PTS（主节点时间轴） */
    uint64_t t_master_now_us;  /**< 主节点当前本地时间（esp_timer） */
} sync_heartbeat_payload_t;

/* ======================== 状态枚举 =============================================== */

typedef enum {
    SYNC_STATE_IDLE = 0,
    SYNC_STATE_PREPARING,   /* 已 start，等待灌入 D 缓冲 */
    SYNC_STATE_PLAYING,
    SYNC_STATE_PAUSED,
    SYNC_STATE_STOPPED,
    SYNC_STATE_ERROR,
} sync_protocol_state_t;

/* 控制命令（同步广播从节点） */
typedef enum {
    SYNC_CMD_PLAY     = 0x01,
    SYNC_CMD_PAUSE    = 0x02,
    SYNC_CMD_NEXT     = 0x03,
    SYNC_CMD_PREV     = 0x04,
    SYNC_CMD_VOLUME   = 0x05,
    SYNC_CMD_RESET    = 0x06,
} sync_protocol_cmd_t;

/* ======================== 配置 ==================================================== */

typedef struct {
    uint32_t     sync_delay_ms;    /* D，默认 200ms */
    uint32_t     frame_duration_ms; /* 每包 PCM 时长（毫秒），默认 15ms */
    uint32_t     delay_queue_pkts; /* 本地延迟队列容量，> D/帧时长 */
    uint32_t     ingress_bytes;    /* PCM 入环字节容量（默认 64KB，PSRAM） */
    wifi_mcast_handle_t audio_chan;   /* 音频 socket（用于**单播**，rx 可关） */
    wifi_mcast_handle_t control_chan; /* 控制通道（239.0.0.1:5679，收发） */

    /* 单播目标从机列表。传 NULL / count=0 时用模块内置的编译期默认列表；
     * 运行时还可以用 sync_protocol_master_set_slaves() 覆盖。 */
    const char *const *slave_ips;
    uint8_t            slave_count;
} sync_protocol_master_cfg_t;

/* ======================== 状态快照 ================================================ */

typedef struct {
    sync_protocol_state_t state;
    uint16_t         stream_id;
    uint32_t         sample_rate;
    uint8_t          channels;
    uint32_t         frame_seq;      /* 最近发送包的 seq（统计窗口 65536 回绕） */
    uint32_t         tx_pkts;        /* 累计发送音频帧数 */
    uint32_t         dropped_pkts;   /* 入环满丢弃 + 发送失败累计 */
    uint32_t         ingress_used;   /* ingress 已用字节 */
    uint32_t         delay_queue_used; /* 本地延迟队列已用包数 */
    uint32_t         silence_frames;   /* 延迟队列露底时补的静音帧数（每帧 =
                                        * cfg_frame_ms）：持续增长就是"卡顿"本身，
                                        * 停止增长说明供数已经跟上 */
    int64_t          play_delta_us;  /* 本机出队播放与 t_play_master 的偏差（µs） */
    bool             slave_online;
    int32_t          last_slave_offset_us; /* 主节点无法单边计算，恒为 0，从机阶段填写 */
    uint8_t          slave_targets;   /* 当前单播目标数（发现后 = 在线从机数） */
    uint8_t          slaves_online;   /* ANNOUNCE 在线从机数 */
} sync_protocol_master_status_t;

/* ======================== 事件回调 ================================================ */

typedef enum {
    SYNC_EVT_STATE_CHANGED,
    SYNC_EVT_STREAM_CHANGED,   /* stream_id 变化/换源 */
    SYNC_EVT_SLAVE_SYNCED,     /* 收到第一个成功 PING 应答链路 */
    SYNC_EVT_SLAVE_LOST,       /* 长时间未收到从节点 PING */
    SYNC_EVT_ERROR,
} sync_protocol_event_id_t;

typedef struct {
    sync_protocol_event_id_t      id;
    sync_protocol_master_status_t status;   /* 事件附带当前状态快照 */
} sync_protocol_event_t;

typedef void (*sync_protocol_event_cb_t)(const sync_protocol_event_t *evt,
                                         void *user_ctx);

/* ======================== API ===================================================== */

esp_err_t sync_protocol_master_init(const sync_protocol_master_cfg_t *cfg);
esp_err_t sync_protocol_master_deinit(void);

esp_err_t sync_protocol_master_register_event_cb(
        sync_protocol_event_cb_t cb, void *user_ctx);

/* 启动一路流；换源传新 stream_id（0 则自动生成） */
esp_err_t sync_protocol_master_start(uint16_t stream_id,
                                     uint32_t sample_rate,
                                     uint8_t  channels);
esp_err_t sync_protocol_master_stop(void);

/* 所有音源 PCM 统一入口（线程安全、非阻塞） */
esp_err_t sync_protocol_master_push_pcm(const int16_t *pcm, size_t samples);

esp_err_t sync_protocol_master_pause(void);
esp_err_t sync_protocol_master_resume(void);

esp_err_t sync_protocol_master_set_delay_ms(uint32_t delay_ms);
esp_err_t sync_protocol_master_set_sample_rate(uint32_t sample_rate);
esp_err_t sync_protocol_master_set_channels(uint8_t channels);

/**
 * @brief 运行时设置单播目标从机列表（最多 SYNC_MAX_SLAVES 个）
 *
 * 手动设置只在"还没收到过任何 ANNOUNCE"时长期有效；一旦从节点开始公告，
 * 单播目标会由发现表按在线状态自动重建，手动表只作为过渡（最长 3 秒）。
 */
esp_err_t sync_protocol_master_set_slaves(const char *const *ips, uint8_t count);

/**
 * @brief 读取"已发现从机"快照（供 LCD 设置页 1s 轮询）
 *
 * 表按 MAC 去重、按发现顺序排列；离线条目会保留（灰显用），最多
 * SYNC_MAX_SLAVES 条。新设备在表满时会顶掉最早离线的条目。
 *
 * @param out       输出数组，调用方分配
 * @param capacity  out 的容量（通常传 SYNC_MAX_SLAVES）
 * @param count     输出实际条数（可为 NULL）
 * @return ESP_OK；未初始化/参数非法返回错误
 */
esp_err_t sync_protocol_master_get_slaves(sync_slave_info_t *out,
                                          uint8_t capacity,
                                          uint8_t *count);

esp_err_t sync_protocol_master_broadcast_cmd(sync_protocol_cmd_t cmd,
                                             uint8_t param);
esp_err_t sync_protocol_master_get_status(sync_protocol_master_status_t *status);
uint32_t sync_protocol_master_get_tx_pkts(void);
uint32_t sync_protocol_master_get_dropped_pkts(void);

#ifdef __cplusplus
}
#endif

#endif /* SYNC_PROTOCOL_H */
