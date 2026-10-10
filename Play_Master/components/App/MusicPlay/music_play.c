/**
 * @file music_play.c
 * @brief 音乐播放模块实现（App 层），对应《主音频节点软件架构分层设计》3.3.1
 *
 * 实现要点：
 *   1. MusicPlay_Task 内部自建（任务表：优先级 9 / 栈 4096 / Core 1），
 *      UI 命令、自动/手动源选择、bt_audio 事件、audio_decoder 事件统一进
 *      同一个命令队列，由任务内状态机串行处理；
 *   2. 本地源：scan_local 扫 /sdcard/music 下的 .mp3（audio_decoder 当前仅
 *      支持 MP3），select_track 打开解码器，READY 后 sync_protocol_master_start，
 *      解码 PCM 经 on_pcm 回调直送 sync_protocol_master_push_pcm()；
 *   3. 蓝牙源：控制命令全部经 bt_audio_send_ctrl_cmd()，状态以 bt_audio
 *      事件为准（pending_cmd 表示 UI 指令尚未被事件回执确认）；
 *   4. 音量：功放 + sync 广播 + （蓝牙源）bt_audio_set_volume 三元同步，
 *      持久化到 node_role，create() 时恢复；播放模式同样读写 node_role；
 *   5. 自动源选择（原 PlayMode 并入）：任务内按 source_poll_ms 轮询
 *      bt_audio_get_a2dp_info()，A2DP 连接→MUSIC_SOURCE_BT、断开→MUSIC_SOURCE_LOCAL，
 *      仅在源变化时切换（create 后首次轮询即判定，开机未连蓝牙→本地）；
 *   6. sync_protocol 初始化/反初始化在本模块 create()/destroy() 完成，
 *      所需两个组播好消息由 App 层经 wifi_manager_mcast_open() 传入。
 *
 * 依赖接口（均为项目内既有头文件真实签名）：
 *   - bt_audio.h：bt_audio_send_ctrl_cmd / bt_audio_set_volume /
 *     bt_audio_get_play_state / bt_audio_get_a2dp_info / bt_audio_get_position_ms
 *   - audio_decoder.h：audio_decoder_init/open/close/play/pause/
 *     get_info/get_position_ms
 *   - sync_protocol.h：master_init/deinit/start/stop/pause/resume/
 *     broadcast_cmd/push_pcm、SYNC_CMD_VOLUME
 *   - amplifier.h：Amplifier_Set_Volume
 *   - sd_card.h：SD_Card_Is_Mounted
 *   - node_role.h：node_role_get/set（音量、播放模式、同步延迟 D）
 */

#include "music_play.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "amplifier.h"
#include "audio_decoder.h"
#include "node_role.h"
#include "sd_card.h"
#include "sync_protocol.h"

/* ======================== 模块常量 ======================================== */

#define TAG "MusicPlay"

/** MusicPlay_Task（设计任务表：优先级 9 / 栈 4096 / Core 1） */
#define MP_TASK_NAME          "MusicPlay_Task"
#define MP_TASK_PRIO          9
#define MP_TASK_STACK         4096
#define MP_TASK_CORE          1

/** 命令队列深度（UI + 蓝牙事件 + 解码事件）
 * 注意：队列元素最大为 bt_audio_event_t（约 536B），8 条需 ~4.3KB 内部 RAM，
 * 在 wifi + BT + SD 都已初始化后容易分配失败（ESP32 内部堆紧张），故取 4。 */
#define MP_CMD_QUEUE_LEN      4

/** 入队超时（ms） */
#define MP_QUEUE_TIMEOUT_MS   100

/** 本地音乐目录（sd_card.h 约定 /sdcard/music） */
#define MP_MUSIC_DIR          "/sdcard/music"

/** 本地列表最大曲目数 */
#define MP_MAX_TRACKS         512

/** 同步流 stream_id：蓝牙/本地独立（sync_protocol.h 换源 stream_id 语义） */
#define MP_STREAM_ID_BT       1
#define MP_STREAM_ID_LOCAL    2

/** 音量步进（VOLUME_UP/DOWN） */
#define MP_VOLUME_STEP        5

/** 全链路约定采样率（amplifier.h：44100） */
#define MP_SAMPLE_RATE_44100  44100

/** 自动源选择轮询间隔默认值（ms）：cfg->source_poll_ms 为 0 时使用（原 PlayMode poll_interval_ms） */
#define MP_SOURCE_POLL_MS_DEFAULT  1000

/* ==================== 内部定义 ================================= */

/** 任务内部消息类型 */
typedef enum {
    MUSIC_MSG_CMD = 0,        /* 用户/UI 播放控制命令 */
    MUSIC_MSG_SWITCH_SOURCE,  /* 播放源切换（手动指令 / 自动轮询） */
    MUSIC_MSG_SCAN,           /* 重新扫描本地列表 */
    MUSIC_MSG_SELECT_TRACK,   /* 播放指定本地曲目 */
    MUSIC_MSG_SET_MODE,       /* 播放模式 */
    MUSIC_MSG_SET_VOLUME,     /* 音量 */
    MUSIC_MSG_BT_EVENT,       /* bt_audio 事件（回调线程拷贝入队） */
    MUSIC_MSG_DEC_EVENT,      /* audio_decoder 事件（解码线程拷贝入队） */
    MUSIC_MSG_DESTROY,        /* 销毁模块 */
} music_msg_type_t;

/** 消息负载（最大成员 bt_audio_event_t 含曲目信息 union） */
typedef struct {
    music_msg_type_t type;
    union {
        music_cmd_t        cmd;
        music_source_t     source;
        uint16_t           track_index;
        music_play_mode_t  mode;
        uint8_t            volume;
        bt_audio_event_t   bt_evt;
        struct {
            audio_decoder_event_id_t id;
            audio_decoder_info_t     info;
        } dec;
    } u;
} music_msg_t;

/** 模块实例（App 层，单实例由 App 创建） */
struct music_play_s
{
    music_play_cfg_t cfg;        /* 创建参数（bt_audio/组播通道/回调） */
    music_event_cb_t on_event;   /* 事件回调（引用 cfg 冗余字段） */
    void            *event_ctx;

    /* ---- 状态（Task 上下文写；读接口持锁） ---- */
    SemaphoreHandle_t lock;
    music_play_view_t view;      /* 权威视图 */
    music_play_view_t ui_view;   /* get_view() 返回快照 */

    /* ---- 本地曲目列表 ---- */
    music_track_t *tracks;
    uint16_t       track_count;

    /* ---- 蓝牙曲目槽（view.track 指向） ---- */
    music_track_t  bt_track;
    bool           bt_track_valid;

    audio_decoder_handle_t dec;  /* 当前本地解码器句柄 */
    bool              sync_ok;      /* sync_protocol_master_init 成功 */
    bool              sync_started; /* 当前 sync 流已 start */
    uint32_t          source_poll_ms;   /* 自动源选择轮询间隔（ms） */
    uint32_t          last_src_poll_ms; /* 上次轮询时刻（esp_timer ms；仅任务上下文访问） */

