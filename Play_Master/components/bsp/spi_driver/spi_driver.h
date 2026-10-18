#ifndef SPI_DRIVER_H
#define SPI_DRIVER_H

/* ======================== 是否打开 SPI 调试打印 ======================== */
#define SPI_DRIVER_DEBUG_LOGS    (false)

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/gpio.h"
#include <stddef.h>

/* ======================== 总线仲裁（优先级准入 + 会话预算） =====================
 *
 * 同一条 SPI2 总线上的从设备按优先级准入：
 *   - 每个从设备在 Spi_DeviceConfig_t 里声明仲裁优先级，数值大者优先；
 *   - 会话式设备（csManualCtrl=true，如 SD 卡）的调用方应在长等待循环或
 *     扇区边界调用 Spi_Device_Session_Yield_If_Needed()：当有更高优先级设备
 *     在等待、或本次占用超过会话预算时，临时释放总线并立即重新申请，让
 *     UI/LCD 这类实时消费者插入一笔事务（CS 保持低电平，SD 命令序列不中断）；
 *   - 长时间拿不到总线的会话有防饿死保护：等待超过阈值后，本设备在仲裁中
 *     临时按最高优先级处理。
 */

/* SPI 总线默认引脚（LCD 与 SD 卡共用，具体以调用方传入的 Spi_Config_t 为准） */
#define SPI_SCK_GPIO   19
#define SPI_MOSI_GPIO  18
#define SPI_MISO_GPIO  21

/** 同一总线上最多注册的从设备数（LCD + SD 卡 + 未来扩展） */
#define SPI_MAX_DEVICES 8

/**
 * SPI 总线配置（一条总线 = 三根信号线：SCK/MOSI/MISO）
 *
 * 注意：片选 CS 不属于总线，属于从设备，在 Spi_DeviceConfig_t 中指定。
 */
typedef struct {
    int clockSpeedHz;
    int sckPin;
    int mosiPin;
    int misoPin;
} Spi_Config_t;

/**
 * @brief 设备仲裁优先级（数值大者优先）
 */
typedef enum {
    SPI_PRIO_LOW    = 1,   /* 后台维护类：格式化、批量写 */
    SPI_PRIO_NORMAL = 2,   /* 默认：数据流读写（SD 卡） */
    SPI_PRIO_HIGH   = 3,   /* 实时交互：LCD 刷屏 */
} spi_prio_t;

/**
 * @brief SPI 从设备配置（挂到同一条总线上的一个片选设备）
 *
 * 每个从设备用 CS 引脚唯一标识；同一个 CS 只能注册一次。
 */
typedef struct {
    int  csPin;            /* 片选 GPIO，唯一标识该从设备（必填） */
    int  clockSpeedHz;     /* 该从设备的 SPI 时钟（Hz），0 则用默认 10MHz */
    int  mode;             /* SPI 工作模式：0~3 */
    int  queueSize;        /* 事务队列深度 */
    bool csManualCtrl;     /* 手动片选：由会话 API 自己拉高/拉低 CS（SD 卡协议需要整段序列保持 CS，默认 false） */
    int  priority;         /* 仲裁优先级 spi_prio_t，0 按 SPI_PRIO_NORMAL 处理 */
    const char *label;     /* 设备名（日志/统计用，可空） */
} Spi_DeviceConfig_t;

/**
 * @brief 设备会话配置（优先级准入 + 会话预算 + 防饿死）
 */
typedef struct {
    int      priority;            /* 本会话优先级（spi_prio_t），0 按 SPI_PRIO_NORMAL */
    uint32_t session_budget_ms;   /* 单段占用预算(ms)，超时让出；0 用全局默认 10ms */
    uint32_t starvation_ms;       /* 连续等待超过该值临时提优先级；0 用全局默认 500ms */
} spi_session_cfg_t;

/** 单设备仲裁统计快照 */
typedef struct {
    const char *label;                 /* 设备名（注册时传入） */
    uint32_t    acquires;              /* 成功申请次数（含让出后重新申请） */
    uint32_t    waits;                 /* 因总线被占而等待的次数 */
    uint32_t    yields;                /* 主动让出次数（Spi_Device_Session_Yield...） */
    uint32_t    held_total_us;         /* 累计持有时长（µs） */
    uint32_t    held_max_us;           /* 单次最长持有时长（µs） */
    uint32_t    wait_total_us;         /* 累计等待时长（µs） */
    uint32_t    wait_max_us;           /* 单次最长等待（µs） */
} spi_stats_t;

