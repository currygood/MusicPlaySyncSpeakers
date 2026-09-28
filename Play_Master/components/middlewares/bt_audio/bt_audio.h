#ifndef __BT_AUDIO_H__
#define __BT_AUDIO_H__


#include "esp_err.h"

/* 句柄 */
typedef struct bt_audio_s *bt_audio_handle_t;

/* PCM 方向 */
typedef enum {
    BT_AUDIO_PCM_SOURCE_A2DP,         /* 手机音乐，16bit 双声道 */
    BT_AUDIO_PCM_SOURCE_HFP_DOWNLINK, /* 手机语音助手应答，16bit SCO PCM */
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
    const char *device_name;
    uint32_t    a2dp_fifo_bytes;
    uint32_t    hfp_fifo_bytes;
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

/* HFP 语音助手（麦克风上行） */
esp_err_t bt_audio_hfp_start_voice(bt_audio_handle_t audio);
esp_err_t bt_audio_hfp_stop_voice(bt_audio_handle_t audio);
esp_err_t bt_audio_hfp_send_pcm(bt_audio_handle_t audio,
                                const int16_t *pcm, size_t samples,
                                uint32_t timeout_ms);
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
