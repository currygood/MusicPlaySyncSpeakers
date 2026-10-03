/**
 * @file audio_decoder.h
 * @brief SD 卡 MP3 解码模块接口（中间件层）
 *
 * 架构约定（见《主音频节点软件架构分层设计.md》3.2.3）：
 *   - 只负责把 SD 卡 /sdcard/music 下的 MP3 文件流式解码为 16-bit PCM；
 *   - 解码任务在模块内部创建（Core 1/优先级 10/栈 32768，见实现文件），
 *     PCM 经 on_pcm 回调交给上层（MusicPlayer → sync_protocol）；
 *   - 本模块只做“解码 + 输出回调”：没有 play/pause 决策权、不提供 seek()、
 *     不控制音量（音量归 amplifier/上层）；
 *   - 播放/暂停/中止的对外语义：audio_decoder_play/pause/stop 只是内部
 *     解码任务的启停，下一首/循环由 App 层 MusicPlayer 决定；
 *   - 当前仅支持 MP3（third_party/minimp3，CC0-1.0），WAV 等其他格式暂不支持，后续按需扩展。
 */
#ifndef AUDIO_DECODER_H
#define AUDIO_DECODER_H

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ======================== 句柄 =================================================== */

/** 句柄（不透明结构体） */
typedef struct audio_decoder_s *audio_decoder_handle_t;

/* ======================== 音频格式 / 文件参数 ===================================== */

/** 音频格式：当前仅支持 MP3 */
typedef enum {
    AUDIO_DECODER_FMT_MP3 = 0,
} audio_decoder_format_t;

/** 文件参数（READY/EOF 事件与 get_info 携带） */
typedef struct {
    audio_decoder_format_t format;
    uint32_t               sample_rate;   /* 采样率（Hz） */
    uint8_t                channels;      /* 声道数：1 单声道 / 2 立体声 */
    uint32_t               duration_ms;   /* 时长（ms，VBR 标签或按码率估算） */
    uint32_t               bitrate_bps;   /* 码率（bps，取首个 MP3 帧头） */
} audio_decoder_info_t;

/* ======================== 音频回调 ================================================ */

/**
 * @brief PCM 输出回调（解码任务上下文同步调用）
 *
 * @param pcm         16-bit PCM（声道交织：L/R/L/R...）
 * @param samples     采样数（含全部声道：立体声 = 帧数 × 2）
 * @param sample_rate 采样率（Hz），是否与功放全链路 44.1kHz 一致由上层判断
 * @param channels    声道数（1/2）
 * @param user_ctx    pcm_ctx
 */
typedef void (*audio_decoder_pcm_cb_t)(const int16_t *pcm,
                                       size_t samples,
                                       uint32_t sample_rate,
                                       uint8_t channels,
                                       void *user_ctx);

/** 播放/编解码事件 */
typedef enum {
    AUDIO_DECODER_EVT_READY,   /* 文件头解析完成，可读取参数 */
    AUDIO_DECODER_EVT_EOF,     /* 解码到文件尾，MusicPlayer 切下一首 */
    AUDIO_DECODER_EVT_ERROR,   /* 文件损坏/解码失败 */
} audio_decoder_event_id_t;

typedef void (*audio_decoder_event_cb_t)(audio_decoder_event_id_t event,
                                         const audio_decoder_info_t *info,
                                         void *user_ctx);

/** 打开文件时的配置 */
typedef struct {
    audio_decoder_pcm_cb_t   on_pcm;    /* 解码输出（必填） */
    void                    *pcm_ctx;   /* on_pcm 用户上下文 */
    audio_decoder_event_cb_t on_event;  /* 事件回调（可选） */
    void                    *event_ctx; /* on_event 用户上下文 */
} audio_decoder_cfg_t;

/* ======================== API 函数 =============================================== */

/**
 * @brief 初始化解码器模块（打开句柄注册表，可重复调用）
 *
 * @return ESP_OK 成功
 */
esp_err_t audio_decoder_init(void);

/**
 * @brief 反初始化：强制关闭所有仍打开的句柄并释放资源
 *
 * @return ESP_OK 成功
 */
esp_err_t audio_decoder_deinit(void);

/**
 * @brief 打开并解析 SD 卡 MP3 文件（如 "/sdcard/music/a.mp3"）
 *
 * 流程：经 sd_card 打开文件 → 创建内部解码任务 → 解码任务在自身 32KB
 * 栈内完成首帧探测（minimp3 自动跳过 ID3v2 标签，不做整文件扫描，
 * 避免大文件卡顿）→ 触发 AUDIO_DECODER_EVT_READY。
 *
 * @param path 文件路径，必须位于 SD_CARD_MOUNT_POINT 下
 * @param cfg  回调配置（on_pcm 必填）
 *
 * @return 有效句柄；失败返回 NULL（可通过日志定位原因）
 */
audio_decoder_handle_t audio_decoder_open(const char *path,
                                          const audio_decoder_cfg_t *cfg);

/**
 * @brief 关闭句柄并释放资源（含内部解码任务）
 *
 * @param dec 句柄（关闭后不再使用）
 *
 * @return ESP_OK 或错误码
 */
esp_err_t audio_decoder_close(audio_decoder_handle_t dec);

/**
 * @brief 开始/继续解码（内部解码任务驱动 on_pcm 回调）
 *
 * 若此前已到文件尾（EOF）会先回到文件头再播放；暂停后调用则从
 * 暂停位置继续。
 *
 * @return ESP_OK 或错误码
 */
esp_err_t audio_decoder_play(audio_decoder_handle_t dec);

/**
 * @brief 暂停解码（解码任务挂起，on_pcm 不再输出，位置保持）
 *
 * @return ESP_OK 或错误码
 */
esp_err_t audio_decoder_pause(audio_decoder_handle_t dec);

/**
 * @brief 停止解码并回到文件开头（下次 play 从头开始）
 *
 * @return ESP_OK 或错误码
 */
esp_err_t audio_decoder_stop(audio_decoder_handle_t dec);

/**
 * @brief 查询文件参数（采样率/声道/时长/码率）
 *
 * @param dec  句柄
 * @param info 输出参数
 *
 * @return ESP_OK 或错误码
 */
esp_err_t audio_decoder_get_info(audio_decoder_handle_t dec,
                                 audio_decoder_info_t *info);

/**
 * @brief 查询当前播放进度（毫秒，UI 显示用）
 *
 * @param dec         句柄
 * @param position_ms 输出参数：已解码位置（ms）
 *
 * @return ESP_OK 或错误码
 */
esp_err_t audio_decoder_get_position_ms(audio_decoder_handle_t dec,
                                        uint32_t *position_ms);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_DECODER_H */
