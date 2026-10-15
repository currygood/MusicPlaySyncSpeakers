/**
 * @file node_role.h
 * @brief 节点角色与运行参数持久化模块接口（NVS 唯一管理入口）
 *
 * 模块职责（对应分层设计 3.4.3 与《nvs存储.md》）：
 *   - NVS 持久化唯一入口：节点角色（主/从）与运行参数（WiFi 凭证、
 *     音频采样率、音量、播放模式、同步延迟、组播地址、OTA 服务器地址）；
 *   - 全局唯一实例，内存缓存一份配置，读走缓存；本模块只做持久化读写，
 *     不触发业务动作；
 *   - 主/从节点共用同一份结构；首次开机（NVS 无数据）按 defaults 落盘。
 *
 * 存储设计（见 docs/02-设计/nvs存储.md）：
 *   - 分区：默认 nvs 分区；命名空间 `node_role`；Key `cfg`（单 blob）；
 *   - blob 结构 = `node_role_cfg_t`（首字段 uint16_t version 结构版本号）；
 *   - 其余业务模块一律通过 node_role_get() 读取，不直接操作 NVS。
 *
 * 使用约定：
 *   - app_main 开机调用 node_role_init()；其余业务模块（wifi_manager 等）
 *     只读不写 NVS；
 *   - 各 API 线程安全；未调用 node_role_init() 前 get/set 返回
 *     ESP_ERR_INVALID_STATE。
 */
#ifndef __NODE_ROLE_H__
#define __NODE_ROLE_H__

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* ======================== 节点角色 ======================== */

/** 节点角色（与《nvs存储.md》字段表一致，NVS 中以 uint8_t 存储） */
typedef enum {
    NODE_ROLE_MASTER = 0,   /* 主节点（音源侧） */
    NODE_ROLE_SLAVE,        /* 从节点（音频输出侧） */
} node_role_t;

/* ======================== 运行参数（整体式读写） ======================== */

/**
 * @brief 节点运行参数整体结构（NVS blob，字段顺序对应《nvs存储.md》1.2 节）
 *
 * @note version 为 blob 结构版本号，字段新增/变更时递增以触发迁移；
 *       play_mode 取值对应 MusicPlay 的 music_play_mode_t（0 顺序、1 单曲循环），
 *       本模块以 uint8_t 存储，不依赖上层枚举类型。
 */
typedef struct {
    uint16_t version;           /* blob 结构版本号，当前 1 */
    uint8_t  role;               /* 节点角色：0=NODE_ROLE_MASTER，1=NODE_ROLE_SLAVE */
    char     wifi_ssid[32];      /* WiFi 名（触摸屏配网填写） */
    char     wifi_password[64];  /* WiFi 密码 */
    char     ota_server_url[256]; /* OTA 服务器地址，默认 192.168.4.16:5000/ota/check */
    uint8_t  volume;             /* 软件音量 0-100 */
    uint8_t  play_mode;          /* 播放模式：0 顺序、1 单曲循环 */
    uint32_t sync_delay_ms;      /* 同步延迟 D（ms） */
    uint32_t audio_sample_rate;  /* 音频采样率：全链路固定 44100（保留字段） */
    char     multicast_group[16]; /* 音频组播地址 239.0.0.1 */
} node_role_cfg_t;

/* ======================== 默认参数（内置默认，配网 UI 接入前） =========== */

/** 默认 WiFi SSID/密码（开发期测试凭据，配网后由 node_role_set 覆盖） */
#define NODE_ROLE_DEFAULT_SSID          "败家之眼"
#define NODE_ROLE_DEFAULT_PASSWORD      "Tgs200410"

/** 默认 OTA 服务器地址（不带协议头，ota_manager 使用前自行补全） */
#define NODE_ROLE_DEFAULT_OTA_URL       "192.168.4.16:5000/ota/check"
/** 默认音频组播地址 */
#define NODE_ROLE_DEFAULT_GROUP         "239.0.0.1"
/** 默认音频采样率（全链路固定 44100） */
#define NODE_ROLE_DEFAULT_SAMPLE_RATE   (44100)
/** 默认同步延迟 D（ms） */
#define NODE_ROLE_DEFAULT_SYNC_DELAY_MS (200)
/** 默认音量（0-100） */
#define NODE_ROLE_DEFAULT_VOLUME        (80)
/** 默认播放模式（0 顺序、1 单曲循环） */
#define NODE_ROLE_DEFAULT_PLAY_MODE     (0)

/* ======================== API ======================== */

/**
 * @brief 初始化节点角色配置（app_main 开机调用）
 *
 * 从 NVS（node_role:cfg）读取整体配置到内存缓存；
 * 首次开机 / 结构版本不匹配 / 读取失败时，用 defaults 写回 NVS。
 *
 * @param defaults  默认配置；NULL 时使用本模块内置默认值
 *                  （WiFi 默认 HW666，见 node_role.c 头宏）
 * @return ESP_OK 成功；NVS 初始化/写入失败时返回对应 esp_err
 */
esp_err_t node_role_init(const node_role_cfg_t *defaults);

/**
 * @brief 读配置：返回缓存副本（线程安全）
 * @param out 输出参数，非空
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG / ESP_ERR_INVALID_STATE
 */
esp_err_t node_role_get(node_role_cfg_t *out);

/**
 * @brief 写配置：更新缓存并写 NVS（整体原子提交，线程安全）
 * @param cfg 待写入配置（非空）
 * @return ESP_OK 成功；参数错误或 NVS 写入失败时返回对应 esp_err
 */
esp_err_t node_role_set(const node_role_cfg_t *cfg);

/** 便捷查询：当前节点角色（未初始化时返回 NODE_ROLE_MASTER） */
node_role_t node_role_get_role(void);

/** 便捷查询：是否主节点（未初始化时按主节点处理） */
bool node_role_is_master(void);

#endif /* __NODE_ROLE_H__ */

