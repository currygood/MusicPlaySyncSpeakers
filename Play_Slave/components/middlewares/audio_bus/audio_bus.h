/**
 * @file audio_bus.h
 * @brief 音频总线（多生产者仲裁层，从节点：仅 TX 功放）
 *
 * Play_Slave 只带功放、没有麦克风，因此本层只保留 TX 方向
 * （多生产者）：
 *
 *   - 多个 writer 注册写者句柄，写操作以"块"为单位串行化；
 *   - 同一时刻只允许一个 writer 占用物理 I2S（功放），防止数据交错。
 *
 * 本层位于 middlewares，只依赖 bsp 的 i2s_driver（物理驱动），
 * 不包含任何业务逻辑。RX（麦克风）能力在从节点不编译。
 */

#ifndef AUDIO_BUS_H
#define AUDIO_BUS_H

#include "esp_err.h"
#include "i2s_driver.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ======================== 类型定义 =============================================== */

/** 音频总线句柄（仅 TX：功放输出） */
typedef struct audio_bus_s *audio_bus_handle_t;

/** 写者句柄（TX 生产者） */
typedef struct audio_writer_s *audio_writer_handle_t;

/**
 * @brief TX 写入策略
 *
 * 当总线上已有其他 writer 正在写入时的行为：
 *   - AUDIO_WRITE_BLOCK       : 阻塞等待写锁，直到超时（默认推荐）
 *   - AUDIO_WRITE_BYPASS_OLD  : 抢占：丢弃尚未播放的旧数据，
 *                               等待旧 writer 释放后立即写入新数据
 *   - AUDIO_WRITE_FAIL_IF_BUSY: 不等待，总线忙立即返回 ESP_ERR_NOT_ALLOWED
 */
typedef enum {
    AUDIO_WRITE_BLOCK = 0,
    AUDIO_WRITE_BYPASS_OLD = 1,
    AUDIO_WRITE_FAIL_IF_BUSY = 2,
} audio_write_policy_t;

/* ======================== 总线创建/销毁 ========================================= */

/**
 * @brief 创建 TX 音频总线（功放输出）
 *
 * 从节点只有功放，总线固定为 TX 方向，因此不再有方向参数。
 * 内部会创建物理 I2S 发送句柄，并初始化多生产者写锁。
 *
 * @param port       I2S 控制器端口（I2S_NUM_0 / I2S_NUM_1）
 * @param pin_cfg    物理引脚配置（功放）
 * @param bus_cfg    音频格式与 DMA 配置
 * @return 总线句柄，失败返回 NULL
 */
audio_bus_handle_t audio_bus_create(i2s_port_t port,
                                    const i2s_pin_cfg_t *pin_cfg,
                                    const i2s_bus_cfg_t *bus_cfg);

/**
 * @brief 销毁总线（必须先注销全部 writer）
 * @param bus 总线句柄（可为 NULL）
 * @return ESP_OK 成功
 */
esp_err_t audio_bus_destroy(audio_bus_handle_t bus);

/* ======================== TX：多生产者接口 ========================================= */

/**
 * @brief 注册一个写者（生产者）
 *
 * @param bus   TX 总线（功放）
 * @param name  写者名称（用于日志调试，如 "multicast"、"beep"）
 * @param out   输出写者句柄
 * @return ESP_OK / 错误码
 */
esp_err_t audio_writer_register(audio_bus_handle_t bus, const char *name,
                                audio_writer_handle_t *out);

/**
 * @brief 原子写入一块 PCM 数据（多生产者串行化）
 *
 * 数据格式：16-bit 采样，单声道/立体声由总线配置决定。
 * 发送函数内部会校验写者句柄属于 TX 总线（从节点唯一方向）。
 *
 * @param writer     写者句柄
 * @param pcm        PCM 数据（int16_t 数组）
 * @param samples    采样数（单声道=帧数；立体声=帧数×2）
 * @param timeout_ms 超时时间（毫秒）
 * @param policy     写入策略
 * @return
 *     ESP_OK                写入成功
 *     ESP_ERR_INVALID_ARG   句柄无效/方向错误/参数错误
 *     ESP_ERR_TIMEOUT       等待写锁超时（BLOCK）/ 等待写入超时
 *     ESP_ERR_NOT_ALLOWED   总线忙且策略为 FAIL_IF_BUSY
 *     ESP_FAIL              硬件写入失败
 */
esp_err_t audio_writer_write(audio_writer_handle_t writer, const int16_t *pcm,
                             size_t samples, uint32_t timeout_ms,
                             audio_write_policy_t policy);

/**
 * @brief 注销写者
 * @param writer 写者句柄（注销后句柄失效）
 * @return ESP_OK
 */
esp_err_t audio_writer_unregister(audio_writer_handle_t writer);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_BUS_H */