/**
 * @brief SPI 从设备句柄（不透明）
 *
 * 句柄内部绑定 CS 引脚与 ESP-IDF 底层设备，创建自 Spi_Register_Device()；
 * 通过句柄收发数据时，驱动会自动选中该设备对应的 CS。
 */
typedef struct spi_device_s *Spi_Handle_t;

/**
 * @brief 初始化 SPI 总线（SPI2 主机）
 *
 * 总线如果已被其他模块（如 SD 卡 / LCD 先调用）占用，返回
 * ESP_ERR_INVALID_STATE，调用方可选择复用现有总线。
 *
 * @param config 引脚与时钟配置（NULL 则使用默认宏引脚）
 * @return ESP_OK / ESP_ERR_INVALID_STATE / 其他错误
 */
esp_err_t Spi_Init(Spi_Config_t *config);

/**
 * @brief 释放 SPI2 总线
 *
 * 注意：仅当 SPI 总线由本模块（Spi_Init 成功返回 ESP_OK）创建时才应调用；
 * 若总线是被其他模块复用的，不要调用本函数。
 *
 * @return ESP_OK 释放成功
 */
/** @brief 查询 SPI2 总线是否由本驱动本次创建（false 表示被其他模块占用复用） */
bool Spi_Is_Bus_Created(void);

esp_err_t Spi_Deinit(void);

/**
 * @brief 注册一个 SPI 从设备（绑定 CS 引脚）
 *
 * 注册成功后返回不透明句柄，句柄内部持有 CS/时钟/模式信息；
 * 同一条总线上的多个从设备分别注册（LCD 一个 CS，SD 卡另一个 CS）。
 *
 * @param config 从设备配置（csPin 必填且不能重复注册）
 * @return 设备句柄；失败返回 NULL
 */
Spi_Handle_t Spi_Register_Device(const Spi_DeviceConfig_t *config);

/**
 * @brief 查询从设备的仲裁优先级（spi_prio_t）
 *
 * @param handle 设备句柄
 * @return 优先级数值；句柄无效返回 0
 */
int Spi_Get_Priority(Spi_Handle_t handle);

/**
 * @brief 修改已注册从设备的 SPI 时钟（如 SD 卡初始化 400kHz -> 正常 20MHz）
 *
 * 底层通过"移除设备并重新注册"实现（ESP-IDF 无运行中改频接口，
 * 官方 sdspi_host 同样采用 remove/re-add 方案）。
 * 注意：必须在设备会话之外调用（先 Spi_Device_Release 再调用本函数）。
 *
 * @param handle           设备句柄（须已注册）
 * @param clock_speed_hz  新时钟频率（Hz）
 * @return ESP_OK 成功
 */
esp_err_t Spi_Device_Set_Clock(Spi_Handle_t handle, int clock_speed_hz);

/**
 * @brief 发送（按句柄，选中句柄绑定的 CS 从设备）
 */
esp_err_t Spi_Transmit(Spi_Handle_t handle, const uint8_t *data, size_t length);

/**
 * @brief 发送 16 位数据（大端序，同 Spi_Transmit）
 */
esp_err_t Spi_Transmit_16Bit(Spi_Handle_t handle, uint16_t data);

/**
 * @brief 接收（按句柄，选择该句柄绑定的从设备）
 *
 * SPI 为同步总线，接收时主机会发送全 1（0xFF）dummy 时钟。
 *
 * @param handle 设备句柄
 * @param buffer 接收缓冲区
 * @param length 期望接收字节数
 * @return ESP_OK / ESP_ERR_INVALID_ARG / ESP_FAIL
 */
esp_err_t Spi_Receive(Spi_Handle_t handle, uint8_t *buffer, size_t length);

/**
 * @brief 全双工收发（按句柄，同时发送并接收）
 */
esp_err_t Spi_Transmit_Receive(Spi_Handle_t handle,
                               const uint8_t *tx_buffer, uint8_t *rx_buffer,
                               size_t length);

/**
 * @brief 按 CS 引脚选择从设备并发送
 */
esp_err_t Spi_Transmit_CS(int csPin, const uint8_t *data, size_t length);

/**
 * @brief 按 CS 引脚选择从设备并接收
 */
