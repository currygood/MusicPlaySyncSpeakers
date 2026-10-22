/**
 * @file udp_audio.h
 * @brief 单播 UDP 音频接收 + 主从同步播放（协议 v2）
 *
 * 设计原则（对应《MVP 通信架构 - UDP 音频接收.md》第二章）：
 *   - **数据面只走单播**：音频永远点对点发，不用组播（组播在 Wi-Fi 上无 ACK、
 *     无重传、强制基本速率、AP 按 DTIM 缓存，见文档里的对照表）；
 *   - **发送端提前量 + 播放时刻戳**：主节点给每一帧打上 `t_play_us`，从节点
 *     按时间戳出声，而不是"收到就播"；
 *   - **抖动缓冲**：按 seq 排序的环形槽位，吸收网络抖动与乱序；
 *   - **漂移用丢/插单样本补偿**（sample slip），不去动 I2S 采样率。
 *
 * 线格式（小端，20 字节头 + PCM）：
 *
 *     offset 0   uint16 magic        固定 0xAA55
 *     offset 2   uint8  version      固定 2
 *     offset 3   uint8  flags        bit0 = 本次流的首帧
 *     offset 4   uint16 stream_id    一次流唯一标识（发送端重启会变）
 *     offset 6   uint16 seq          包序号，0~65535 回绕
 *     offset 8   uint64 t_play_us    该帧的基准播放时刻（**发送端时钟**，µs）
 *     offset 16  uint16 payload_len  PCM 字节数（不含头）
 *     offset 18  uint16 lead_ms      发送端提前量 D（ms），接收端换算时钟偏移要用
 *     offset 20  int16  pcm[]        44100Hz / 单声道 / 16bit 小端
 *
 * 时刻换算（推导见架构文档 4.2 节）：
 *
 *     d(i)   = 本地收到第 i 帧的时刻 − 该帧的 t_play_us
 *     d_min  = 最近一个窗口内 d(i) 的最小值           ≈ (从机时钟 − 主机时钟) − D + L_min
 *     offset = d_min + lead_ms × 1000 + UDIO_MARGIN_US
 *     出声时刻 = t_play_us + offset
 *
 * 发送端本地播放时刻是 `t_play_us + UDIO_MARGIN_US`（同一常数），
 * 因此两端残差只有 L_min（局域网通常 1~3ms），听不出来。
 *
 * 协议 v1（8 字节头、无时间戳）已废弃，Python 发送脚本必须同步升级。
 */
#ifndef __UDP_AUDIO_H__
#define __UDP_AUDIO_H__

#include <stdint.h>

#include "esp_err.h"

/* ======================== 线格式 ======================== */

/** 帧头魔数，与发送端共用 */
#define UDIO_MAGIC                  (0xAA55)

/** 协议版本。改动线格式必须递增，接收端会丢弃版本不符的包 */
#define UDIO_VERSION                (2)

/** 默认监听端口 */
#define UDIO_DEFAULT_PORT           (5680)

/** 固定音频格式：与 Amplifier_Init() 里 I2S 的配置一致 */
#define UDIO_SAMPLE_RATE            (44100)
#define UDIO_CHANNELS               (1)
#define UDIO_BITS                   (16)

/** flags 位定义 */
#define UDIO_FLAG_STREAM_START      (0x01)

/** 帧头（packed，20 字节）。`t_play_us` 落在 8 字节边界上，避免非对齐访问 */
typedef struct __attribute__((packed)) {
    uint16_t magic;         /* 固定 UDIO_MAGIC */
    uint8_t  version;       /* 固定 UDIO_VERSION */
    uint8_t  flags;         /* UDIO_FLAG_* */
    uint16_t stream_id;     /* 一次音频流唯一标识 */
    uint16_t seq;           /* 包序号，0~65535 回绕 */
    uint64_t t_play_us;     /* 基准播放时刻，发送端时钟（µs） */
    uint16_t payload_len;   /* PCM 字节数（不含头） */
    uint16_t lead_ms;       /* 发送端提前量 D（ms） */
} udio_hdr_t;

#define UDIO_HDR_SIZE               ((size_t)sizeof(udio_hdr_t))

/* ======================== 同步参数 ======================== */

/**
 * 接收端抖动余量（µs）。
 *
 * 接收端必须比"最早可能到达时刻"再晚一点才能保证每一帧都到齐，
 * 这个余量就是抖动缓冲深度；发送端本地播放也要加同一个值，
 * 否则从机整体比主机晚 MARGIN，同房间能听出回声。
 *
 * 60ms 对家用 Wi-Fi 非常宽松（实测抖动个位数 ms）。
 */
#define UDIO_MARGIN_US              (60000)

/* ======================== API ======================== */

/**
 * @brief 启动接收 + 播放链路（内部起接收、播放两个任务）
 *
 * 调用前必须先 Amplifier_Init()。
 *
 * @param port  监听端口，传 0 用 UDIO_DEFAULT_PORT
 * @return ESP_OK 任务已创建；ESP_ERR_NO_MEM / ESP_ERR_INVALID_STATE
 */
esp_err_t udp_audio_start(uint16_t port);

/**
 * @brief 停止接收任务并关闭 socket（调试用，正常流程不调用）
 */
void udp_audio_stop(void);

#endif /* __UDP_AUDIO_H__ */