    QueueHandle_t     cmd_q;
    TaskHandle_t      task;
    SemaphoreHandle_t destroy_done;
};

/* ===================== 静态原型 =================================== */

static void Music_Emit(music_play_handle_t h, music_event_id_t id);
static void Music_Publish_View(music_play_handle_t h);

static void Sync_Start(music_play_handle_t h, uint16_t stream_id,
                       uint32_t sample_rate, uint8_t channels);
static void Sync_Stop(music_play_handle_t h);
static void Sync_Pause(music_play_handle_t h);
static void Sync_Resume(music_play_handle_t h);

static void Music_Apply_Volume(music_play_handle_t h, uint8_t volume,
                               bool sync_bt);
static void Music_On_Bt_Volume(music_play_handle_t h, uint8_t volume);
static void Music_Set_Mode_Msg(music_play_handle_t h, music_play_mode_t mode);

static void Music_Dec_Pcm_Cb(const int16_t *pcm, size_t samples,
                             uint32_t sample_rate, uint8_t channels,
                             void *user_ctx);
static void Music_Dec_Event_Bridge(audio_decoder_event_id_t evt,
                                   const audio_decoder_info_t *info,
                                   void *user_ctx);

static void Music_Close_Local(music_play_handle_t h);
static void Music_Start_Local_Track(music_play_handle_t h, uint16_t index);
static void Music_Pause_Local(music_play_handle_t h);
static void Music_Resume_Local(music_play_handle_t h);
static void Music_On_Local_Eof(music_play_handle_t h);
static void Music_On_Local_Error(music_play_handle_t h);

static void Music_BT_Enter_Playing(music_play_handle_t h);
static void Music_BT_Enter_Paused(music_play_handle_t h);
static void Music_BT_Enter_Stopped(music_play_handle_t h);
static void Music_Handle_Bt_Event(music_play_handle_t h,
                                  const bt_audio_event_t *evt);

static void Music_Switch_Source_Msg(music_play_handle_t h, music_source_t src);
static void Music_Poll_Source(music_play_handle_t h);
static void Music_Scan_Local_Msg(music_play_handle_t h);
static void Music_Handle_Msg(music_play_handle_t h, const music_msg_t *msg);
/* ==================== 事件与视图发布 ============================ */

/** 事件投递（锁内拷贝视图快照，回调在锁外执行） */
static void Music_Emit(music_play_handle_t h, music_event_id_t id)
{
    music_event_t evt;

    if (h->on_event == NULL)
    {
        return;
    }
    evt.id = id;
    xSemaphoreTake(h->lock, portMAX_DELAY);
    evt.view = h->view;
    xSemaphoreGive(h->lock);
    h->on_event(&evt, h->event_ctx);
}

/** 把权威视图同步到 get_view() 快照 */
static void Music_Publish_View(music_play_handle_t h)
{
    xSemaphoreTake(h->lock, portMAX_DELAY);
    h->ui_view = h->view;
    xSemaphoreGive(h->lock);
}

/* ---------------- sync 封装（主从同步：本地/蓝牙流） ---------------- */

static void Sync_Start(music_play_handle_t h, uint16_t stream_id,
                       uint32_t sample_rate, uint8_t channels)
{
    if (!h->sync_ok || h->sync_started)
    {
        return;
    }
    if (sync_protocol_master_start(stream_id, sample_rate, channels) == ESP_OK)
    {
        h->sync_started = true;
        ESP_LOGI(TAG, "sync start: stream=%u %luHz %uch",
                 stream_id, (unsigned long)sample_rate, channels);
    }
}

static void Sync_Stop(music_play_handle_t h)
{
    if (!h->sync_ok || !h->sync_started)
    {
        return;
    }
    sync_protocol_master_stop();
    h->sync_started = false;
    ESP_LOGI(TAG, "sync stop");
}

static void Sync_Pause(music_play_handle_t h)
{
    if (!h->sync_ok || !h->sync_started)
    {
        return;
    }
    sync_protocol_master_pause();
}

static void Sync_Resume(music_play_handle_t h)
{
    if (!h->sync_ok || !h->sync_started)
    {
        return;
    }
    sync_protocol_master_resume();
}

/* ---------------- 音量 / 播放模式 ---------------- */

/**
 * @brief 音量应用（用户/UI 触发）
 * @param sync_bt true=还要同步蓝牙绝对音量（UI 调）+ 持久化；
 *                蓝牙音量事件回调时传 false 避免 AVRCP 回环
 */
static void Music_Apply_Volume(music_play_handle_t h, uint8_t volume,
                               bool sync_bt)
{
    node_role_cfg_t nr;

    if (volume > 100)
    {
        volume = 100;
    }

    xSemaphoreTake(h->lock, portMAX_DELAY);
    h->view.volume = volume;
    xSemaphoreGive(h->lock);

    Amplifier_Set_Volume(volume);
    if (h->sync_ok)
    {
        sync_protocol_master_broadcast_cmd(SYNC_CMD_VOLUME, volume);
    }
    if (sync_bt && h->view.source == MUSIC_SOURCE_BT &&
        h->cfg.bt_audio != NULL)
    {
        bt_audio_set_volume(h->cfg.bt_audio, volume);
    }

    if (node_role_get(&nr) == ESP_OK)
    {
        nr.volume = volume;
        node_role_set(&nr);
    }

    Music_Emit(h, MUSIC_EVT_VOLUME_CHANGED);
    Music_Publish_View(h);
}

/** 蓝牙音量上报（手机侧调整）：回灌功放/同步/持久化，不再回写蓝牙 */
static void Music_On_Bt_Volume(music_play_handle_t h, uint8_t volume)
{
    node_role_cfg_t nr;

    if (volume > 100)
    {
        volume = 100;
    }

    xSemaphoreTake(h->lock, portMAX_DELAY);
    h->view.volume = volume;
    xSemaphoreGive(h->lock);

    Amplifier_Set_Volume(volume);
    if (h->sync_ok)
    {
        sync_protocol_master_broadcast_cmd(SYNC_CMD_VOLUME, volume);
    }
    if (node_role_get(&nr) == ESP_OK)
    {
        nr.volume = volume;
        node_role_set(&nr);
    }

    Music_Emit(h, MUSIC_EVT_VOLUME_CHANGED);
    Music_Publish_View(h);
}

static void Music_Set_Mode_Msg(music_play_handle_t h, music_play_mode_t mode)
{
    node_role_cfg_t nr;

    if (mode != MUSIC_PLAY_MODE_SEQUENTIAL &&
        mode != MUSIC_PLAY_MODE_SINGLE_LOOP)
    {
        return;
    }

    xSemaphoreTake(h->lock, portMAX_DELAY);
    h->view.mode = mode;
    xSemaphoreGive(h->lock);

    if (node_role_get(&nr) == ESP_OK)
    {
        nr.play_mode = (uint8_t)mode;
        node_role_set(&nr);
    }

    Music_Emit(h, MUSIC_EVT_PLAY_MODE_CHANGED);
    Music_Publish_View(h);
}

