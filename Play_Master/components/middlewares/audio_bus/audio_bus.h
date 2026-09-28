/**
 * @file audio_bus.h
 * @brief 音频总线（多生产者 / 多消费者仲裁层）
 *
 * I2S 不像 I2C 有"地址"概念，整个系统仍然只有一份物理功放（TX）
 * 和一份物理麦克风（RX）。因此本层用"软件仲裁"把单条物理 I2S
 * 总线共享给多个上层 App：
 *
 *   - TX（多生产者）：多个 writer 注册句柄，写操作以"块"为单位
 *     串行化；同一时刻只允许一个 writer 占用总线，防止数据交错。
 *   - RX（多消费者）：内部任务持续读取麦克风，把同一份数据广播
 *     到每个 reader 的私有 FIFO；某个消费者处理慢只丢自己的数据，
 *     不影响其他消费者。
 *
 * 本层位于 middlewares，只依赖 bsp 的 i2s_driver（物理驱动），
 * 不包含任何业务逻辑。
 *
 * 采样率约束：全链路固定 44.1kHz（见 AUDIO_BUS_SAMPLE_RATE），
 * create() 时会对 bus_cfg->sample_rate 做一致性校验，强制覆盖并告警。
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

/* ======================== 固定采样率 ============================================== */

/**
 * 全链路固定采样率：44.1kHz。
 * 手机 A2DP（典型 44.1k）、SD 卡解码、麦克风采集、功放输出均以此为基线；
 * 若音源协商到其它采样率（48k/32k/16k 等），由对应音源模块内部重采样对齐。
 */
#define AUDIO_BUS_SAMPLE_RATE  44100

/* ======================== 类型定义 =============================================== */

/** 音频总线句柄（一个方向一条总线） */
typedef struct audio_bus_s *audio_bus_handle_t;

/** 写者句柄（TX 生产者） */
typedef struct audio_writer_s *audio_writer_handle_t;

/** 读者句柄（RX 消费者） */
typedef struct audio_reader_s *audio_reader_handle_t;

/** 总线方向 */
typedef enum {
    AUDIO_BUS_TX = 0,   /**< 输出总线（功放） */
    AUDIO_BUS_RX = 1,   /**< 输入总线（麦克风） */
} audio_bus_dir_t;

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

/**
 * @brief RX FIFO 溢出策略
 * - AUDIO_FIFO_DROP_OLDEST: 丢弃最旧的数据，保证读到的是新数据
 * - AUDIO_FIFO_DROP_NEW   : 丢弃本次新数据，保留旧数据
 */
typedef enum {
    AUDIO_FIFO_DROP_OLDEST = 0,
    AUDIO_FIFO_DROP_NEW = 1,
} audio_fifo_overflow_t;

/* ======================== 总线创建/销毁 ========================================= */

/**
 * @brief 创建音频总线
 *
 * 内部会创建对应方向的物理 I2S 句柄（TX 用 I2S_NUM_1，RX 用
 * I2S_NUM_0，由调用方通过 port 指定）。RX 总线创建后会启动一个
 * 内部读取任务，持续把麦克风数据广播给所有已注册的 reader。
 *
 * @param dir     总线方向
 * @param port    I2S 控制器端口（I2S_NUM_0 / I2S_NUM_1）
 * @param pin_cfg 物理引脚配置（功放/麦克风）
 * @param bus_cfg 音频格式与 DMA 配置（采样率会被强制为 AUDIO_BUS_SAMPLE_RATE=44100，不一致时覆盖并告警）
 * @return 总线句柄，失败返回 NULL
 */
audio_bus_handle_t audio_bus_create(audio_bus_dir_t dir, i2s_port_t port,
                                    const i2s_pin_cfg_t *pin_cfg,
                                    const i2s_bus_cfg_t *bus_cfg);

/**
 * @brief 销毁总线（必须先注销全部 writer/reader）
 * @param bus 总线句柄（可为 NULL）
 * @return ESP_OK 成功
 */
esp_err_t audio_bus_destroy(audio_bus_handle_t bus);

/**
 * @brief 获取音频总线内部的 I2S 物理总线句柄（调试/底层访问用）
 *
 * @param bus 音频总线句柄
 * @return I2S 物理总线句柄；bus 为 NULL 时返回 NULL
 */
i2s_bus_handle_t audio_bus_get_phy(audio_bus_handle_t bus);

