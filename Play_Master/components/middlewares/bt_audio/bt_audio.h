/**
 * @file bt_audio.h
 * @brief 经典蓝牙音频模块（A2DP Sink + HFP(HF) 语音助手 + AVRCP 控制）
 *
 * 模块职责：
 *   - A2DP Sink：接收手机音乐，由 ESP-IDF 解 SBC 为 PCM，
 *     经 on_pcm(BT_AUDIO_PCM_SOURCE_A2DP) 输出（16bit，采样率/声道见回调参数）；
 *   - HFP(HF)：与手机语音助手建立 SLC + SCO 链路。麦克风上行由应用层以
 *     44.1kHz 单声道 16bit 调 bt_audio_hfp_send_pcm() 喂入，模块内降采样到
 *     SCO 速率（mSBC 16k / CVSD 8k）发送；手机应答下行由
 *     on_pcm(BT_AUDIO_PCM_SOURCE_HFP_DOWNLINK) 输出统一 44.1kHz 单声道 16bit；
 *   - AVRCP：UI 控制手机播放（播放/暂停/切歌）与音量（绝对音量）；
 *     手机上报播放状态、曲目信息经 on_event 输出；支持时自动请求元数据。
 *
 * 使用约定：
 *   - on_pcm / on_event 均在 bt_audio 内部任务线程触发，协议栈线程绝不调用用户回调；
 *   - 各 API 线程安全；未 create 即调用返回 ESP_ERR_INVALID_STATE。
 */
#ifndef __BT_AUDIO_H__
#define __BT_AUDIO_H__


#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* 句柄 */
typedef struct bt_audio_s *bt_audio_handle_t;

/* PCM 方向 */
typedef enum {
    BT_AUDIO_PCM_SOURCE_A2DP,         /* 手机音乐，16bit 双声道 */
    BT_AUDIO_PCM_SOURCE_HFP_DOWNLINK, /* 手机语音应答：44.1kHz 单声道 16bit（模块内由 SCO 8k/16k 上采样） */
} bt_audio_pcm_source_t;

/* 控制命令（UI → 手机） */
typedef enum {
    BT_AUDIO_CMD_PLAY,
    BT_AUDIO_CMD_PAUSE,
    BT_AUDIO_CMD_TOGGLE_PLAY,
    BT_AUDIO_CMD_PREV,
    BT_AUDIO_CMD_NEXT,
    BT_AUDIO_CMD_VOLUME_UP,
    BT_AUDIO_CMD_VOLUME_DOWN,
} bt_audio_cmd_t;

/* 播放状态 / 歌曲信息 */
typedef enum {
    BT_AUDIO_PLAY_STATE_UNKNOWN = 0,
    BT_AUDIO_PLAY_STATE_PLAYING,
    BT_AUDIO_PLAY_STATE_PAUSED,
    BT_AUDIO_PLAY_STATE_STOPPED,
} bt_audio_play_state_t;

typedef struct {
    char     title[256];
    char     artist[128];
    char     album[128];
    uint32_t track_num;
    uint32_t total_tracks;
    uint32_t duration_ms;
} bt_audio_track_info_t;

/* A2DP 参数 */
typedef struct {
    bool     connected;
    uint32_t sample_rate;
    uint8_t  channels;
    uint8_t  bits_per_sample;
} bt_audio_a2dp_info_t;

/* 事件 */
typedef enum {
    BT_AUDIO_EVT_A2DP_CONNECTED,
    BT_AUDIO_EVT_A2DP_DISCONNECTED,
    BT_AUDIO_EVT_A2DP_STREAM_STARTED,
    BT_AUDIO_EVT_A2DP_STREAM_STOPPED,
    BT_AUDIO_EVT_A2DP_SAMPLE_RATE_CHANGED,
    BT_AUDIO_EVT_HFP_CONNECTED,
    BT_AUDIO_EVT_HFP_DISCONNECTED,
    BT_AUDIO_EVT_HFP_AUDIO_OPEN,
    BT_AUDIO_EVT_HFP_AUDIO_CLOSE,
    BT_AUDIO_EVT_PLAY_STATE_CHANGED,
    BT_AUDIO_EVT_TRACK_CHANGED,
    BT_AUDIO_EVT_TRACK_INFO,
    BT_AUDIO_EVT_VOLUME_CHANGED,
} bt_audio_event_id_t;

