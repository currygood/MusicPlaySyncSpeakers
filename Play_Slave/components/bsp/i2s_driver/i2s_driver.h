/**
 * @file i2s_driver.h
 * @brief I2S 物理总线驱动接口（句柄化、参数化）
 *
 * 本模块把 ESP32-S3 的 I2S 外设封装为"物理总线"句柄，供上层模块（amplifier/microphone）与普通调用方使用。设计要点：
 *
 *   1. 不写死活引脚/采样率/格式：全部通过参数结构体由调用方传入；
 *   2. 一个句柄绑定一个 I2S 控制器端口，可创建 TX 或 RX 单向通道；
 *   3. 本层是"裸物理通道"，只做配置、收发、采样率切换；
 *      多生产者/多消费者的串行化由上层模块内部实现（amplifier 写锁 / microphone FIFO）；
 *   4. 发送/接收函数内部会校验句柄方向，方向不匹配返回 ESP_ERR_INVALID_ARG。
 *
 * 硬件说明（当前板卡）：
 *   - TX（功放 NS4168）  ：I2S_NUM_1，bclk=GPIO8 ws=GPIO9 dout=GPIO10
 */

#ifndef __I2S_DRIVER_H__
#define __I2S_DRIVER_H__

#include "driver/i2s_std.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ======================== 类型定义 =============================================== */

/** I2S 物理总线句柄（不透明结构体） */
/** I2S 控制器端口号（ESP-IDF 6.x 驱动已无此类型，自行定义以兼容旧代码风格） */
typedef int i2s_port_t;
typedef struct i2s_bus_phy_s *i2s_bus_handle_t;

/**
 * @brief I2S 引脚配置
 *
 * 未使用的引脚填 -1（I2S_GPIO_UNUSED）。
 * ws_pol / bit_shift 仅对 RX 麦克风需要调整，TX 通常保持默认。
 */
typedef struct {
    int mclk;            /**< 主时钟引脚，一般未使用 */
    int bclk;            /**< 位时钟引脚 */
    int ws;              /**< 左右声道时钟引脚 */
    int dout;            /**< 数据输出引脚（TX，接功放 DIN） */
    int din;             /**< 数据输入引脚（RX，接麦克风 SD） */
    bool ws_pol;         /**< WS 极性反转（INMP441 需要 true） */
    bool bit_shift;      /**< 位偏移 */
} i2s_pin_cfg_t;

/**
 * @brief I2S 音频格式与 DMA 配置
 */
typedef struct {
    uint32_t sample_rate;                 /**< 采样率（Hz） */
    i2s_data_bit_width_t bit_width;       /**< 数据位宽（16/24/32bit） */
    i2s_slot_mode_t slot_mode;            /**< 单声道/立体声 */
    i2s_std_slot_mask_t slot_mask;        /**< 槽位掩码（I2S_STD_SLOT_LEFT/RIGHT/BOTH） */
    bool slot_ws_pol;                     /**< 标准模式槽位 WS 极性（INMP441 通常需要 true） */
    bool slot_bit_shift;                  /**< 标准模式槽位位偏移 */
    int dma_desc_num;                      /**< DMA 描述符数量（建议 8） */
    int dma_frame_num;                     /**< 每个描述符的帧数（建议 256） */
    bool tx_auto_clear;                    /**< TX 复用 DMA 时自动清零（推荐 true） */
} i2s_bus_cfg_t;

/* ======================== API 函数 =============================================== */

/**
 * @brief 创建 I2S 物理总线
 *
 * 根据参数创建 TX 或 RX 通道（不可同时复位双通道，一柄一方向）。
 * 采样率、位宽、引脚全部由参数决定，不再写死。
 *
 * @param port      I2S 控制器端口（I2S_NUM_0 / I2S_NUM_1）
 * @param is_tx     true=创建 TX（功放）；false=创建 RX（麦克风）
 * @param pin_cfg   引脚配置
 * @param bus_cfg   音频格式与 DMA 配置
 * @param out_handle 输出句柄
 * @return
 *     ESP_OK              成功
 *     ESP_ERR_INVALID_ARG 参数为空
 *     ESP_ERR_NO_MEM      内存不足
 *     其他                底层 i2s_new_channel 返回的错误码
 */
esp_err_t i2s_bus_phy_create(i2s_port_t port, bool is_tx,
                             const i2s_pin_cfg_t *pin_cfg,
                             const i2s_bus_cfg_t *bus_cfg,
                             i2s_bus_handle_t *out_handle);

/**
 * @brief 写入 PCM 数据（仅限 TX 总线）
 *
 * 将 PCM 数据通过 DMA 发送到 I2S 外设。若数据区满，会阻塞等待
 * 至超时（timeout 单位为 tick）。
 *
 * @param bus           句柄（必须 通过 create 创建且 is_tx=true）
 * @param buffer         数据缓冲区
 * @param size           数据长度（字节）
 * @param bytes_written  输出实际写入字节数（可为 NULL）
 * @param timeout        超时时间（tick）
 * @return
 *     ESP_OK               写入成功
 *     ESP_ERR_INVALID_ARG 句柄不匹配或参数为空
 *     ESP_FAIL              硬件层错误
 */
esp_err_t i2s_bus_phy_write(i2s_bus_handle_t bus, const uint8_t *buffer,
                            size_t size, size_t *bytes_written, uint32_t timeout);

/**
 * @brief 读取 PCM 数据（仅限 RX 总线）
 *
 * 从 DMA 接收缓冲区读取数据。若缓冲区空则阻塞等待至超时。
 *
 * @param bus       句柄（TX 方向返回错误）
 * @param buffer    接收缓冲区
 * @param size      期望读取字节数
 * @param bytes_read 输出实际读取字节数
 * @param timeout 超时时间（tick）
 * @return
 *     ESP_OK               读取成功
 *     ESP_ERR_INVALID_ARG  句柄不匹配或参数为空
 *     ESP_FAIL              硬件层错误
 */
esp_err_t i2s_bus_phy_read(i2s_bus_handle_t bus, uint8_t *buffer,
                           size_t size, size_t *bytes_read, uint32_t timeout);

/**
 * @brief 丢弃物理总线上尚未播放/未读出的数据（冲刷）
 *
 * 可视为"软复位"：禁能后重新使能通道。TX 用于强制丢弃尚未发送的
 * 旧数据（抢占场景）；RX 用于清空硬件/DMA 中残留数据。
 *
 * @param bus 句柄
 * @return ESP_OK 成功
 */
esp_err_t i2s_bus_phy_flush(i2s_bus_handle_t bus);

/**
 * @brief 动态切换采样率（不重新初始化）
 *
 * @param bus         句柄
 * @param sample_rate 新的采样率（Hz）
 * @return ESP_OK 成功
 */
esp_err_t i2s_bus_phy_set_sample_rate(i2s_bus_handle_t bus, uint32_t sample_rate);

/**
 * @brief 销毁物理总线，释放 DMA、GPIO 等资源
 *
 * @param bus 句柄（可为 NULL）
 * @return ESP_OK 成功
 */
esp_err_t i2s_bus_phy_destroy(i2s_bus_handle_t bus);

#ifdef __cplusplus
}
#endif

#endif /* __I2S_DRIVER_H__ */