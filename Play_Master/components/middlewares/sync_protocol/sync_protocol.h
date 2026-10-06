/**
 * @file sync_protocol.h
 * @brief 音频同步协议模块（middlewares 层，主节点侧）
 *
 * 架构依据（《主音频节点软件构架分层设计.md》3.2.8）：
 *   - 汇聚 bt_audio（A2DP/HFP 下行）与 audio_decoder（SD 卡）的 PCM，
 *     打时间戳后经 wifi_manager 组播通道发到从节点，并按
 *     t_play_master = t_tx + D 交回本机播放（主从同拍）；
 *   - 本模块不持有 UDP socket：音频组播通道（239.0.0.1:5678）与控制组播
 *     通道（239.0.0.1:5679）由 App 经 wifi_manager_mcast_open() 创建后传入；
 *   - 源无关：不感知蓝牙/SD/解码器类型，只认 stream_id + sample_rate + channels；
 *   - 最终出声（主节点本地播放）按第九阶段讨论决定：直接经 amplifier 的
 *     Amplifier_Play_Buffer() 与从节点同一时刻写 I2S。
 *
 * 组播协议（与《音频同步协议.md》一致，主从共用）：
 *   音频包（239.0.0.1:5678）：
 *     Magic(0xAA55,2) | StreamID(2) | Seq(2) | Timestamp(8,us) | DataLen(2) | PCM
 *     注意 Timestamp 采用 64bit 微秒，避免 32bit 约 71 分钟回绕（讨论定案）；
 *   控制命令（239.0.0.1:5679）：Magic + Cmd + Param
 *   时间同步扩展帧：PING/PONG、SYNC_START(0x12)、SYNC_HEARTBEAT(0x13)，
 *     命令码沿用《项目文档》建议的 0x12/0x13，PING/PONG 定 0x20/0x21；
 *     0x10/0x11 预留（SYNC_PREPARE/SYNC_READY）。
 */
#ifndef SYNC_PROTOCOL_H
#define SYNC_PROTOCOL_H

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

/** 音频组播组（音频同步协议：UDP 239.0.0.1） */
#define SYNC_PROTOCOL_MCAST_GROUP       "239.0.0.1"
/** 音频数据端口（音频同步协议格局） */
#define SYNC_PROTOCOL_MCAST_PORT_AUDIO  5678
/** 控制命令/时间同步端口（音频同步协议格局） */
#define SYNC_PROTOCOL_MCAST_PORT_CTRL   5679

/* ======================== 一线格式定义（主从共用，勿改动） ====================== */

/** 所有包共同 Magic 标志：0xAA55，用于过滤干扰 */
#define SYNC_MAGIC 0xAA55u

/**
 * @brief 音频数据包头部（16 字节）
 *
 * 相比《音频同步协议.md》v0.1 的 10 字节头，增加了 stream_id(2) 且
 * timestamp 扩为 64bit（iso：0xAA55 | stream_id | seq | ts(8) | len）。
 */
typedef struct __attribute__((packed)) {
    uint16_t magic;         /**< 0xAA55 */
    uint16_t stream_id;     /**< 一次音频流唯一标识（主节点重启/切歌检测） */
    uint16_t seq;           /**< 序列号，0~65535 循环，用于丢包检测 */
    uint64_t timestamp_us;  /**< 主节点发送时刻，esp_timer_get_time()，微秒 */
    uint16_t data_len;      /**< PCM 数据字节数（单声道 16bit），最大 1472-16 */
} sync_audio_pkt_hdr_t;

/** 音频包固定头部长度 */
#define SYNC_AUDIO_HDR_SIZE ((uint16_t)sizeof(sync_audio_pkt_hdr_t))

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
} sync_protocol_msg_type_t;

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
    uint32_t     ingress_bytes;    /* PCM 入环字节容量（16~32KB 起步） */
    wifi_mcast_handle_t audio_chan;   /* 音频通道（239.0.0.1:5678，只发） */
    wifi_mcast_handle_t control_chan; /* 控制通道（239.0.0.1:5679，收发） */
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
    int64_t          play_delta_us;  /* 本机出队播放与 t_play_master 的偏差（µs） */
    bool             slave_online;
    int32_t          last_slave_offset_us; /* 主节点无法单边计算，恒为 0，从机阶段填写 */
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

esp_err_t sync_protocol_master_broadcast_cmd(sync_protocol_cmd_t cmd,
                                             uint8_t param);
esp_err_t sync_protocol_master_get_status(sync_protocol_master_status_t *status);
uint32_t sync_protocol_master_get_tx_pkts(void);
uint32_t sync_protocol_master_get_dropped_pkts(void);

#ifdef __cplusplus
}
#endif

#endif /* SYNC_PROTOCOL_H */