/* ---------------- 本地源解码回调 ---------------- */

/** 本地 MP3 解码 PCM → sync_protocol（仅本地播放态时推送，非阻塞丢帧由协议负责） */
static void Music_Dec_Pcm_Cb(const int16_t *pcm, size_t samples,
                             uint32_t sample_rate, uint8_t channels,
                             void *user_ctx)
{
    music_play_handle_t h = user_ctx;

    (void)sample_rate;
    (void)channels;

    if (h == NULL || !h->sync_ok)
    {
        return;
    }
    xSemaphoreTake(h->lock, portMAX_DELAY);
    bool push = (h->view.source == MUSIC_SOURCE_LOCAL &&
                 h->view.state == MUSIC_STATE_PLAYING);
    xSemaphoreGive(h->lock);
    if (push)
    {
        sync_protocol_master_push_pcm(pcm, samples);
    }
}

/** 解码器事件 → 事件队列（解码任务上下文调用） */
static void Music_Dec_Event_Bridge(audio_decoder_event_id_t evt,
                                   const audio_decoder_info_t *info,
                                   void *user_ctx)
{
    music_msg_t msg;
    music_play_handle_t h = user_ctx;

    if (h == NULL)
    {
        return;
    }
    memset(&msg, 0, sizeof(msg));
    msg.type = MUSIC_MSG_DEC_EVENT;
    msg.u.dec.id = evt;
    if (info != NULL)
    {
        msg.u.dec.info = *info;
    }
    xQueueSend(h->cmd_q, &msg, pdMS_TO_TICKS(MP_QUEUE_TIMEOUT_MS));
}
/* ==================== 本地播放流程 ============================ */

/** 停止本地源：sync 停流 + 关闭解码器 */
static void Music_Close_Local(music_play_handle_t h)
{
    Sync_Stop(h);
    if (h->dec != NULL)
    {
        audio_decoder_close(h->dec);
        h->dec = NULL;
    }
}

/** 打开指定本地曲目（关闭旧解码器；READY 事件异步驱动 sync 启动 + 播放） */
static void Music_Start_Local_Track(music_play_handle_t h, uint16_t index)
{
    audio_decoder_cfg_t dcfg;

    if (h->track_count == 0 || index >= h->track_count)
    {
        ESP_LOGE(TAG, "local start: bad index %u (count=%u)",
                 index, h->track_count);
        return;
    }

    Music_Close_Local(h);

    xSemaphoreTake(h->lock, portMAX_DELAY);
    h->view.track_index = index;
    h->view.track       = &h->tracks[index];
    h->view.pending_cmd = false;
    xSemaphoreGive(h->lock);

    memset(&dcfg, 0, sizeof(dcfg));
    dcfg.on_pcm    = Music_Dec_Pcm_Cb;
    dcfg.pcm_ctx   = h;
    dcfg.on_event  = Music_Dec_Event_Bridge;
    dcfg.event_ctx = h;

    h->dec = audio_decoder_open(h->tracks[index].path, &dcfg);
    if (h->dec == NULL)
    {
        ESP_LOGE(TAG, "open failed: %s", h->tracks[index].path);
        Music_On_Local_Error(h);
        return;
    }
    ESP_LOGI(TAG, "open track[%u]: %s", index, h->tracks[index].display_name);
}

/** 暂停本地源（解码器挂起 + sync 暂停，位置保持） */
static void Music_Pause_Local(music_play_handle_t h)
{
    if (h->dec != NULL)
    {
        audio_decoder_pause(h->dec);
    }
    Sync_Pause(h);

    xSemaphoreTake(h->lock, portMAX_DELAY);
    h->view.state = MUSIC_STATE_PAUSED;
    h->view.pending_cmd = false;
    xSemaphoreGive(h->lock);
    Music_Emit(h, MUSIC_EVT_PLAY_STATE_CHANGED);
    Music_Publish_View(h);
}

/** 恢复本地源（解码器从暂停点继续，sync 重新灌 D 缓冲） */
static void Music_Resume_Local(music_play_handle_t h)
{
    if (h->dec == NULL || h->view.state != MUSIC_STATE_PAUSED)
    {
        return;
    }
    audio_decoder_play(h->dec);
    Sync_Resume(h);

    xSemaphoreTake(h->lock, portMAX_DELAY);
    h->view.state = MUSIC_STATE_PLAYING;
    xSemaphoreGive(h->lock);
    Music_Emit(h, MUSIC_EVT_PLAY_STATE_CHANGED);
    Music_Publish_View(h);
}

/** 本地 EOF：单曲循环重播当前曲，顺序播放顺延下一首（末尾回第一首） */
static void Music_On_Local_Eof(music_play_handle_t h)
{
    uint16_t next;

    Music_Emit(h, MUSIC_EVT_EOF);

    if (h->view.mode == MUSIC_PLAY_MODE_SINGLE_LOOP)
    {
        next = h->view.track_index;
    }
    else
    {
        next = (uint16_t)((h->view.track_index + 1) % h->track_count);
    }
    Music_Start_Local_Track(h, next);
}

/** 本地解码 ERROR：进入 ERROR 态（选曲/指令可恢复） */
static void Music_On_Local_Error(music_play_handle_t h)
{
    Sync_Stop(h);
    if (h->dec != NULL)
    {
        audio_decoder_close(h->dec);
        h->dec = NULL;
    }

    xSemaphoreTake(h->lock, portMAX_DELAY);
    h->view.state = MUSIC_STATE_ERROR;
    h->view.pending_cmd = false;
    xSemaphoreGive(h->lock);
    Music_Emit(h, MUSIC_EVT_ERROR);
    Music_Publish_View(h);
}

/* ==================== 蓝牙源 ============================ */

/** 蓝牙进入播放：确保 sync 流已启动/恢复，状态置 PLAYING */
static void Music_BT_Enter_Playing(music_play_handle_t h)
{
    bt_audio_a2dp_info_t ai;
    uint32_t rate = MP_SAMPLE_RATE_44100;
    uint8_t  ch   = 2;

    if (h->cfg.bt_audio != NULL &&
        bt_audio_get_a2dp_info(h->cfg.bt_audio, &ai) == ESP_OK &&
        ai.sample_rate != 0)
    {
        rate = ai.sample_rate;
        if (ai.channels == 1 || ai.channels == 2)
        {
            ch = ai.channels;
        }
    }

    if (!h->sync_started)
    {
        Sync_Start(h, MP_STREAM_ID_BT, rate, ch);
    }
    else
    {
        Sync_Resume(h);
    }

    xSemaphoreTake(h->lock, portMAX_DELAY);
    h->view.state = MUSIC_STATE_PLAYING;
    h->view.pending_cmd = false;
    xSemaphoreGive(h->lock);
    Music_Emit(h, MUSIC_EVT_PLAY_STATE_CHANGED);
    Music_Publish_View(h);
}

