/**
 * @file microphone.h
 * @brief 麦克风模块（INMP441 采集 + 内部读取任务 + 单一 FIFO）
 *
 * 本模块封装 INMP441 MEMS 麦克风的 I2S 音频采集功能，并直接承担
 * 原 audio_bus RX 侧的职责（读取任务 + FIFO）：
 *   - Microphone_Init() 创建 I2S RX 物理通道并启动内部读取任务，
 *     持续把麦克风数据写入模块私有 FIFO（满时丢最旧，保证实时性）；
 *   - 消费者（当前为 CallPhone）通过 Microphone_Read_* 阻塞读取；
 *   - 当前系统只有单个麦克风消费者，无需多读者广播，故不再提供
 *     总线/读者注册接口。
 *
 * 硬件连接（I2S RX）：
 *   - BCLK=GPIO2（连 INMP441 SCK）
 *   - LRCK=GPIO5（连 INMP441 WS）
 *   - DOUT=GPIO34（连 INMP441 SD）
 */

#ifndef MICROPHONE_H
#define MICROPHONE_H

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

/* ======================== 麦克风音频配置 ========================================= */

/** 音频采样率（Hz）：44.1kHz，全链路统一 */
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
 *   槽位掩码/引脚在 microphone.c 的 Mic_BusCfg 中配置。
 */

/* ======================== API 函数 =============================================== */

/**
 * @brief 初始化麦克风模块
 *
 * 初始化 INMP441 MEMS 麦克风，配置 I2S RX 接口（I2S_NUM_0）、
 * 创建模块 FIFO 并启动内部读取任务。
 *
 * 初始化顺序（严格按此顺序，否则麦克风无法正常工作）：
 *   1. 使能 SD 引脚内部下拉（防止三态悬空）
 *   2. 初始化 I2S RX 接口（启动 SCK/WS 时钟）
 *   3. 重新使能 SD 引脚下拉（I2S 初始化可能重置 GPIO 配置）
 *   4. 使能 INMP441（CHIPEN 控制或等待待机恢复）
 *
 * @return ESP_OK  初始化成功
 *         ESP_FAIL I2S RX 接口初始化失败
 */
esp_err_t Microphone_Init(void);

/**
 * @brief 反初始化麦克风模块
 *
 * 停止内部读取任务，释放 FIFO 与 I2S RX 接口资源。
 *
 * @return ESP_OK 反初始化成功
 */
esp_err_t Microphone_Deinit(void);

/**
 * @brief 从麦克风读取原始音频数据（32-bit 格式）
 *
 * 从模块 FIFO 读取原始 32-bit 采样数据，不做格式转换。
 * INMP441 输出 24-bit 数据，I2S 接收以 32-bit 对齐存储。
 *
 * @param buffer     接收缓冲区（uint8_t 数组）
 * @param size       缓冲区大小（字节）
 * @param bytes_read 输出参数，返回实际读取的字节数（可为 NULL）
 * @param timeout    超时时间（FreeRTOS tick 数；UINT32_MAX 表示无限等待）
 *
 * @return ESP_OK             读取成功（至少 1 字节）
 *         ESP_ERR_INVALID_ARG 参数错误
 *         ESP_ERR_INVALID_STATE 模块未初始化
 *         ESP_ERR_TIMEOUT    超时未读到任何数据
 */
esp_err_t Microphone_Read_Raw(uint8_t *buffer, size_t size, size_t *bytes_read, uint32_t timeout);

/**
 * @brief 从 FIFO 读取并转换为 PCM 16-bit 格式
 *
 * FIFO 中的 32-bit 采样取高 16 位（INMP441 24-bit 有效数据位于高 24 位）。
 * 数据流：44.1kHz / 16-bit 立体声槽位对（每帧 L/R 各一个采样，
 * 由消费方按需提取单通道）。
 *
 * @param pcmBuffer   接收缓冲区（int16_t 数组）
 * @param sampleCount 期望读取的采样数
 * @param samplesRead 输出参数，返回实际读取的采样数（可为 NULL）
 * @param timeout     超时时间（FreeRTOS tick 数；UINT32_MAX 表示无限等待）
 *
 * @return ESP_OK 读取成功（至少 1 个采样）
 *         ESP_ERR_INVALID_ARG 参数错误
 *         ESP_ERR_INVALID_STATE 模块未初始化
 *         ESP_ERR_TIMEOUT     超时未读到任何采样
 */
esp_err_t Microphone_Read_Pcm16(int16_t *pcmBuffer, size_t sampleCount, size_t *samplesRead, uint32_t timeout);

/**
 * @brief 清空麦克风 FIFO（丢弃所有未读数据）
 *
 * 用于唤醒后丢弃积压的旧数据。不阻塞正在读取的调用方。
 *
 * @return ESP_OK 清空成功
 *         ESP_ERR_INVALID_STATE 模块未初始化
 */
esp_err_t Microphone_Flush(void);

#endif /* MICROPHONE_H */
