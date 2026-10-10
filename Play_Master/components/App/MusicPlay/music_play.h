/**
 * @file music_play.h
 * @brief 音乐播放模块接口（App 层），对应《主音频节点软件架构分层设计》3.3.1
 *
 * 模块职责（分层设计 3.3.1；原 PlayMode 3.3.4 已并入本模块）：
 *   - 播放源、播放状态、音量、播放模式的唯一状态主人；
 *   - 蓝牙源驱动 bt_audio，本地源驱动 audio_decoder，
 *     同步控制走 sync_protocol_master_broadcast_cmd() / start/stop/pause/resume；
 *   - 自动源选择：蓝牙优先级最高——内部按 source_poll_ms 轮询 A2DP，
 *     连接→MUSIC_SOURCE_BT，断开→MUSIC_SOURCE_LOCAL（create 后首次轮询即判定）；
 *   - UI 触摸、自动/手动源选择、手机 AVRCP、本地解码事件都进入同一个事件队列，
 *     由内部 MusicPlay_Task 状态机串行处理；
 *   - 手机与 UI 同时按键时，UI 指令作为"期望"，蓝牙源的最终状态以 bt_audio
 *     事件为准，本地源以自身状态机为准。
 *
 * 依赖接线（App 初始化时完成）：
 *   - bt_audio 由 App 层创建后经 cfg.bt_audio 传入；其 on_pcm/on_event 由 App
 *     接线：on_pcm 直连 sync_protocol_master_push_pcm()（不经过本模块），
 *     on_event 胶水函数调用 music_play_on_bt_event()；
 *   - sync_protocol 所需组播通道由 App 层经 wifi_manager_mcast_open() 创建后传入，
 *     本模块 create() 内自行调用 sync_protocol_master_init()，destroy() 内 deinit；
 *   - 源切换：UI 手动走 music_play_set_source()，自动规则在模块内部，二者最终都
 *     由 MusicPlay_Task 串行执行（自动规则会在下一次轮询校正手动选择）。
 */

#ifndef __MUSIC_PLAY_H__
#define __MUSIC_PLAY_H__

#include "esp_err.h"
#include "audio_decoder.h"
#include "bt_audio.h"
#include "wifi_manager.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ======================== 类型定义 ================================================== */

/** 音乐播放句柄（不透明结构体） */
typedef struct music_play_s *music_play_handle_t;

/* 播放源 / 状态 */
typedef enum {
    MUSIC_SOURCE_NONE = 0,
    MUSIC_SOURCE_BT,
    MUSIC_SOURCE_LOCAL,
} music_source_t;

typedef enum {
    MUSIC_STATE_IDLE = 0,
    MUSIC_STATE_PLAYING,
    MUSIC_STATE_PAUSED,
    MUSIC_STATE_STOPPED,
    MUSIC_STATE_ERROR,
} music_state_t;

/* 播放模式（只支持两种） */
typedef enum {
    MUSIC_PLAY_MODE_SEQUENTIAL = 0,  /* 顺序播放，播完列表末尾回第一首 */
    MUSIC_PLAY_MODE_SINGLE_LOOP,     /* 单曲循环 */
} music_play_mode_t;

/* 命令 */
typedef enum {
    MUSIC_CMD_PLAY,
    MUSIC_CMD_PAUSE,
    MUSIC_CMD_RESUME,
    MUSIC_CMD_TOGGLE,
    MUSIC_CMD_NEXT,
    MUSIC_CMD_PREV,
    MUSIC_CMD_VOLUME_UP,
    MUSIC_CMD_VOLUME_DOWN,
} music_cmd_t;

/* 曲目 */
typedef enum {
    MUSIC_TRACK_FMT_MP3 = 0,
    MUSIC_TRACK_FMT_WAV,
} music_track_format_t;

typedef struct {
    music_track_format_t format;
    char path[128];
    char display_name[64];
    uint32_t duration_ms;
} music_track_t;

