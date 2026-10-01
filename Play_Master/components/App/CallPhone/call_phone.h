/**
 * @file call_phone.h
 * @brief 语音通话模块（唤醒词检测 + HFP 语音上行桥接，App 层）
 *
 * 模块职责（对应《主音频节点软件架构分层设计》3.3.2）：
 *   - 本地唤醒词检测：从 audio_bus RX 注册麦克风 reader（名称 "call_phone_mic"），
 *     把 PCM 喂给乐鑫 esp-sr 做唤醒词检测（唤醒词：你好小智，wn9s_nihaoxiaozhi）；
 *   - 唤醒命中后自动调用 bt_audio_hfp_start_voice() 建立 HFP 语音链路，
 *     SCO 打开后把麦克风 PCM 经 bt_audio_hfp_send_pcm() 持续上行到手机；
 *   - 手机语音助手应答（HFP 下行）由 bt_audio.on_pcm(BT_AUDIO_PCM_SOURCE_HFP_DOWNLINK)
 *     输出、接至 sync_protocol，本模块不处理下行；
 *   - 不对外发事件：UI / 灯控 / MusicPlay 均不订阅；需要状态时通过
 *     call_phone_get_state() 查询。
 *
 * 数据流：麦克风 → audio_bus(RX) → call_phone_mic reader → esp-sr 唤醒检测
 *          → 命中 → HFP SCO 建立 → 麦克风上行 → 手机语音助手。
 *
 * 依赖：audio_bus（RX 多读者）、bt_audio（HFP 能力）、esp-sr（乐鑫组件，仅主节点引入）。
 */

#ifndef __CALL_PHONE_H__
#define __CALL_PHONE_H__

#include "esp_err.h"
#include "audio_bus.h"
#include "bt_audio.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ======================== 类型定义 ================================================== */

/** 句柄 */
typedef struct call_phone_s *call_phone_handle_t;

/* 状态（App 查询/调试用） */
typedef enum {
    CALL_PHONE_STATE_IDLE       = 0,  /* 未创建/已停止 */
    CALL_PHONE_STATE_LISTENING,       /* 正在监听唤醒词 */
    CALL_PHONE_STATE_CONNECTING,      /* 唤醒命中，建立 HFP SCO 中 */
    CALL_PHONE_STATE_STREAMING,       /* 语音链路建立，麦克风上行转发中 */
} call_phone_state_t;

/* 创建参数 */
typedef struct {
    audio_bus_handle_t bus;        /* audio_bus（本模块内部注册 RX reader "call_phone_mic"） */
    bt_audio_handle_t  audio;      /* 复用 bt_audio 的 HFP 能力 */
    uint32_t sample_rate;          /* 麦克风采样率：固定 44100（esp-sr 内部需 16k，本模块内重采样） */
    uint32_t pcm_fifo_bytes;       /* 上行 FIFO 字节数，0 = 默认 16384 */
} call_phone_cfg_t;

/* ======================== API ============================================================ */

/* 生命周期 */
call_phone_handle_t call_phone_create(const call_phone_cfg_t *cfg);
esp_err_t call_phone_destroy(call_phone_handle_t cp);

/* 检测控制：开关唤醒词监听 */
esp_err_t call_phone_start_listening(call_phone_handle_t cp);
esp_err_t call_phone_stop_listening(call_phone_handle_t cp);

/* 链路控制：主动结束当前语音（等价于手机挂断） */
esp_err_t call_phone_hangup(call_phone_handle_t cp);

/* 状态查询 */
esp_err_t call_phone_get_state(call_phone_handle_t cp,
                               call_phone_state_t *state);

#ifdef __cplusplus
}
#endif

#endif /* __CALL_PHONE_H__ */