/**
 * @file OTA_Update.h
 * @brief OTA 远程升级模块接口（局域网 Pull-style OTA）
 *
 * 本模块与局域网内的 Python OTA 服务器配合，完成：
 *   1. 访问 /ota/check 获取最新固件的版本号、SHA256 和下载 URL
 *   2. 与本地运行固件版本做语义化比较（自动忽略 v / esp_v 等前缀）
 *   3. 使用 esp_https_ota 流式下载并写入备用 app 分区
 *   4. 下载完成后校验 SHA256，校验通过后切换启动分区并重启
 *
 * 设计原则：
 *   - 模块不依赖 WiFi、音频、AI Cloud 等具体业务模块，只暴露事件回调。
 *   - main.c 可在事件回调中暂停/恢复音频采集与 WebSocket，实现并发控制。
 *   - 支持一次性阻塞检测 OTA_Update_CheckAndUpgrade()，也支持后台任务 OTA_Update_Start()。
 *
 * 典型用法（在 app_main 中，WiFi 就绪后）：
 *   OTA_Update_Init(NULL);
 *   OTA_Update_Start();
 *
 * 如需自定义服务器地址和本地版本：
 *   ota_update_config_t cfg = {
 *       .check_url = "http://192.168.1.100:5000/ota/check",
 *       .local_version = NULL,   // NULL 时自动读取当前运行固件的版本号
 *       .event_cb = my_ota_event_handler,
 *   };
 *   OTA_Update_Init(&cfg);
 *
 * check_url 解析优先级：cfg->check_url → NVS（node_role 持久化的 ota_server_url）→ OTA_SERVER_CHECK_URL；
 * 因此 OTA_Update_Init(NULL) 也会从 NVS 读到已配置的服务器地址。
 */

#ifndef __OTA_UPDATE_H__
#define __OTA_UPDATE_H__

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ======================== 默认配置（可被 main.c 或构建系统覆盖） ================== */

/** OTA 元数据接口完整地址（兜底默认值；优先级：cfg->check_url → NVS node_role:ota_server_url → 本宏） */
#ifndef OTA_SERVER_CHECK_URL
#define OTA_SERVER_CHECK_URL "http://192.168.4.16:5000/ota/check"
#endif

/** 本地固件版本号兜底值（仅当自动读取运行固件版本失败时使用） */
#ifndef OTA_LOCAL_VERSION
#define OTA_LOCAL_VERSION "1.0.0"
#endif

/** /ota/check 请求超时时间（毫秒） */
#ifndef OTA_CHECK_TIMEOUT_MS
#define OTA_CHECK_TIMEOUT_MS 5000
#endif

/** 固件下载超时时间（毫秒） */
#ifndef OTA_DOWNLOAD_TIMEOUT_MS
#define OTA_DOWNLOAD_TIMEOUT_MS 60000
#endif

/** 后台 OTA 任务栈大小（字节） */
#ifndef OTA_TASK_STACK_SIZE
#define OTA_TASK_STACK_SIZE 12288
#endif

/** 后台 OTA 任务优先级（建议高于普通业务任务） */
#ifndef OTA_TASK_PRIORITY
#define OTA_TASK_PRIORITY 5
#endif

/** URL 缓冲区长度 */
#ifndef OTA_URL_MAX_LEN
#define OTA_URL_MAX_LEN 256
#endif

/** 版本号缓冲区长度 */
#ifndef OTA_VERSION_MAX_LEN
#define OTA_VERSION_MAX_LEN 32
#endif

/** SHA256 十六进制字符串缓冲区长度（64 字符 + 结束符） */
#ifndef OTA_SHA256_MAX_LEN
#define OTA_SHA256_MAX_LEN 65
#endif

/* ======================== 事件类型 ============================================= */

/**
 * @brief OTA 状态事件
 */
typedef enum
{
    OTA_UPDATE_EVT_CHECK_STARTED = 0,   /**< 开始检查远端版本 */
    OTA_UPDATE_EVT_UP_TO_DATE,          /**< 当前已是最新版本 */
    OTA_UPDATE_EVT_UPDATE_AVAILABLE,    /**< 发现新版本，准备下载 */
    OTA_UPDATE_EVT_DOWNLOAD_STARTED,    /**< 开始下载固件 */
    OTA_UPDATE_EVT_DOWNLOAD_PROGRESS,   /**< 下载进度变化 */
    OTA_UPDATE_EVT_DOWNLOAD_FINISHED,   /**< 固件下载并校验完成 */
    OTA_UPDATE_EVT_READY_TO_REBOOT,     /**< 即将重启进入新固件 */
    OTA_UPDATE_EVT_ABORTED,             /**< 检测/下载失败或升级中止 */
} ota_update_event_t;

