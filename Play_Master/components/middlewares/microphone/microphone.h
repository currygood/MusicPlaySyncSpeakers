#ifndef MICROPHONE_H
#define MICROPHONE_H

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include "audio_bus.h"

/* ======================== 麦克风音频配置 ========================================= */

/** 音频采样率（Hz）：44.1kHz，与全链路（audio_bus）统一 */
#define MICROPHONE_SAMPLE_RATE    44100

/** I2S 接收位深度：32-bit（INMP441 输出 24-bit 数据，I2S 标准以 32-bit 对齐） */
#define MICROPHONE_BIT_DEPTH      32

/** PCM 输出位深度：16-bit（从 32-bit 中提取高 16 位有效数据） */
#define MICROPHONE_PCM_BIT_DEPTH  16

/** 音频通道数：1（单声道），单麦克风采集 */
#define MICROPHONE_CHANNEL_NUM    1

/** 每次读取的最大采样数：512，用于分批读取时的批量大小 */
#define MICROPHONE_READ_BUF_LEN   512

/* ======================== INMP441 硬件配置 ======================================= */

/**
 * INMP441 CHIPEN（Pin 8）GPIO 配置：
 * - 设置为实际 GPIO 编号时，MCU 将控制 CHIPEN 引脚
 * - 设置为 -1 时，表示 CHIPEN 已硬连接到 VDD（3.3V），始终使能
 *
 * ⚠️ INMP441 在 CHIPEN 为 LOW 或悬空时将不会输出任何数据！
 */
#define INMP441_CHIPEN_GPIO      -1

/**
 * INMP441 L/R（Pin 4）声道选择：
 * - L/R = GND  -> 数据在 LEFT  声道输出（I2S_STD_SLOT_LEFT）
 * - L/R = VDD  -> 数据在 RIGHT 声道输出（I2S_STD_SLOT_RIGHT）
 *
 * ⚠️ 请勿将 L/R 引脚悬空！必须明确连接到 GND 或 VDD。
 *   I2S 槽位掩码/引脚在 audio_bus 创建麦克风总线时配置（见 microphone.c + Mic_BusCfg）。
 *
 * INMP441 SD（Pin 2）下拉电阻建议：
 * - 根据数据手册，强烈建议在 SD 线上添加 100kΩ 外部下拉电阻，
 *   防止麦克风三态输出时 SD 线悬空。
 * - 驱动层使用 ESP32 内部约 45kΩ 下拉作为软件备用方案。
 */

/* ======================== API 函数 =============================================== */

/**
 * @brief 初始化麦克风模块
 *
 * 初始化 INMP441 MEMS 麦克风，配置 I2S RX 接口（I2S_NUM_0）。
 *
 * 初始化顺序（严格按此顺序，否则麦克风无法正常工作）：
 *   1. 使能 SD 引脚内部下拉（防止三态悬空）
 *   2. 初始化 I2S RX 接口（启动 SCK/WS 时钟）
 *   3. 重新使能 SD 引脚下拉（I2S 初始化可能重置 GPIO 配置）
 *   4. 使能 INMP441（CHIPEN 控制或等待待机恢复）
 *
 * 注意：SCK 和 WS 时钟必须在 CHIPEN 拉高之前运行，
 *       否则 INMP441 会进入待机模式，无法正常初始化。
 *
 * @return ESP_OK  初始化成功
 *         ESP_FAIL I2S RX 接口初始化失败
 */
esp_err_t Microphone_Init(void);

/**
 * @brief 反初始化麦克风模块
 *
 * 关闭 I2S RX 接口，停止时钟输出，释放 DMA 缓冲区。
 *
 * @return ESP_OK 反初始化成功
 */
esp_err_t Microphone_Deinit(void);

/**
 * @brief 从麦克风读取原始音频数据（32-bit 格式）
 *
 * 直接从 I2S RX 接口读取原始 32-bit 采样数据，不做任何格式转换。
 * INMP441 输出 24-bit 数据，I2S 接收时以 32-bit 对齐存储。
 *
 * @param buffer     接收缓冲区（uint8_t 数组）
 * @param size       缓冲区大小（字节）
 * @param bytes_read 输出参数，返回实际读取的字节数（可为 NULL）
 * @param timeout    超时时间（FreeRTOS tick 数）
 *
 * @return ESP_OK             读取成功
 *         ESP_ERR_INVALID_ARG buffer 为 NULL 或 size 为 0
 *         ESP_FAIL            读取失败
 */
esp_err_t Microphone_Read_Raw(uint8_t *buffer, size_t size, size_t *bytes_read, uint32_t timeout);

/**
 * @brief 从麦克风读取并转换为 PCM 16-bit 格式
 *
 * 从 I2S RX 读取 32-bit 原始采样数据，提取高 16 位有效数据，
 * 转换为标准的 PCM 16-bit 格式。
 *
 * 转换逻辑：INMP441 输出 24-bit 数据，存放在 I2S 32-bit 帧的
 * 高 24 位中。取高 16 位（右移 16 位）即可获得有效的 16-bit PCM 数据。
 *
 * 数据格式：
 *   - 采样率：44.1kHz
 *   - 位深度：16-bit
 *   - 通道数：1（单声道）
 *
 * @param pcmBuffer   接收缓冲区（int16_t 数组，16-bit 有符号 PCM）
 * @param sampleCount 期望读取的采样数
 * @param samplesRead 输出参数，返回实际读取的采样数（可为 NULL）
 * @param timeout     超时时间（FreeRTOS tick 数）
 *
 * @return ESP_OK             读取成功（至少读取了 1 个采样）
 *         ESP_ERR_INVALID_ARG pcmBuffer 为 NULL 或 sampleCount 为 0
 *         ESP_FAIL            未读取到任何采样
 */
esp_err_t Microphone_Read_Pcm16(int16_t *pcmBuffer, size_t sampleCount, size_t *samplesRead, uint32_t timeout);

/**
 * @brief 获取麦克风模块的 RX 音频总线句柄（仅供装配层接线复用）
 *
 * 必须在 Microphone_Init() 成功之后调用；未初始化返回 NULL。
 * 总线归属：RX 总线由 microphone 模块私有创建并持有，Microphone_GetBus()
 * 仅供装配层借出句柄（如 CallPhone 注册 reader），不转让所有权；
 * 销毁统一由 Microphone_Deinit() 负责，调用方不得单独调用 audio_bus_destroy()。
 *
 * @return RX 音频总线句柄；未初始化返回 NULL
 */
audio_bus_handle_t Microphone_GetBus(void);

#endif