esp_err_t Spi_Receive_CS(int csPin, uint8_t *buffer, size_t length);

/**
 * @brief 按 CS 引脚选择从设备做全双工收发
 */
esp_err_t Spi_Transmit_Receive_CS(int csPin,
                                  const uint8_t *tx_buffer, uint8_t *rx_buffer,
                                  size_t length);

/**
 * @brief 进入设备会话（占用 SPI 总线，期间其他从设备事务排队等待）
 *
 * 配合手动片选设备（csManualCtrl=true）使用：调用后调用方通过
 * Spi_Device_SetCS 自行控制 CS 电平，适合 SD 卡这类需要
 * “命令 + 响应 + 数据”整段保持 CS 有效的多字节序列。
 *
 * 申请按设备优先级排队（见 spi_prio_t）：高优先级设备的普通事务同样
 * 会优先占用总线。若需会话级预算与主动让出，改用 Spi_Device_Session_Start()。
 *
 * @param handle 设备句柄（须已注册）
 * @return ESP_OK 成功
 */
esp_err_t Spi_Device_Acquire(Spi_Handle_t handle);

/**
 * @brief 申请设备会话（优先级准入 + 会话预算 + 防饿死）
 *
 * @param handle 设备句柄（须已注册）
 * @param cfg    会话配置（NULL 或字段为 0 时使用默认值：NORMAL 优先级、
 *               10ms 会话预算、500ms 防饿死阈值）
 * @return 会话句柄；失败返回 NULL
 */
void *Spi_Device_Session_Start(Spi_Handle_t handle, const spi_session_cfg_t *cfg);

/**
 * @brief 结束设备会话，归还总线
 *
 * @param session Spi_Device_Session_Start() 返回的会话句柄
 */
void Spi_Device_Session_End(void *session);

/**
 * @brief 判断当前会话是否应该让出总线
 *
 * 判定条件（满足任意一条即为 true）：
 *   1. 有其他设备在等待，且其优先级高于本会话当前生效优先级；
 *   2. 本段连续占用时长超过会话预算。
 *
 * 本函数只做判定、不操作总线，供调用方在长等待循环里低成本轮询；
 * 返回 true 时再使用 Spi_Device_Session_Yield() 实际让出。
 *
 * @param session Spi_Device_Session_Start() 返回的会话句柄
 * @return true 建议让出；false 可继续占用
 */
bool Spi_Device_Session_Yield_If_Needed(void *session);

/**
 * @brief 临时让出总线（结束本段占用，随即重新申请）
 *
 * 用于手动片选会话（SD 卡）：调用方必须在协议安全点（扇区边界、
 * 忙等待轮询之间）调用，期间 CS 保持低电平，SD 命令序列不中断。
 * 高优先级设备（LCD 刷屏）会插入一笔事务后立即交还。
 *
 * @param session Spi_Device_Session_Start() 返回的会话句柄
 * @return ESP_OK 成功（含让出后重新拿到总线）
 */
esp_err_t Spi_Device_Session_Yield(void *session);

/**
 * @brief 读取单设备仲裁统计快照
 *
 * @param handle 设备句柄
 * @param out    输出统计（非空）
 * @return ESP_OK / ESP_ERR_INVALID_ARG
 */
esp_err_t Spi_Get_Stats(Spi_Handle_t handle, spi_stats_t *out);

/**
 * @brief 打印所有已注册设备的仲裁统计（调试用，直接走 ESP_LOGI）
 */
void Spi_Dump_Stats(void);

/**
 * @brief 结束设备会话，归还 SPI 总线
 *
 * 注意：结束前请先把 CS 拉高（Spi_Device_SetCS(handle, true)），
 * 否则设备会一直处于选中状态。
 *
 * @param handle 设备句柄
 * @return ESP_OK 成功
 */
esp_err_t Spi_Device_Release(Spi_Handle_t handle);

/**
 * @brief 手动设置设备 CS 引脚电平（仅对 csManualCtrl=true 的设备有效）
 *
 * @param handle 设备句柄
 * @param level  true = 拉高（释放/不选中），false = 拉低（选中）
 * @return ESP_OK / ESP_ERR_NOT_SUPPORTED（非手动片选设备）
 */
esp_err_t Spi_Device_SetCS(Spi_Handle_t handle, bool level);

#endif