static void Music_BT_Enter_Paused(music_play_handle_t h)
{
    Sync_Pause(h);

    xSemaphoreTake(h->lock, portMAX_DELAY);
    h->view.state = MUSIC_STATE_PAUSED;
    h->view.pending_cmd = false;
    xSemaphoreGive(h->lock);
    Music_Emit(h, MUSIC_EVT_PLAY_STATE_CHANGED);
    Music_Publish_View(h);
}

static void Music_BT_Enter_Stopped(music_play_handle_t h)
{
    Sync_Stop(h);

    xSemaphoreTake(h->lock, portMAX_DELAY);
    h->view.state = MUSIC_STATE_STOPPED;
    h->view.pending_cmd = false;
    xSemaphoreGive(h->lock);
    Music_Emit(h, MUSIC_EVT_PLAY_STATE_CHANGED);
    Music_Publish_View(h);
}

/** bt_audio 事件处理（任务上下文；源相关状态以 bt_audio 事件为准） */
static void Music_Handle_Bt_Event(music_play_handle_t h,
                                  const bt_audio_event_t *evt)
{
    switch (evt->id)
    {
    case BT_AUDIO_EVT_A2DP_STREAM_STARTED:
        if (h->view.source == MUSIC_SOURCE_BT)
        {
            Music_BT_Enter_Playing(h);
        }
        break;

    case BT_AUDIO_EVT_A2DP_STREAM_STOPPED:
        if (h->view.source == MUSIC_SOURCE_BT &&
            h->view.state == MUSIC_STATE_PLAYING)
        {
            Music_BT_Enter_Paused(h);
        }
        break;

    case BT_AUDIO_EVT_PLAY_STATE_CHANGED:
        if (h->view.source != MUSIC_SOURCE_BT)
        {
            break;
        }
        switch (evt->data.play_state)
        {
        case BT_AUDIO_PLAY_STATE_PLAYING:
            Music_BT_Enter_Playing(h);
            break;
        case BT_AUDIO_PLAY_STATE_PAUSED:
            Music_BT_Enter_Paused(h);
            break;
        case BT_AUDIO_PLAY_STATE_STOPPED:
            Music_BT_Enter_Stopped(h);
            break;
        default:
            break;
        }
        break;

    case BT_AUDIO_EVT_TRACK_CHANGED:
        /* 新曲：清空蓝牙曲目展示，等元数据 */
        xSemaphoreTake(h->lock, portMAX_DELAY);
        h->bt_track_valid = false;
        h->view.track     = NULL;
        h->view.pending_cmd = false;
        xSemaphoreGive(h->lock);
        Music_Emit(h, MUSIC_EVT_TRACK_CHANGED);
        Music_Publish_View(h);
        break;

    case BT_AUDIO_EVT_TRACK_INFO:
    {
        const bt_audio_track_info_t *ti = &evt->data.track_info;

        memset(&h->bt_track, 0, sizeof(h->bt_track));
        /* 蓝牙源无解码格式概念，UI 只读文本字段 */
        h->bt_track.format      = MUSIC_TRACK_FMT_MP3;
        h->bt_track.duration_ms = ti->duration_ms;
        snprintf(h->bt_track.path, sizeof(h->bt_track.path), "bt://");
        /* 手工按字节限长拼"标题 - 歌手"（避免 -Wformat-truncation） */
        if (ti->title[0] != '\0')
        {
            const char *title = ti->title;
            const char *artist = ti->artist;
            size_t room = sizeof(h->bt_track.display_name) - 1;
            size_t used = 0;
            size_t part;

            part = strlen(title);
            if (part > room)
            {
                part = room;
            }
            memcpy(h->bt_track.display_name, title, part);
            used = part;

            if (artist[0] != '\0' && used < room)
            {
                h->bt_track.display_name[used++] = ' ';
                h->bt_track.display_name[used++] = '-';
                h->bt_track.display_name[used++] = ' ';
                part = strlen(artist);
                if (part > room - used)
                {
                    part = room - used;
                }
                memcpy(&h->bt_track.display_name[used], artist, part);
                used += part;
            }
            h->bt_track.display_name[used] = '\0';
        }
        else if (ti->artist[0] != '\0')
        {
            size_t part = strlen(ti->artist);
            if (part >= sizeof(h->bt_track.display_name))
            {
                part = sizeof(h->bt_track.display_name) - 1;
            }
            memcpy(h->bt_track.display_name, ti->artist, part);
            h->bt_track.display_name[part] = '\0';
        }
        else
        {
            snprintf(h->bt_track.display_name,
                     sizeof(h->bt_track.display_name), "%s",
                     "蓝牙音频");
        }

        xSemaphoreTake(h->lock, portMAX_DELAY);
        h->bt_track_valid = true;
        h->view.track     = &h->bt_track;
        xSemaphoreGive(h->lock);
        Music_Emit(h, MUSIC_EVT_TRACK_INFO);
        Music_Publish_View(h);
        break;
    }

    case BT_AUDIO_EVT_VOLUME_CHANGED:
        Music_On_Bt_Volume(h, evt->data.volume);
        break;

    case BT_AUDIO_EVT_A2DP_CONNECTED:
        /* 源切换由自动轮询/手动指令决定；连接事件只清 pending */
        xSemaphoreTake(h->lock, portMAX_DELAY);
        h->view.pending_cmd = false;
        xSemaphoreGive(h->lock);
        break;

    case BT_AUDIO_EVT_A2DP_DISCONNECTED:
    {
        bool was_bt = false;

        xSemaphoreTake(h->lock, portMAX_DELAY);
        h->view.pending_cmd = false;
        if (h->view.source == MUSIC_SOURCE_BT)
        {
            was_bt = true;
            h->view.state = MUSIC_STATE_STOPPED;
        }
        xSemaphoreGive(h->lock);

        if (was_bt)
        {
            Sync_Stop(h);
            Music_Emit(h, MUSIC_EVT_PLAY_STATE_CHANGED);
            Music_Publish_View(h);
        }
        break;
    }

    default:
        /* HFP / 采样率变化等：本模块不关心 */
        break;
    }
}
/* ==================== 控制命令 ============================ */

