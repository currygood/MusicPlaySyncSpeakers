/**
 * @file wifi_manager.h
 * @brief WiFi 系统服务模块接口（STA 连接/重连/配网 + 统一 UDP 组播通道）
 *
 * 模块职责（对应分层设计 3.4.2）：
 *   - STA 连接、断线自动重连；启动时凭据经 node_role_get() 读取（NVS
 *     未配置时回退到本文件头默认宏兜底，正式环境由触摸屏配网写入）；
 *   - 配网：UI 触摸屏输入 SSID / 密码，先经 node_role_set() 持久化，再调
 *     wifi_manager_set_sta() 发起连接；本模块不直接写 NVS；
 *   - 统一持有 UDP 组播 socket：音频同步（数据 + 控制）、灯控等组播通道均由
 *     wifi_manager_mcast_open() 创建并维护；WiFi 重连 / IP 变化后内部自动
 *     重建并重新加入组播，业务模块无感知；
 *   - 事件仅作状态通知，业务模块可选注册；UI 图标与 OTA 通道查询接口获取
 *     状态，不注册事件。
 *
 * 使用约定：
 *   - create() 只做资源准备，start() 才初始化 WiFi 并开始连接；
 *   - 事件回调统一在默认事件循环任务上下文触发，回调内不要阻塞；
 *   - 各 API 线程安全；未 create 即调用返回 ESP_ERR_INVALID_STATE。
 */
#ifndef __WIFI_MANAGER_H__
#define __WIFI_MANAGER_H__


#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* ======================== 开发期兜底凭据 ======================== */
/** 兜底 SSID：node_role NVS 未配网（ssid 为空）时使用，与 node_role 内置默认一致 */
#define WIFI_MANAGER_DEFAULT_SSID        "败家之眼"
/** 兜底密码：同上 */
#define WIFI_MANAGER_DEFAULT_PASSWORD    "Tgs200410"

/* ======================== 句柄与状态 ======================== */

/* 句柄 */
typedef struct wifi_manager_s *wifi_manager_handle_t;

/* 状态（UI 图标 / OTA 用） */
typedef enum {
    WIFI_STATE_IDLE = 0,      /* 未启动/已停止 */
    WIFI_STATE_CONNECTING,    /* 连接中 */
    WIFI_STATE_CONNECTED,     /* 已连接并拿到 IP */
    WIFI_STATE_DISCONNECTED,  /* 断开，自动重连中 */
    WIFI_STATE_ERROR,         /* 重试次数耗尽/凭据错误 */
} wifi_state_t;

/* 事件（状态通知，业务模块可选注册） */
typedef enum {
    WIFI_EVT_CONNECTED,       /* 连接成功，携带 IP */
    WIFI_EVT_DISCONNECTED,    /* 连接断开 */
    WIFI_EVT_RECONNECTING,    /* 开始自动重连 */
    WIFI_EVT_IP_CHANGED,      /* IP 变化，本模块已自动重建组播通道 */
} wifi_event_id_t;

/* esp-idf 6.0.1 已定义类型 wifi_event_t（esp_wifi_types_generic.h 的 WiFi 事件枚举），
 * 与设计文档 3.4.2 中的结构体重名，故实现改为 wifi_mgr_event_t（事件含义与文档一致） */
typedef struct {
    wifi_event_id_t id;
    char   ssid[33];
    char   ip[16];
    int8_t rssi;
} wifi_mgr_event_t;

typedef void (*wifi_event_cb_t)(const wifi_mgr_event_t *evt, void *user_ctx);

/* 配置 */
typedef struct {
    uint32_t connect_timeout_ms;     /* 单次连接超时，默认 10000 */
    uint32_t reconnect_interval_ms;  /* 重连间隔，默认 3000 */
    uint32_t max_retries;            /* 连续失败次数，0 = 无限重连 */
} wifi_manager_cfg_t;

/* ======================== 生命周期 ======================== */

/* 生命周期；start 时从 node_role_get() 读取凭据并连接（NVS 未配网时用兜底宏） */
wifi_manager_handle_t wifi_manager_create(const wifi_manager_cfg_t *cfg);
void                  wifi_manager_destroy(wifi_manager_handle_t h);
esp_err_t             wifi_manager_start(wifi_manager_handle_t h);
esp_err_t             wifi_manager_stop(wifi_manager_handle_t h);

/* 配网（UI 触摸屏）：仅用新凭据立即发起连接（持久化由 node_role 负责） */
esp_err_t wifi_manager_set_sta(wifi_manager_handle_t h,
                               const char *ssid, const char *password);

/* 事件订阅（业务模块可选注册） */
esp_err_t wifi_manager_register_event_cb(wifi_manager_handle_t h,
                                         wifi_event_cb_t cb, void *user_ctx);

/* 状态查询（UI 图标 / OTA 用） */
esp_err_t wifi_manager_get_state(wifi_manager_handle_t h, wifi_state_t *state);
bool      wifi_manager_is_connected(wifi_manager_handle_t h);
esp_err_t wifi_manager_get_ip(wifi_manager_handle_t h, char *ip, size_t *len);
int8_t    wifi_manager_get_rssi(wifi_manager_handle_t h);

/* ======================== 组播通道 ======================== */

/* 组播通道：UDP socket 统一由本模块持有（音频同步 / 灯控共用） */
typedef struct wifi_mcast_s *wifi_mcast_handle_t;

typedef struct {
    const char *group;          /* 组播组，如 239.0.0.1（音频）/ 239.0.0.2（灯控） */
    uint16_t    port;           /* 绑定端口，音频 5678/5679、灯控 8889 */
    bool        rx_enable;      /* 需要接收：模块内开 RX 任务 + FIFO */
    uint32_t    rx_fifo_bytes;  /* 接收 FIFO，默认 16384 */
} wifi_mcast_cfg_t;

esp_err_t wifi_manager_mcast_open(wifi_manager_handle_t h,
                                  const wifi_mcast_cfg_t *cfg,
                                  wifi_mcast_handle_t *out);
esp_err_t wifi_manager_mcast_send(wifi_mcast_handle_t ch,
                                  const void *data, size_t len,
                                  uint32_t timeout_ms);
esp_err_t wifi_manager_mcast_recv(wifi_mcast_handle_t ch, /* 从 FIFO 阻塞取 */
                                  void *buf, size_t cap, size_t *len,
                                  uint32_t timeout_ms);
esp_err_t wifi_manager_mcast_close(wifi_mcast_handle_t ch);


#endif