/* UI 可读状态快照 */
typedef struct {
    music_source_t     source;
    music_state_t      state;
    music_play_mode_t  mode;
    uint8_t            volume;
    uint16_t           track_index;
    uint16_t           track_count;
    const music_track_t *track;   /* BT 源指向内部 bt_track；本地源指向列表项；无则 NULL */
    bool               pending_cmd;
} music_play_view_t;

/* 事件 */
typedef enum {
    MUSIC_EVT_SOURCE_CHANGED,   /* 播放源变化（手动或自动切换） */
    MUSIC_EVT_PLAY_STATE_CHANGED,
    MUSIC_EVT_TRACK_CHANGED,
    MUSIC_EVT_TRACK_INFO,
    MUSIC_EVT_PLAYLIST_CHANGED,
    MUSIC_EVT_PLAY_MODE_CHANGED,
    MUSIC_EVT_VOLUME_CHANGED,
    MUSIC_EVT_EOF,
    MUSIC_EVT_ERROR,            /* 源选择参数非法 / A2DP 轮询失败 */
} music_event_id_t;

typedef struct {
    music_event_id_t  id;
    music_play_view_t view;   /* 事件携带整体状态快照 */
} music_event_t;

typedef void (*music_event_cb_t)(const music_event_t *evt, void *user_ctx);

/* 创建参数（含 App 层接线传入的句柄/通道） */
typedef struct {
    bt_audio_handle_t     bt_audio;          /* 蓝牙源句柄（App 创建传入） */
    wifi_mcast_handle_t   audio_chan;         /* 音频组播通道 239.0.0.1:5678（App 打开传入） */
    wifi_mcast_handle_t   control_chan;       /* 控制组播通道 239.0.0.1:5679（App 打开传入） */
    uint32_t              sync_delay_ms;      /* 同步延迟 D（ms），0=从 node_role 读取 */
    uint32_t              source_poll_ms;     /* 自动源选择轮询间隔（ms），0=默认 1000 */
    music_event_cb_t      on_event;           /* 播放器事件回调（UI 注册） */
    void                 *event_ctx;
} music_play_cfg_t;

/* ======================== API ========================================================= */

/* 生命周期 */
music_play_handle_t music_play_create(const music_play_cfg_t *cfg);
esp_err_t           music_play_destroy(music_play_handle_t player);

/* 控制 */
/* 手动源选择（UI 调用）：蓝牙优先自动规则仍在，连接期间切本地会被下一次轮询拉回 */
esp_err_t music_play_set_source(music_play_handle_t player, music_source_t source);
esp_err_t music_play_command(music_play_handle_t player, music_cmd_t cmd);

/* 查询当前播放源（UI 初始化/切页用） */
music_source_t music_play_get_source(music_play_handle_t player);

/* 本地列表 */
esp_err_t music_play_scan_local(music_play_handle_t player);
esp_err_t music_play_select_track(music_play_handle_t player, uint16_t index);
uint16_t  music_play_get_track_count(music_play_handle_t player);
const music_track_t *music_play_get_track(music_play_handle_t player, uint16_t index);

/* 播放模式 / 音量 */
esp_err_t music_play_set_mode(music_play_handle_t player, music_play_mode_t mode);
esp_err_t music_play_set_volume(music_play_handle_t player, uint8_t volume);
uint8_t   music_play_get_volume(music_play_handle_t player);

/* 状态查询 */
const music_play_view_t *music_play_get_view(music_play_handle_t player);

/* 播放进度（ms）：本地源取 audio_decoder，蓝牙源取 bt_audio（手机 PLAY_POS_CHANGED），
 * 未知时位置置 0（UI 据此隐藏进度条） */
esp_err_t music_play_get_position_ms(music_play_handle_t player, uint32_t *position_ms);

/* 内部事件入口（由 bt_audio / audio_decoder 调用，App 其余模块不直接使用） */
void music_play_on_bt_event(music_play_handle_t player, const bt_audio_event_t *evt);
void music_play_on_decoder_event(music_play_handle_t player,
                                 audio_decoder_event_id_t evt,
                                 const audio_decoder_info_t *info);

#ifdef __cplusplus
}
#endif

#endif /* __MUSIC_PLAY_H__ */
