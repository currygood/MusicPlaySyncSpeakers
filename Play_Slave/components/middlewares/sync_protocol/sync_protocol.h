/**
 * @file sync_protocol.h
 * @brief 音频同步协议模块（middlewares 层，从节点侧）
 *
 * 架构依据（《从音频节点软件架构分层设计》）：
 *   - 从 wifi_manager 的组播通道收 PCM，按包头时间戳 + PING/PONG 估计的
 *     时钟偏移算出本地播放时刻，经 amplifier 写 I2S，与主节点同拍出声；
 *   - 本模块不持有 UDP socket：音频组播通道（239.0.0.1:5678）与控制组播
 *     通道（239.0.0.1:5679）由 app_main 经 wifi_manager_mcast_open() 创建后传入；
 *   - 从节点只有"收 → 缓冲 → 按时刻播"一条路径，没有音源侧。
 *
 * 主从分工（同一套线格式，两个方向）：
 *   - 主节点 → 从节点：音频包（5678）、SYNC_START / SYNC_HEARTBEAT / 基础命令（5679）；
 *   - 从节点 → 主节点：**PING（5679）**，主节点回 PONG，从节点据此估计时钟偏移。
 *
 * ⚠️ 本文件的"一线格式定义"区块与 Play_Master 的
 *    components/middlewares/sync_protocol/sync_protocol.h **逐字保持一致**，
 *    任何改动必须两个工程同步修改，否则主从不再互通。
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
    uint32_t sync_delay_ms;    /* D，默认 200ms，**必须与主节点一致** */
    uint32_t ping_interval_ms; /* PING 维护周期，默认 1000ms */
    uint32_t jitter_slots;     /* Jitter Buffer 槽数，默认 24（≈360ms） */
    uint32_t write_ahead_us;   /* 写提前量 WA，默认 15000（1 帧） */
    wifi_mcast_handle_t audio_chan;   /* 音频通道（239.0.0.1:5678，收） */
    wifi_mcast_handle_t control_chan; /* 控制通道（239.0.0.1:5679，收发） */
} sync_protocol_slave_cfg_t;

/* ======================== 状态快照 ================================================ */

typedef struct {
    sync_protocol_state_t state;
    uint16_t stream_id;
    uint32_t sample_rate;
    bool     clock_locked;      /* PING/PONG 已收敛，偏移可用 */
    int64_t  offset_us;         /* θ = 从节点时钟 − 主节点时钟 */
    int64_t  net_delay_us;      /* 单程网络延迟估计（最小 RTT 的一半） */
    int64_t  sync_error_us;     /* 本地写游标 − 主节点心跳写游标；正=从机领先 */
    uint32_t cur_sample_rate;   /* 当前 I2S 采样率（含校正，v1 恒为 44100） */
    uint32_t rx_pkts;           /* 累计收到的音频包 */
    uint32_t lost_pkts;         /* seq 缺口补静音的包数 */
    uint32_t late_pkts;         /* 迟到丢弃的包数 */
    uint32_t underrun_ms;       /* 累计欠载静音时长（ms） */
    uint32_t jitter_used;       /* 当前缓冲包数 */
    uint32_t jitter_cap;        /* 缓冲容量（槽数） */
    uint32_t ping_sent;         /* 累计发出的 PING 数 */
    uint32_t pong_recv;         /* 累计 t1 匹配成功的 PONG 数 */
} sync_protocol_slave_status_t;

/* ======================== 事件回调 ================================================ */

typedef enum {
    SYNC_EVT_STATE_CHANGED,
    SYNC_EVT_STREAM_CHANGED,   /* stream_id 变化/换源 */
    SYNC_EVT_CLOCK_LOCKED,     /* PING/PONG 首次收敛 */
    SYNC_EVT_ERROR,
} sync_protocol_event_id_t;

typedef struct {
    sync_protocol_event_id_t      id;
    sync_protocol_slave_status_t  status;   /* 事件附带当前状态快照 */
} sync_protocol_event_t;

typedef void (*sync_protocol_event_cb_t)(const sync_protocol_event_t *evt,
                                         void *user_ctx);

/* ======================== API ===================================================== */

esp_err_t sync_protocol_slave_init(const sync_protocol_slave_cfg_t *cfg);
esp_err_t sync_protocol_slave_deinit(void);

esp_err_t sync_protocol_slave_register_event_cb(sync_protocol_event_cb_t cb,
                                                void *user_ctx);

/* 开始接收与同步播放（起内部任务、开始发 PING 探测） */
esp_err_t sync_protocol_slave_start(void);
/* 停止接收与播放（清缓冲，保留时钟估计） */
esp_err_t sync_protocol_slave_stop(void);

esp_err_t sync_protocol_slave_get_status(sync_protocol_slave_status_t *status);

/* 便捷查询（供 app_main / StatusLed 轮询） */
bool sync_protocol_slave_is_playing(void);
uint32_t sync_protocol_slave_get_rx_pkts(void);

#ifdef __cplusplus
}
#endif

#endif /* SYNC_PROTOCOL_H */