typedef struct {
    bt_audio_event_id_t id;
    uint8_t remote_addr[6];
    union {
        bt_audio_play_state_t play_state;
        bt_audio_track_info_t track_info;
        uint8_t               volume;
        uint32_t              sample_rate;
    } data;
} bt_audio_event_t;

/* 对外回调 */
typedef void (*bt_audio_pcm_cb_t)(bt_audio_pcm_source_t source,
                                  const int16_t *pcm, size_t samples,
                                  uint32_t sample_rate, void *user_ctx);
typedef void (*bt_audio_event_cb_t)(const bt_audio_event_t *evt, void *user_ctx);

/* 配置 */
typedef struct {
    const char *device_name;     /* 蓝牙广播名（默认 "MPS-Sync-Speaker"） */
    uint32_t    a2dp_fifo_bytes; /* A2DP PCM FIFO 大小，0=默认 24KB（数据缓冲在 PSRAM） */
    uint32_t    hfp_fifo_bytes;  /* HFP 上行 FIFO 大小，0=默认 16KB（下行固定 8KB） */
    bt_audio_pcm_cb_t on_pcm;
    void *pcm_ctx;
    bt_audio_event_cb_t on_event;
    void *event_ctx;
} bt_audio_cfg_t;

/* 生命周期与发现 */
bt_audio_handle_t bt_audio_create(const bt_audio_cfg_t *cfg);
esp_err_t         bt_audio_destroy(bt_audio_handle_t audio);
esp_err_t bt_audio_start(bt_audio_handle_t audio);
esp_err_t bt_audio_stop(bt_audio_handle_t audio);
esp_err_t bt_audio_set_device_name(bt_audio_handle_t audio, const char *name);
esp_err_t bt_audio_set_discoverable(bt_audio_handle_t audio, bool enable);

/* A2DP */
esp_err_t bt_audio_get_a2dp_info(bt_audio_handle_t audio, bt_audio_a2dp_info_t *info);

/* HFP 语音助手（用法：HFP_CONNECTED 事件 → start_voice → wait_audio_open
 * → 循环 send_pcm(麦克风 44.1k 单声道) → stop_voice；
 * 手机应答由 on_pcm(BT_AUDIO_PCM_SOURCE_HFP_DOWNLINK) 输出） */
esp_err_t bt_audio_hfp_start_voice(bt_audio_handle_t audio);
esp_err_t bt_audio_hfp_stop_voice(bt_audio_handle_t audio);
esp_err_t bt_audio_hfp_send_pcm(bt_audio_handle_t audio,
                                const int16_t *pcm, size_t samples,
                                uint32_t timeout_ms); /* FIFO 满等待上限，超时返回 ESP_ERR_TIMEOUT */
esp_err_t bt_audio_hfp_wait_audio_open(bt_audio_handle_t audio,
                                       uint32_t timeout_ms); /* 阻塞等待 SCO 打开 */
bool      bt_audio_hfp_is_audio_open(bt_audio_handle_t audio); /* 查询 SCO 是否已打开 */

/* AVRCP：UI 控制 */
esp_err_t bt_audio_send_ctrl_cmd(bt_audio_handle_t audio, bt_audio_cmd_t cmd);
esp_err_t bt_audio_get_play_state(bt_audio_handle_t audio, bt_audio_play_state_t *state);
esp_err_t bt_audio_get_track_info(bt_audio_handle_t audio,
                                  const bt_audio_track_info_t **info);

/* 音量 */
esp_err_t bt_audio_set_volume(bt_audio_handle_t audio, uint8_t volume);
uint8_t   bt_audio_get_volume(bt_audio_handle_t audio);

/* 调试 */
uint32_t bt_audio_get_pcm_dropped(bt_audio_handle_t audio, bt_audio_pcm_source_t source);

#endif