/** 蓝牙源命令：经 bt_audio 发 AVRCP；UI 指令作为"期望"（pending_cmd） */
static void Music_Cmd_BT(music_play_handle_t h, music_cmd_t cmd)
{
    bt_audio_cmd_t bcmd;

    switch (cmd)
    {
    case MUSIC_CMD_PLAY:
    case MUSIC_CMD_RESUME:
        bcmd = BT_AUDIO_CMD_PLAY;
        break;
    case MUSIC_CMD_PAUSE:
        bcmd = BT_AUDIO_CMD_PAUSE;
        break;
    case MUSIC_CMD_TOGGLE:
        bcmd = BT_AUDIO_CMD_TOGGLE_PLAY;
        break;
    case MUSIC_CMD_NEXT:
        bcmd = BT_AUDIO_CMD_NEXT;
        break;
    case MUSIC_CMD_PREV:
        bcmd = BT_AUDIO_CMD_PREV;
        break;
    default:
        return;
    }

    xSemaphoreTake(h->lock, portMAX_DELAY);
    h->view.pending_cmd = true;
    xSemaphoreGive(h->lock);

    if (bt_audio_send_ctrl_cmd(h->cfg.bt_audio, bcmd) != ESP_OK)
    {
        ESP_LOGW(TAG, "bt ctrl cmd %d failed", (int)bcmd);
        xSemaphoreTake(h->lock, portMAX_DELAY);
        h->view.pending_cmd = false;
        xSemaphoreGive(h->lock);
    }
}

/** 本地源：状态机即时生效（本地源以自身状态机为准） */
static void Music_Cmd_Local(music_play_handle_t h, music_cmd_t cmd)
{
    switch (cmd)
    {
    case MUSIC_CMD_PLAY:
    case MUSIC_CMD_RESUME:
        if (h->view.state == MUSIC_STATE_PAUSED)
        {
            Music_Resume_Local(h);
        }
        else if (h->view.state != MUSIC_STATE_PLAYING)
        {
            if (h->track_count > 0)
            {
                uint16_t idx = (h->view.track_index < h->track_count)
                                   ? h->view.track_index : 0;
                Music_Start_Local_Track(h, idx);
            }
            else
            {
                ESP_LOGW(TAG, "local play: empty playlist, scan first");
            }
        }
        break;

    case MUSIC_CMD_PAUSE:
        Music_Pause_Local(h);
        break;

    case MUSIC_CMD_TOGGLE:
        if (h->view.state == MUSIC_STATE_PLAYING)
        {
            Music_Pause_Local(h);
        }
        else if (h->track_count > 0)
        {
            uint16_t idx = (h->view.track_index < h->track_count)
                               ? h->view.track_index : 0;
            Music_Start_Local_Track(h, idx);
        }
        else
        {
            ESP_LOGW(TAG, "local toggle: empty playlist");
        }
        break;

    case MUSIC_CMD_NEXT:
        if (h->track_count > 0)
        {
            Music_Start_Local_Track(
                h, (uint16_t)((h->view.track_index + 1) % h->track_count));
        }
        break;

    case MUSIC_CMD_PREV:
        if (h->track_count > 0)
        {
            uint16_t idx = (h->view.track_index == 0)
                               ? (uint16_t)(h->track_count - 1)
                               : (uint16_t)(h->view.track_index - 1);
            Music_Start_Local_Track(h, idx);
        }
        break;

    default:
        break;
    }
}

/* ==================== 源切换 / 扫描 ============================ */

/** 源切换（手动指令与自动轮询共用）：停旧源 → 切状态 → 起新源 */
static void Music_Switch_Source_Msg(music_play_handle_t h, music_source_t src)
{
    if (src != MUSIC_SOURCE_BT && src != MUSIC_SOURCE_LOCAL)
    {
        return;
    }
    if (h->view.source == src)
    {
        return;
    }

    switch (h->view.source)
    {
    case MUSIC_SOURCE_LOCAL:
        Music_Close_Local(h);
        break;
    case MUSIC_SOURCE_BT:
        Sync_Stop(h);
        break;
    default:
        break;
    }

    xSemaphoreTake(h->lock, portMAX_DELAY);
    h->view.source = src;
    h->view.pending_cmd = false;
    h->view.state = (src == MUSIC_SOURCE_LOCAL) ? MUSIC_STATE_IDLE
                                                : MUSIC_STATE_STOPPED;
    xSemaphoreGive(h->lock);
    Music_Emit(h, MUSIC_EVT_SOURCE_CHANGED);
    Music_Publish_View(h);

    if (src == MUSIC_SOURCE_LOCAL && h->track_count > 0)
    {
        uint16_t idx = (h->view.track_index < h->track_count)
                           ? h->view.track_index : 0;
        Music_Start_Local_Track(h, idx);
    }
    else if (src == MUSIC_SOURCE_BT && h->cfg.bt_audio != NULL)
    {
        bt_audio_play_state_t st;
        if (bt_audio_get_play_state(h->cfg.bt_audio, &st) == ESP_OK &&
            st == BT_AUDIO_PLAY_STATE_PLAYING)
        {
            Music_BT_Enter_Playing(h);
        }
    }
}

/**
 * @brief 自动源选择轮询（原 PlayMode 3.3.4 并入）：蓝牙优先级最高
 *
 * A2DP 连接→MUSIC_SOURCE_BT，断开→MUSIC_SOURCE_LOCAL；仅在源变化时切换。
 * 任务循环每 tick 调用一次，内部按 source_poll_ms 限频（create 后首次即判定，
 * 因此开机时未连蓝牙会自动落到本地源）。轮询失败发 MUSIC_EVT_ERROR。
 */
static void Music_Poll_Source(music_play_handle_t h)
{
    bt_audio_a2dp_info_t info;
    music_source_t       want;
    uint32_t             now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    if ((uint32_t)(now_ms - h->last_src_poll_ms) < h->source_poll_ms)
    {
        return;
    }
    h->last_src_poll_ms = now_ms;

    if (bt_audio_get_a2dp_info(h->cfg.bt_audio, &info) != ESP_OK)
    {
        ESP_LOGW(TAG, "poll: bt_audio_get_a2dp_info failed");
        Music_Emit(h, MUSIC_EVT_ERROR);
        return;
    }

    want = info.connected ? MUSIC_SOURCE_BT : MUSIC_SOURCE_LOCAL;
    if (want != h->view.source)
    {
        ESP_LOGI(TAG, "auto switch: a2dp %s -> source %s",
                 info.connected ? "connected" : "disconnected",
                 (want == MUSIC_SOURCE_BT) ? "BT" : "LOCAL");
        Music_Switch_Source_Msg(h, want);
    }
}