/* ======================== TX：多生产者接口 ========================================= */

/**
 * @brief 注册一个写者（生产者）
 *
 * @param bus   TX 方向总线；RX 总线返回 ESP_ERR_INVALID_ARG
 * @param name  写者名称（用于日志调试，如 "bluetooth"、"multicast"）
 * @param out   输出写者句柄
 * @return ESP_OK / 错误码
 */
esp_err_t audio_writer_register(audio_bus_handle_t bus, const char *name,
                                audio_writer_handle_t *out);

/**
 * @brief 原子写入一块 PCM 数据（多生产者串行化）
 *
 * 数据格式：16-bit 采样，单声道/立体声由总线配置决定。
 * 函数内部会校验写者句柄所属总线必须是 TX 方向。
 *
 * @param writer    写者句柄
 * @param pcm       PCM 数据（int16_t 数组）
 * @param samples   采样数（单声道=帧数；立体声=帧数×2）
 * @param timeout_ms 超时时间（毫秒）
 * @param policy    写入策略
 * @return
 *     ESP_OK                写入成功
 *     ESP_ERR_INVALID_ARG   句柄无效/方向错误/参数错误
 *     ESP_ERR_TIMEOUT       等待写锁超时（BLOCK）/ 等待写入超时
 *     ESP_ERR_NOT_ALLOWED     总线忙且策略为 FAIL_IF_BUSY
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

/* ======================== RX 多消费者注册 ========================================= */

/**
 * @brief 注册一个读者（消费者）
 *
 * 为读者分配私有 FIFO，总线读取任务会把麦克风数据广播进来。
 *
 * @param bus        RX 总线；传入 TX 总线返回 ESP_ERR_INVALID_ARG
 * @param name       读者名称（调试用）
 * @param fifo_bytes FIFO 容量（字节）。按固定采样率 44.1kHz×4 字节/样本，
 *                   1 秒 ≈ 176KB，建议至少 8~32KB
 * @param overflow   FIFO 满时的溢出策略
 * @param out        输出读者句柄
 * @return ESP_OK / 错误码
 */
esp_err_t audio_reader_register(audio_bus_handle_t bus, const char *name,
                                size_t fifo_bytes, audio_fifo_overflow_t overflow,
                                audio_reader_handle_t *out);

/**
 * @brief 读取原始 32-bit 采样数据（不做格式转换）
 *
 * 与旧 Microphone_Read_Raw 语义一致：尽可能读满，不足时等待。
 * 内部会校验读者句柄为 RX 总线。
 *
 * @param reader    读者句柄
 * @param buffer    接收缓冲区
 * @param size      期望读取字节数（32-bit 采样：4 字节/样本）
 * @param bytes_read 输出实际读取字节数（可为 NULL）
 * @param timeout_ms 超时时间（毫秒），FIFO 为空时等待
 * @return
 *     ESP_OK               至少读到了数据
 *     ESP_ERR_INVALID_ARG  句柄/参数错误
 *     ESP_ERR_TIMEOUT      FIFO 一直为空，超时未读到数据
 */
esp_err_t audio_reader_read_raw(audio_reader_handle_t reader, uint8_t *buffer,
                                size_t size, size_t *bytes_read, uint32_t timeout_ms);

/**
 * @brief 读取并转换为 16-bit PCM（右移 16 位取高 16 位）
 *
 * @param reader    读者句柄
 * @param pcm       输出缓冲区（int16_t）
 * @param samples   期望读取的采样数
 * @param samples_read 输出实际读取采样数（可为 NULL）
 * @param timeout_ms 超时时间（毫秒）
 * @return ESP_OK / 错误码（同 audio_reader_read_raw）
 */
esp_err_t audio_reader_read_pcm16(audio_reader_handle_t reader, int16_t *pcm,
                                  size_t samples, size_t *samples_read,
                                  uint32_t timeout_ms);

/**
 * @brief 清空读者 FIFO（丢弃所有未读数据）
 * @param reader 读者句柄
 * @return ESP_OK
 */
esp_err_t audio_reader_flush(audio_reader_handle_t reader);

/**
 * @brief 注销读者，释放 FIFO
 * @param reader 读者句柄
 * @return ESP_OK
 */
esp_err_t audio_reader_unregister(audio_reader_handle_t reader);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_BUS_H */
