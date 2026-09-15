/**
 * @file amplifier.h
 * @brief 功放模块接口（基于 NS4168 D 类功放芯片）
 *
 * 本模块封装 NS4168 功放芯片的 I2S 音频输出功能。NS4168 是一款
 * 5W 单声道 D 类功放芯片，通过 I2S 标准协议接收 PCM 音频数据，
 * 驱动扬声器发声。
 *
 * 核心功能：
 *   - 初始化：配置 I2S TX 接口（I2S_NUM_1），启动时钟输出
 *   - 播放：将 PCM 16-bit 单声道音频数据通过 I2S 发送到 NS4168
 *   - 音量控制：保存音量值（软件层面，NS4168 本身不支持 I2C 音量调节）
 *
 * 硬件连接（I2S TX）：
 *   - BCLK  GPIO26 （I2S 位时钟，Bit Clock）
 *   - LRCK  GPIO27 （I2S 左右声道时钟，Left/Right Clock）
 *   - DIN   GPIO13 （I2S 数据输入，Data IN）
 *
 * 依赖模块：
 *   - audio_bus：通过音频总线注册写者，多路音源共享功放（总线仲裁）
 */
#ifndef AMPLIFIER_H
#define AMPLIFIER_H

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

/* ======================== 功放音频配置 =========================================== */

/** 音频采样率（Hz）：16kHz，满足语音频段需求（0~8kHz） */
#define AMPLIFIER_SAMPLE_RATE  16000

/** 音频位深度：16-bit，每个采样点 2 字节 */
#define AMPLIFIER_BIT_DEPTH    16

/** 音频通道数：1（单声道），语音播放不需要立体声 */
#define AMPLIFIER_CHANNEL_NUM  1

/** 最大音量值（百分比），范围 0~100 */
#define AMPLIFIER_VOLUME_MAX   100

/** 最小音量值（百分比），0 表示静音 */
#define AMPLIFIER_VOLUME_MIN   0

/* ======================== API 函数 =============================================== */

/**
 * @brief 初始化功放模块
 *
 * 初始化 I2S TX（I2S_NUM_1）接口，准备音频输出到 NS4168 功放芯片。
 * NS4168 是一款 5W 单声道 D 类功放芯片，通过 I2S 接口接收 PCM 音频数据。
 *
 * 注意：初始化后需要调用 Amplifier_Play_Buffer() 才能播放音频。
 *
 * @return ESP_OK  初始化成功
 *         ESP_FAIL I2S 接口初始化失败
 */
esp_err_t Amplifier_Init(void);

/**
 * @brief 反初始化功放模块
 *
 * 关闭 I2S TX 接口，释放 DMA 缓冲区等资源。
 *
 * @return ESP_OK 反初始化成功
 */
esp_err_t Amplifier_Deinit(void);

/**
 * @brief 通过功放播放 PCM 音频缓冲区
 *
 * 将 PCM 16-bit 单声道音频数据通过 I2S 总线发送到 NS4168 功放芯片，
 * 驱动扬声器发声。
 *
 * @param buffer        指向 PCM 音频数据的指针（16-bit 单声道，小端字节序）
 * @param size          缓冲区大小（字节）
 * @param bytes_written 输出参数，返回实际写入的字节数（可为 NULL）
 * @param timeout       超时时间（FreeRTOS tick 数），若 I2S DMA 缓冲区满则等待
 *
 * @return ESP_OK             播放成功
 *         ESP_ERR_INVALID_ARG buffer 为 NULL 或 size 为 0
 *         ESP_FAIL            I2S 写入失败
 */
esp_err_t Amplifier_Play_Buffer(const uint8_t *buffer, size_t size, size_t *bytes_written, uint32_t timeout);

/**
 * @brief 设置功放音量
 *
 * 注意：当前实现仅保存音量值，未实现硬件音量控制（NS4168 不支持
 *       软件音量调节，需通过 PWM 或外部功放芯片实现）。
 *
 * @param volume 音量值（0~100），超出范围自动截断到 100
 *
 * @return ESP_OK 设置成功
 */
esp_err_t Amplifier_Set_Volume(uint8_t volume);

/**
 * @brief 获取当前音量值
 *
 * @return 当前音量值（0~100）
 */
uint8_t Amplifier_Get_Volume(void);

#endif