/** 扫描本地列表（仅 *.mp3；audio_decoder 仅支持 MP3） */
static void Music_Scan_Local_Msg(music_play_handle_t h)
{
    DIR *dir;
    struct dirent *ent;
    music_track_t *tracks = NULL;
    uint16_t count = 0;
    size_t cap = 0;

    if (!SD_Card_Is_Mounted())
    {
        ESP_LOGW(TAG, "scan: sd not mounted");
        goto done;
    }
    dir = opendir(MP_MUSIC_DIR);
    if (dir == NULL)
    {
        ESP_LOGW(TAG, "scan: open %s failed", MP_MUSIC_DIR);
        goto done;
    }
    while ((ent = readdir(dir)) != NULL)
    {
        size_t nlen = strlen(ent->d_name);
        if (nlen < 5)
        {
            continue;
        }
        /* 扩展名 .mp3（大小写不敏感） */
        if (ent->d_name[nlen - 4] != '.' ||
            tolower((unsigned char)ent->d_name[nlen - 3]) != 'm' ||
            tolower((unsigned char)ent->d_name[nlen - 2]) != 'p' ||
            tolower((unsigned char)ent->d_name[nlen - 1]) != '3')
        {
            continue;
        }
        if (count >= MP_MAX_TRACKS)
        {
            break;
        }
        if (count >= cap)
        {
            size_t ncap = (cap == 0) ? 8 : cap * 2;
            if (ncap > MP_MAX_TRACKS)
            {
                ncap = MP_MAX_TRACKS;
            }
            music_track_t *nb = realloc(tracks, ncap * sizeof(*nb));
            if (nb == NULL)
            {
                ESP_LOGE(TAG, "scan: realloc fail at %u", count);
                break;
            }
            tracks = nb;
            cap = ncap;
        }

        memset(&tracks[count], 0, sizeof(tracks[count]));
        tracks[count].format = MUSIC_TRACK_FMT_MP3;
        {
            /* 手工拼 /sdcard/music/xxx.mp3（避免 -Wformat-truncation） */
            size_t base_len = strlen(MP_MUSIC_DIR);
            size_t name_len = nlen;
            size_t room = sizeof(tracks[count].path) - 1 - base_len;
            char *dst_path = tracks[count].path;

            memcpy(dst_path, MP_MUSIC_DIR, base_len);
            dst_path[base_len] = '/';
            if (name_len > room)
            {
                name_len = room;
            }
            memcpy(&dst_path[base_len + 1], ent->d_name, name_len);
            dst_path[base_len + 1 + name_len] = '\0';
        }
        {
            size_t dlen = nlen - 4;
            if (dlen > sizeof(tracks[count].display_name) - 1)
            {
                dlen = sizeof(tracks[count].display_name) - 1;
            }
            memcpy(tracks[count].display_name, ent->d_name, dlen);
            tracks[count].display_name[dlen] = '\0';
        }
        count++;
    }
    closedir(dir);

done:
    xSemaphoreTake(h->lock, portMAX_DELAY);
    if (h->tracks != NULL)
    {
        free(h->tracks);
    }
    h->tracks = tracks;
    h->track_count = count;
    if (count > 0)
    {
        if (h->view.track_index >= count)
        {
            h->view.track_index = 0;
        }
        h->view.track = &h->tracks[h->view.track_index];
    }
    else
    {
        h->view.track_index = 0;
        h->view.track = NULL;
    }
    h->view.track_count = count;
    xSemaphoreGive(h->lock);

    ESP_LOGI(TAG, "scan: %u tracks under %s", count, MP_MUSIC_DIR);
    Music_Emit(h, MUSIC_EVT_PLAYLIST_CHANGED);
    Music_Publish_View(h);
}

/* ==================== 消息处理与任务 ============================ */

static void Music_Handle_Msg(music_play_handle_t h, const music_msg_t *msg)
{
    switch (msg->type)
    {
    case MUSIC_MSG_CMD:
        if (msg->u.cmd == MUSIC_CMD_VOLUME_UP)
        {
            uint8_t v = h->view.volume;
            Music_Apply_Volume(h,
                (uint8_t)((v > 100 - MP_VOLUME_STEP) ? 100 : v + MP_VOLUME_STEP),
                true);
        }
        else if (msg->u.cmd == MUSIC_CMD_VOLUME_DOWN)
        {
            uint8_t v = h->view.volume;
            Music_Apply_Volume(h,
                (uint8_t)((v < MP_VOLUME_STEP) ? 0 : v - MP_VOLUME_STEP),
                true);
        }
        else if (h->view.source == MUSIC_SOURCE_BT)
        {
            Music_Cmd_BT(h, msg->u.cmd);
        }
        else if (h->view.source == MUSIC_SOURCE_LOCAL)
        {
            Music_Cmd_Local(h, msg->u.cmd);
        }
        else
        {
            ESP_LOGW(TAG, "cmd before source selected");
        }
        break;

    case MUSIC_MSG_SWITCH_SOURCE:
        Music_Switch_Source_Msg(h, msg->u.source);
        break;

    case MUSIC_MSG_SCAN:
        Music_Scan_Local_Msg(h);
        break;

    case MUSIC_MSG_SELECT_TRACK:
        if (msg->u.track_index < h->track_count)
        {
            if (h->view.source != MUSIC_SOURCE_LOCAL)
            {
                /* 切到本地源：Music_Switch_Source_Msg 内部会按 view.track_index
                 * 自动起播。这里先把目标曲目落进 view，避免"切源起一次 +
                 * 下面再起一次"把同一曲目 open 两遍：两遍 open 会各发一个
                 * READY，MusicPlay 于是对同一个解码器调用两次
                 * audio_decoder_play，解码任务把多出来的 PLAY 当成打断命令
                 * 退出解码循环 —— 表现为只出一帧音频后彻底无声
                 * （第十一阶段实测：解码位置卡在 26ms）。 */
                xSemaphoreTake(h->lock, portMAX_DELAY);
                h->view.track_index = msg->u.track_index;
                h->view.track       = &h->tracks[msg->u.track_index];
                xSemaphoreGive(h->lock);
                Music_Switch_Source_Msg(h, MUSIC_SOURCE_LOCAL);
            }
            else
            {
                Music_Start_Local_Track(h, msg->u.track_index);
            }
        }
        else
        {
            ESP_LOGW(TAG, "select: bad index %u (count=%u)",
                     msg->u.track_index, h->track_count);
        }
        break;

    case MUSIC_MSG_SET_MODE:
        Music_Set_Mode_Msg(h, msg->u.mode);
        break;

    case MUSIC_MSG_SET_VOLUME:
        Music_Apply_Volume(h, msg->u.volume, true);
        break;

    case MUSIC_MSG_BT_EVENT:
        Music_Handle_Bt_Event(h, &msg->u.bt_evt);
        break;

    case MUSIC_MSG_DEC_EVENT:
        if (msg->u.dec.id == AUDIO_DECODER_EVT_READY)
        {
            if (h->view.track_index < h->track_count)
            {
                h->tracks[h->view.track_index].duration_ms =
                    msg->u.dec.info.duration_ms;
                xSemaphoreTake(h->lock, portMAX_DELAY);
                h->view.track = &h->tracks[h->view.track_index];
                xSemaphoreGive(h->lock);
            }
            ESP_LOGI(TAG, "READY: %luHz/%uch dur=%lums",
                     (unsigned long)msg->u.dec.info.sample_rate,
                     msg->u.dec.info.channels,
                     (unsigned long)msg->u.dec.info.duration_ms);
            if (msg->u.dec.info.sample_rate != MP_SAMPLE_RATE_44100)
            {
                ESP_LOGW(TAG, "track is %luHz, full-chain is 44.1kHz",
                         (unsigned long)msg->u.dec.info.sample_rate);
            }
            Sync_Start(h, MP_STREAM_ID_LOCAL, msg->u.dec.info.sample_rate,
                       msg->u.dec.info.channels);
            if (h->dec != NULL)
            {
                audio_decoder_play(h->dec);
            }
            xSemaphoreTake(h->lock, portMAX_DELAY);
            h->view.state = MUSIC_STATE_PLAYING;
            h->view.pending_cmd = false;
            xSemaphoreGive(h->lock);
            Music_Emit(h, MUSIC_EVT_TRACK_CHANGED);
            Music_Emit(h, MUSIC_EVT_PLAY_STATE_CHANGED);
            Music_Publish_View(h);
        }
        else if (msg->u.dec.id == AUDIO_DECODER_EVT_EOF)
        {
            Music_On_Local_Eof(h);
        }
        else if (msg->u.dec.id == AUDIO_DECODER_EVT_ERROR)
        {
            Music_On_Local_Error(h);
        }
        break;

    case MUSIC_MSG_DESTROY:
        break;

    default:
        break;
    }
}
/** MusicPlay_Task：事件队列出队，状态机串行处理 */
static void Music_Play_Task(void *arg)
{
    music_play_handle_t h = arg;
    music_msg_t msg;

    ESP_LOGI(TAG, "task started");
    for (;;)
    {
        if (xQueueReceive(h->cmd_q, &msg, pdMS_TO_TICKS(50)) == pdTRUE)
        {
            if (msg.type == MUSIC_MSG_DESTROY)
            {
                break;
            }
            Music_Handle_Msg(h, &msg);
        }
        Music_Poll_Source(h);   /* 自动源选择：每 tick 检查，内部按 source_poll_ms 限频 */
    }
    ESP_LOGI(TAG, "task exit");
    xSemaphoreGive(h->destroy_done);
    vTaskDelete(NULL);
}

