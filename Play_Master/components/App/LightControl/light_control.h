/**
 * @file light_control.h
 * @brief 灯控模块接口（App 层），对应《主音频节点软件架构分层设计》3.3.3
 *
 * 模块职责（对应分层设计 3.3.3）：
 *   - 封装灯控 JSON 协议，提供 on/off 开关单个区域灯 App 控制接口（不支持全屋广播）；
 *   - 数据包经 wifi_manager 灯控组播通道发送（默认 239.0.0.2:8889，由调用方
 *     wifi_manager_mcast_open() 创建后传入），本模块不自行持有 socket；
 *   - 解析灯控端两类上行消息：即时回执与周期状态上报，统一经 on_event
 *     回调通知 UI 刷新灯状态图标；
 *   - 灯状态以 ack/status 的 ok 为准（true=开、false=关），未收到上行为 UNKNOWN；
 *     超时未收到某灯上行，发 LIGHT_EVT_ERROR 并把状态回落 UNKNOWN。
 */

#ifndef __LIGHT_CONTROL_H__
#define __LIGHT_CONTROL_H__

#include "wifi_manager.h"
#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ======================== 类型定义 ================================================== */

/** 句柄 */
typedef struct light_control_s *light_control_handle_t;

/* 指令（对应 JSON 的 "cmd" 字段） */
typedef enum {
    LIGHT_CMD_ON = 0,     /* {"cmd":"on","light":"<name>"} */
    LIGHT_CMD_OFF,        /* {"cmd":"off","light":"<name>"} */
} light_cmd_t;

/* 灯状态（UI 图标用） */
typedef enum {
    LIGHT_STATE_UNKNOWN = 0,
    LIGHT_STATE_ON,
    LIGHT_STATE_OFF,
} light_state_t;

/* 灯控事件 */
typedef enum {
    LIGHT_EVT_ACK = 0,    /* 收到即时回执 {"cmd":"ack",...} */
    LIGHT_EVT_STATUS,     /* 收到周期状态上报 {"cmd":"status",...}（每 30s） */
    LIGHT_EVT_ERROR,      /* 回执异常 / 状态上报超时 */
} light_event_id_t;

typedef struct {
    char          light[16]; /* 区域灯名，如 "bedroom" */
    bool          ok;        /* ack/status 上报：true=开，false=关 */
    light_state_t state;     /* 本次上报对应的实际开关状态（由 ok 归一化，超时为 UNKNOWN） */
} light_event_data_t;

typedef struct {
    light_event_id_t   id;
    light_event_data_t data;
} light_event_t;

typedef void (*light_control_event_cb_t)(const light_event_t *evt, void *user_ctx);

/* 配置 */
typedef struct {
    wifi_mcast_handle_t      chan;              /* 灯控组播通道（App 经 wifi_manager 创建传入，rx 需开启） */
    uint32_t                 tx_timeout_ms;     /* 发送超时，默认 100 */
    uint32_t                 state_timeout_ms;  /* 状态上报超时判定，默认 90000（3 x 30s 周期） */
    light_control_event_cb_t on_event;          /* 灯状态事件回调（ack/status/超时）：UI 刷新灯状态图标 */
    void                     *event_ctx;
} light_control_cfg_t;

/* ======================== API ============================================================ */

/* 生命周期 */
light_control_handle_t light_control_create(const light_control_cfg_t *cfg);
void                   light_control_destroy(light_control_handle_t h);

/* UI：开关某盏灯 */
esp_err_t light_control_send(light_control_handle_t h,
                             light_cmd_t cmd, const char *light);

/* 查询某灯当前状态（缓存最近一次 ack/status；未收到上行为 UNKNOWN） */
esp_err_t light_control_get_state(light_control_handle_t h,
                                  const char *light,
                                  light_state_t *out);

#ifdef __cplusplus
}
#endif

#endif /* __LIGHT_CONTROL_H__ */