#ifndef SPI_DRIVER_H
#define SPI_DRIVER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/gpio.h"
#include <stddef.h>

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
 * @brief SPI 从设备配置（挂到同一条总线上的一个片选设备）
 *
 * 每个从设备用 CS 引脚唯一标识；同一个 CS 只能注册一次。
 */
typedef struct {
    int csPin;            /* 片选 GPIO，唯一标识该从设备（必填） */
    int clockSpeedHz;     /* 该从设备的 SPI 时钟（Hz），0 则用默认 10MHz */
    int mode;             /* SPI 工作模式：0~3 */
    int queueSize;        /* 事务队列深度 */
    bool csManualCtrl;    /* 手动片选：由会话 API 自己拉高/拉低 CS（SD 卡协议需要整段序列保持 CS，默认 false） */
} Spi_DeviceConfig_t;

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
 * @brief 修改已注册从设备的 SPI 时钟（如 SD 卡初始化 400kHz -> 正常 10MHz）
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
 * @param handle 设备句柄（须已注册）
 * @return ESP_OK 成功
 */
esp_err_t Spi_Device_Acquire(Spi_Handle_t handle);

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