/* ==================== 对外 API ============================ */

music_play_handle_t music_play_create(const music_play_cfg_t *cfg)
{
    music_play_handle_t h;
    node_role_cfg_t nr;
    uint8_t volume = NODE_ROLE_DEFAULT_VOLUME;
    uint8_t mode = MUSIC_PLAY_MODE_SEQUENTIAL;
    uint32_t delay_ms = 0;
    sync_protocol_master_cfg_t scfg;

    if (cfg == NULL || cfg->bt_audio == NULL ||
        cfg->audio_chan == NULL || cfg->control_chan == NULL)
    {
        ESP_LOGE(TAG, "create: invalid cfg (need bt_audio + 2 mcast chan)");
        return NULL;
    }

    h = calloc(1, sizeof(*h));
    if (h == NULL)
    {
        ESP_LOGE(TAG, "create: no mem");
        return NULL;
    }
    h->cfg       = *cfg;
    h->on_event  = cfg->on_event;
    h->event_ctx = cfg->event_ctx;
    h->source_poll_ms = (cfg->source_poll_ms != 0) ? cfg->source_poll_ms
                                                   : MP_SOURCE_POLL_MS_DEFAULT;

    h->lock         = xSemaphoreCreateMutex();
    h->destroy_done = xSemaphoreCreateBinary();
    h->cmd_q        = xQueueCreate(MP_CMD_QUEUE_LEN, sizeof(music_msg_t));
    if (h->lock == NULL || h->destroy_done == NULL || h->cmd_q == NULL)
    {
        ESP_LOGE(TAG, "create: queue/sem create failed: free=%u largest=%u",
                 heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        goto fail;
    }

    /* 从 node_role 恢复音量 / 播放模式 / 同步延迟 */
    if (node_role_get(&nr) == ESP_OK)
    {
        if (nr.volume <= 100)
        {
            volume = nr.volume;
        }
        if (nr.play_mode == 1)
        {
            mode = MUSIC_PLAY_MODE_SINGLE_LOOP;
        }
        if (cfg->sync_delay_ms == 0 && nr.sync_delay_ms != 0)
        {
            delay_ms = nr.sync_delay_ms;
        }
    }
    if (cfg->sync_delay_ms != 0)
    {
        delay_ms = cfg->sync_delay_ms;
    }

    h->view.volume      = volume;
    h->view.mode        = mode;
    h->view.source      = MUSIC_SOURCE_NONE;
    h->view.state       = MUSIC_STATE_IDLE;
    h->view.track_index = 0;
    h->view.track_count = 0;
    h->view.track       = NULL;
    h->view.pending_cmd = false;
    Amplifier_Set_Volume(volume);

    /* sync_protocol 由本模块接管初始化/反初始化 */
    memset(&scfg, 0, sizeof(scfg));
    scfg.audio_chan   = cfg->audio_chan;
    scfg.control_chan = cfg->control_chan;
    scfg.sync_delay_ms = delay_ms;
    if (sync_protocol_master_init(&scfg) == ESP_OK)
    {
        h->sync_ok = true;
        ESP_LOGI(TAG, "sync_protocol inited (D=%u ms)", delay_ms);
    }
    else
    {
        ESP_LOGE(TAG, "sync_protocol_master_init failed, sync disabled");
    }

    audio_decoder_init();   /* 解码注册表：可重复调用 */

    if (xTaskCreatePinnedToCore(Music_Play_Task, MP_TASK_NAME, MP_TASK_STACK, h,
                                MP_TASK_PRIO, &h->task, MP_TASK_CORE) != pdPASS)
    {
        ESP_LOGE(TAG, "create: task create failed");
        goto fail;
    }

    ESP_LOGI(TAG, "created (vol=%u mode=%u)", volume, mode);
    return h;

fail:
    if (h->sync_ok)
    {
        sync_protocol_master_deinit();
    }
    if (h->cmd_q != NULL)
    {
        vQueueDelete(h->cmd_q);
    }
    if (h->lock != NULL)
    {
        vSemaphoreDelete(h->lock);
    }
    if (h->destroy_done != NULL)
    {
        vSemaphoreDelete(h->destroy_done);
    }
    free(h);
    return NULL;
}

esp_err_t music_play_destroy(music_play_handle_t player)
{
    music_msg_t msg;

    if (player == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&msg, 0, sizeof(msg));
    msg.type = MUSIC_MSG_DESTROY;
    if (xQueueSend(player->cmd_q, &msg, pdMS_TO_TICKS(MP_QUEUE_TIMEOUT_MS))
            != pdTRUE)
    {
        ESP_LOGE(TAG, "destroy: queue send timeout");
        return ESP_ERR_TIMEOUT;
    }
    if (xSemaphoreTake(player->destroy_done, pdMS_TO_TICKS(3000)) != pdTRUE)
    {
        ESP_LOGW(TAG, "destroy: task exit timeout");
    }

    if (player->dec != NULL)
    {
        audio_decoder_close(player->dec);
        player->dec = NULL;
    }
    if (player->sync_ok)
    {
        sync_protocol_master_deinit();
    }
    vQueueDelete(player->cmd_q);
    vSemaphoreDelete(player->lock);
    vSemaphoreDelete(player->destroy_done);
    if (player->tracks != NULL)
    {
        free(player->tracks);
    }
    free(player);
    ESP_LOGI(TAG, "destroyed");
    return ESP_OK;
}

esp_err_t music_play_set_source(music_play_handle_t player,
                                music_source_t source)
{
    music_msg_t msg;

    if (player == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (source != MUSIC_SOURCE_BT && source != MUSIC_SOURCE_LOCAL)
    {
        /* 源参数非法：发 ERROR 事件（Music_Emit 内部先锁后回调，可跨线程调用） */
        ESP_LOGW(TAG, "set_source: invalid source %d", (int)source);
        Music_Emit(player, MUSIC_EVT_ERROR);
        return ESP_ERR_INVALID_ARG;
    }
    memset(&msg, 0, sizeof(msg));
    msg.type  = MUSIC_MSG_SWITCH_SOURCE;
    msg.u.source = source;
    return (xQueueSend(player->cmd_q, &msg, pdMS_TO_TICKS(MP_QUEUE_TIMEOUT_MS))
            == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

music_source_t music_play_get_source(music_play_handle_t player)
{
    music_source_t src = MUSIC_SOURCE_NONE;

    if (player != NULL)
    {
        xSemaphoreTake(player->lock, portMAX_DELAY);
        src = player->view.source;
        xSemaphoreGive(player->lock);
    }
    return src;
}

esp_err_t music_play_command(music_play_handle_t player, music_cmd_t cmd)
{
    music_msg_t msg;

    if (player == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(&msg, 0, sizeof(msg));
    msg.type  = MUSIC_MSG_CMD;
    msg.u.cmd = cmd;
    return (xQueueSend(player->cmd_q, &msg, pdMS_TO_TICKS(MP_QUEUE_TIMEOUT_MS))
            == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t music_play_scan_local(music_play_handle_t player)
{
    music_msg_t msg;

    if (player == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(&msg, 0, sizeof(msg));
    msg.type = MUSIC_MSG_SCAN;
    return (xQueueSend(player->cmd_q, &msg, pdMS_TO_TICKS(MP_QUEUE_TIMEOUT_MS))
            == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t music_play_select_track(music_play_handle_t player, uint16_t index)
{
    music_msg_t msg;

    if (player == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(&msg, 0, sizeof(msg));
    msg.type = MUSIC_MSG_SELECT_TRACK;
    msg.u.track_index = index;
    return (xQueueSend(player->cmd_q, &msg, pdMS_TO_TICKS(MP_QUEUE_TIMEOUT_MS))
            == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

uint16_t music_play_get_track_count(music_play_handle_t player)
{
    uint16_t count = 0;

    if (player != NULL)
    {
        xSemaphoreTake(player->lock, portMAX_DELAY);
        count = player->track_count;
        xSemaphoreGive(player->lock);
    }
    return count;
}

const music_track_t *music_play_get_track(music_play_handle_t player,
                                          uint16_t index)
{
    const music_track_t *t = NULL;

    if (player != NULL)
    {
        xSemaphoreTake(player->lock, portMAX_DELAY);
        if (index < player->track_count)
        {
            t = &player->tracks[index];
        }
        xSemaphoreGive(player->lock);
    }
    return t;
}

esp_err_t music_play_set_mode(music_play_handle_t player, music_play_mode_t mode)
{
    music_msg_t msg;

    if (player == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(&msg, 0, sizeof(msg));
    msg.type = MUSIC_MSG_SET_MODE;
    msg.u.mode = mode;
    return (xQueueSend(player->cmd_q, &msg, pdMS_TO_TICKS(MP_QUEUE_TIMEOUT_MS))
            == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t music_play_set_volume(music_play_handle_t player, uint8_t volume)
{
    music_msg_t msg;

    if (player == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(&msg, 0, sizeof(msg));
    msg.type = MUSIC_MSG_SET_VOLUME;
    msg.u.volume = volume;
    return (xQueueSend(player->cmd_q, &msg, pdMS_TO_TICKS(MP_QUEUE_TIMEOUT_MS))
            == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

uint8_t music_play_get_volume(music_play_handle_t player)
{
    uint8_t v = 0;

    if (player != NULL)
    {
        xSemaphoreTake(player->lock, portMAX_DELAY);
        v = player->view.volume;
        xSemaphoreGive(player->lock);
    }
    return v;
}

const music_play_view_t *music_play_get_view(music_play_handle_t player)
{
    if (player == NULL)
    {
        return NULL;
    }
    xSemaphoreTake(player->lock, portMAX_DELAY);
    player->ui_view = player->view;
    xSemaphoreGive(player->lock);
    return &player->ui_view;
}

esp_err_t music_play_get_position_ms(music_play_handle_t player,
                                     uint32_t *position_ms)
{
    if (player == NULL || position_ms == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *position_ms = 0;

    if (player->view.source == MUSIC_SOURCE_LOCAL)
    {
        if (player->dec != NULL)
        {
            return audio_decoder_get_position_ms(player->dec, position_ms);
        }
    }
    else if (player->view.source == MUSIC_SOURCE_BT &&
             player->cfg.bt_audio != NULL)
    {
        uint32_t pos = 0;
        if (bt_audio_get_position_ms(player->cfg.bt_audio, &pos) == ESP_OK)
        {
            *position_ms = pos;
        }
    }
    return ESP_OK;
}

/* ---------------- 内部事件入口（App 胶水调用） ---------------- */

void music_play_on_bt_event(music_play_handle_t player,
                            const bt_audio_event_t *evt)
{
    music_msg_t msg;

    if (player == NULL || evt == NULL)
    {
        return;
    }
    memset(&msg, 0, sizeof(msg));
    msg.type = MUSIC_MSG_BT_EVENT;
    msg.u.bt_evt = *evt;
    xQueueSend(player->cmd_q, &msg, pdMS_TO_TICKS(MP_QUEUE_TIMEOUT_MS));
}

void music_play_on_decoder_event(music_play_handle_t player,
                                 audio_decoder_event_id_t evt,
                                 const audio_decoder_info_t *info)
{
    music_msg_t msg;

    if (player == NULL)
    {
        return;
    }
    memset(&msg, 0, sizeof(msg));
    msg.type = MUSIC_MSG_DEC_EVENT;
    msg.u.dec.id = evt;
    if (info != NULL)
    {
        msg.u.dec.info = *info;
    }
    xQueueSend(player->cmd_q, &msg, pdMS_TO_TICKS(MP_QUEUE_TIMEOUT_MS));
}
