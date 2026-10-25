/**
 * @file call_phone.h
 * @brief 语音通话模块（唤醒词检测 + HFP 语音上行桥接，App 层）
 *
 * 模块职责（对应《主音频节点软件架构分层设计》3.3.2）：
 *   - 本地唤醒词检测：从 microphone 模块读取麦克风 FIFO（Microphone_Read_Pcm16），
 *     把 PCM 喂给乐鑫 esp-sr 做唤醒词检测（唤醒词：你好小智，wn9s_nihaoxiaozhi）；
 *   - 唤醒命中后自动调用 bt_audio_hfp_start_voice() 建立 HFP 语音链路，
 *     SCO 打开后把麦克风 PCM 经 bt_audio_hfp_send_pcm() 持续上行到手机；
 *   - 手机语音助手应答（HFP 下行）由 bt_audio.on_pcm(BT_AUDIO_PCM_SOURCE_HFP_DOWNLINK)
 *     输出、接至 sync_protocol，本模块不处理下行；
 *   - 不对外发事件：UI / 灯控 / MusicPlay 均不订阅；需要状态时通过
 *     call_phone_get_state() 查询。
 *
 * 数据流：麦克风 → microphone(FIFO) → call_phone → esp-sr 唤醒检测
 *          → 命中 → HFP SCO 建立 → 麦克风上行 → 手机语音助手。
 *
 * 依赖：microphone（麦克风 FIFO）、bt_audio（HFP 能力）、esp-sr（乐鑫组件，仅主节点引入）。
 */

#ifndef __CALL_PHONE_H__
#define __CALL_PHONE_H__

/* ======================== 是否打开语音通话模块调试打印 ======================== */
#define CALL_PHONE_DEBUG_LOGS    (1)

#include "esp_err.h"
#include "microphone.h"
#include "bt_audio.h"
#include <stdbool.h>
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
    bt_audio_handle_t  audio;         /* 复用 bt_audio 的 HFP 能力 */
    uint32_t sample_rate;             /* 麦克风采样率：固定 44100（esp-sr 内部需 16k，本模块内重采样） */
} call_phone_cfg_t;

/* ======================== API ============================================================ */

/* 生命周期 */
call_phone_handle_t call_phone_create(const call_phone_cfg_t *cfg);
esp_err_t call_phone_destroy(call_phone_handle_t cp);

/* 检测控制：开关唤醒词监听 */
esp_err_t call_phone_start_listening(call_phone_handle_t cp);
esp_err_t call_phone_stop_listening(call_phone_handle_t cp);

/**
 * @brief 播放态抑制唤醒检测（App 层按播放状态下发）
 *
 * suspend=true ：立即停止喂麦克风、停止跑 AFE，任务阻塞在命令队列上，
 *                Core1 立刻收回 ~780ms/s（wn9s 单帧耗时 ≈ 帧长，常驻监听
 *                会把 Core1 吃满，是 IDLE1 饿死 / A2DP starved / INT WDT 的源头）。
 *                通话中（CONNECTING/STREAMING）不打断，等回到监听时再落地。
 * suspend=false：清麦克风 FIFO + 复位重采样，丢弃前几帧唤醒判定后恢复监听。
 *
 * 取舍：抑制期间不支持语音唤醒（需按键或先暂停播放）。
 * 幂等：与当前状态相同则直接返回，可安全地按轮询周期反复下发。
 */
esp_err_t call_phone_set_suspended(call_phone_handle_t cp, bool suspend);
bool      call_phone_is_suspended(call_phone_handle_t cp);

/* 链路控制：主动结束当前语音（等价于手机挂断） */
esp_err_t call_phone_hangup(call_phone_handle_t cp);

/* 状态查询 */
esp_err_t call_phone_get_state(call_phone_handle_t cp,
                               call_phone_state_t *state);

/* 唤醒检测负载（调试用）：判断 Core1 是否还有实时余量 */
typedef struct {
    uint32_t frame_us;      /* 单帧音频时长（16k×256 ≈ 16000us）＝本模块的实时预算 */
    uint32_t frames_per_s;  /* 实际处理的帧率（满速 = 1e6/frame_us ≈ 62） */
    uint32_t proc_avg_us;   /* 平均单帧处理耗时 */
    uint32_t proc_max_us;   /* 单帧处理耗时峰值 */
    uint32_t starve_per_s;  /* 每秒读不到麦克风数据的次数 */
    uint32_t overrun_cnt;   /* 累计超预算次数（应尽量为 0） */
} call_phone_load_t;

esp_err_t call_phone_get_load(call_phone_handle_t cp, call_phone_load_t *out);

#ifdef __cplusplus
}
#endif

#endif /* __CALL_PHONE_H__ */