/**
 * @brief OTA 事件信息
 */
typedef struct
{
    ota_update_event_t event;                 /**< 事件类型 */
    char version[OTA_VERSION_MAX_LEN];        /**< 远端固件版本号（无远端信息时为空字符串） */
    char sha256[OTA_SHA256_MAX_LEN];          /**< 远端固件 SHA256（无远端信息时为空字符串） */
    char url[OTA_URL_MAX_LEN];                /**< 远端固件下载 URL（无远端信息时为空字符串） */
    int  progress_percent;                    /**< 下载进度 0~100；其他事件为 -1 */
    int  http_status;                         /**< HTTP 状态码；非 HTTP 阶段为 0 */
} ota_update_event_info_t;

/**
 * @brief OTA 事件回调
 *
 * @param info      事件信息
 * @param user_ctx  注册时传入的用户上下文
 */
typedef void (*ota_update_event_cb_t)(const ota_update_event_info_t *info, void *user_ctx);

/* ======================== 初始化配置 =========================================== */

/**
 * @brief OTA 模块配置
 */
typedef struct
{
    const char *check_url;              /**< /ota/check 完整 URL；NULL 时依次取 NVS node_role:ota_server_url、OTA_SERVER_CHECK_URL */
    const char *local_version;          /**< 本地版本号；NULL 时自动读取当前运行固件版本 */
    uint32_t check_timeout_ms;          /**< 检测请求超时；0 使用 OTA_CHECK_TIMEOUT_MS */
    uint32_t download_timeout_ms;       /**< 下载超时；0 使用 OTA_DOWNLOAD_TIMEOUT_MS */
    ota_update_event_cb_t event_cb;     /**< OTA 事件回调，可为 NULL */
    void *event_user_ctx;               /**< 回调用户上下文 */
} ota_update_config_t;

/* ======================== 公共 API ============================================= */

/**
 * @brief 初始化 OTA 模块
 *
 * @param cfg 配置参数；可传 NULL 使用默认配置。
 * @return ESP_OK      初始化成功
 *         ESP_ERR_NO_MEM 互斥锁创建失败
 */
esp_err_t OTA_Update_Init(const ota_update_config_t *cfg);

/**
 * @brief 注册 OTA 事件回调
 *
 * @param cb        事件回调
 * @param user_ctx  用户上下文
 * @return ESP_OK            注册成功
 *         ESP_ERR_INVALID_STATE 模块尚未初始化
 */
esp_err_t OTA_Update_RegisterEventCallback(ota_update_event_cb_t cb, void *user_ctx);

/**
 * @brief 启动后台 OTA 检测任务
 *
 * 任务会周期性访问 /ota/check；发现新版本后自动下载、校验并重启。
 * 注意：调用前应确保 WiFi 已就绪，或由模块在任务中自行重试。
 *
 * @return ESP_OK            任务创建成功
 *         ESP_ERR_NO_MEM    任务创建失败（内存不足）
 *         ESP_ERR_INVALID_STATE 初始化失败
 */
esp_err_t OTA_Update_Start(void);

/**
 * @brief 执行一次阻塞式 OTA 检测与升级
 *
 * 适合由 main.c 在 WiFi 就绪后主动调用，而非使用后台任务。
 *
 * @return ESP_OK                    无需升级或升级成功并已重启
 *         其他错误码                检测/下载/校验失败
 */
esp_err_t OTA_Update_CheckAndUpgrade(void);

/**
 * @brief 将当前运行固件标记为有效，取消自动回滚
 *
 * 仅在 CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE 启用时有效。
 * 新固件首次启动并确认运行正常后调用（建议 WiFi 连接成功后再调用）。
 *
 * @return ESP_OK      操作成功或当前不需要确认
 *         ESP_FAIL    标记失败
 */
esp_err_t OTA_Update_MarkAppValid(void);

/**
 * @brief 获取当前运行固件的本地版本号
 *
 * @return 本地版本号字符串（由模块内部静态缓冲持有，不可释放）
 */
const char *OTA_Update_GetLocalVersion(void);

/**
 * @brief 获取最近一次 /ota/check 返回的远端版本号
 *
 * @return 远端版本号字符串；尚未检查或检查失败时为空字符串
 */
const char *OTA_Update_GetRemoteVersion(void);

/**
 * @brief 查询 OTA 模块是否正在执行检测/升级
 *
 * @return true  忙
 *         false 空闲
 */
bool OTA_Update_IsBusy(void);

#ifdef __cplusplus
}
#endif

#endif
