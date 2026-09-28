/**
 * @file audio_bus.c
 * @brief 音频总线实现（从节点：仅 TX 多生产者）
 *
 * 从节点只有功放、没有麦克风，所以本实现只保留 TX 侧：
 * 写锁 + 写入策略。一次 audio_writer_write() 调用 = 一个原子块，
 * 块内数据不会与其他 writer 交错。
 *
 * 本层不做任何业务逻辑：谁在写、由上层 App 决定。
 */

#include "audio_bus.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdlib.h>
#include <string.h>

#define TAG "AUDIO_BUS"

/* 句柄魔数，用于校验非法/悬空句柄 */
#define AUDIO_BUS_MAGIC    0x41554253u /* 'AUBS' */
#define AUDIO_WRITER_MAGIC 0x57525452u /* 'WRTR' */

/** 写者名称最大长度 */
#define AUDIO_BUS_NAME_MAX 16

/* ======================== 结构定义 =============================================== */

struct audio_bus_s {
    uint32_t magic;             /**< 魔数 */
    i2s_port_t port;            /**< I2S 控制器端口 */
    i2s_bus_handle_t phy;       /**< 物理总线句柄 */
    i2s_pin_cfg_t pin_cfg;      /**< 引脚配置备份 */
    i2s_bus_cfg_t bus_cfg;      /**< 格式配置备份（字节换算用） */
    SemaphoreHandle_t write_mutex;  /**< TX 写锁 */
};

struct audio_writer_s {
    uint32_t magic;             /**< 魔数 */
    audio_bus_handle_t bus;     /**< 所属总线 */
    char name[AUDIO_BUS_NAME_MAX];
};

/* ======================== 内部工具 =============================================== */

static bool audio_bus_valid(audio_bus_handle_t bus)
{
    return (bus != NULL) && (bus->magic == AUDIO_BUS_MAGIC);
}

static bool audio_writer_valid(audio_writer_handle_t writer)
{
    return (writer != NULL) && (writer->magic == AUDIO_WRITER_MAGIC);
}

/** 每个 PCM 采样占据的字节数（按配置的位宽与声道数） */
static size_t audio_bus_bytes_per_sample(const i2s_bus_cfg_t *cfg)
{
    size_t channels = (cfg->slot_mode == I2S_SLOT_MODE_STEREO) ? 2 : 1;
    return (cfg->bit_width / 8) * channels;
}

/* ======================== 总线创建/销毁 ========================================== */

audio_bus_handle_t audio_bus_create(i2s_port_t port,
                                    const i2s_pin_cfg_t *pin_cfg,
                                    const i2s_bus_cfg_t *bus_cfg)
{
    if (pin_cfg == NULL || bus_cfg == NULL)
    {
        ESP_LOGE(TAG, "NULL config");
        return NULL;
    }

    audio_bus_handle_t bus = calloc(1, sizeof(struct audio_bus_s));
    if (bus == NULL)
    {
        ESP_LOGE(TAG, "no memory for bus");
        return NULL;
    }
    /* 采样率强制为全链路统一的 44.1kHz（与主节点对齐） */
    i2s_bus_cfg_t cfg = *bus_cfg;
    if (cfg.sample_rate != AUDIO_BUS_SAMPLE_RATE)
    {
        ESP_LOGW(TAG, "sample_rate=%u != %u, forcing to unified 44.1kHz",
                 (unsigned)cfg.sample_rate, (unsigned)AUDIO_BUS_SAMPLE_RATE);
        cfg.sample_rate = AUDIO_BUS_SAMPLE_RATE;
    }

    bus->magic = AUDIO_BUS_MAGIC;
    bus->port = port;
    memcpy(&bus->pin_cfg, pin_cfg, sizeof(bus->pin_cfg));
    memcpy(&bus->bus_cfg, &cfg, sizeof(bus->bus_cfg));

    /* 创建物理层发送句柄（从节点总线固定为 TX 功放） */
    esp_err_t ret = i2s_bus_phy_create(port, true, pin_cfg, &cfg, &bus->phy);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "phy create failed: %s", esp_err_to_name(ret));
        free(bus);
        return NULL;
    }

    /* TX：创建写锁，多生产者串行化 */
    bus->write_mutex = xSemaphoreCreateMutex();
    if (bus->write_mutex == NULL)
    {
        i2s_bus_phy_destroy(bus->phy);
        free(bus);
        return NULL;
    }

    ESP_LOGI(TAG, "TX bus created: port=%d, sample_rate=%u",
             (int)port, (unsigned)bus_cfg->sample_rate);
    return bus;
}

esp_err_t audio_bus_destroy(audio_bus_handle_t bus)
{
    if (!audio_bus_valid(bus))
    {
        return ESP_ERR_INVALID_ARG;
    }

    vSemaphoreDelete(bus->write_mutex);
    i2s_bus_phy_destroy(bus->phy);
    bus->magic = 0;
    free(bus);
    ESP_LOGI(TAG, "bus destroyed");
    return ESP_OK;
}

/* ======================== TX：多生产者接口 ========================================= */

esp_err_t audio_writer_register(audio_bus_handle_t bus, const char *name,
                                audio_writer_handle_t *out)
{
    if (!audio_bus_valid(bus) || name == NULL || out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    audio_writer_handle_t writer = calloc(1, sizeof(struct audio_writer_s));
    if (writer == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    writer->magic = AUDIO_WRITER_MAGIC;
    writer->bus = bus;
    strncpy(writer->name, name, AUDIO_BUS_NAME_MAX - 1);
    writer->name[AUDIO_BUS_NAME_MAX - 1] = '\0';

    ESP_LOGI(TAG, "writer[%s] registered", writer->name);
    *out = writer;
    return ESP_OK;
}

esp_err_t audio_writer_write(audio_writer_handle_t writer, const int16_t *pcm,
                             size_t samples, uint32_t timeout_ms,
                             audio_write_policy_t policy)
{
    /* 发送函数必须校验是发送句柄，且属于 TX 总线 */
    if (!audio_writer_valid(writer) || pcm == NULL || samples == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    audio_bus_handle_t bus = writer->bus;
    if (!audio_bus_valid(bus))
    {
        ESP_LOGE(TAG, "writer[%s] bus invalid", writer->name);
        return ESP_ERR_INVALID_ARG;
    }

    TickType_t wait_ticks = (timeout_ms == UINT32_MAX)
        ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);

    /* 按策略获取写锁 */
    if (policy == AUDIO_WRITE_FAIL_IF_BUSY)
    {
        if (xSemaphoreTake(bus->write_mutex, 0) != pdTRUE)
        {
            ESP_LOGW(TAG, "writer[%s] bus busy, fail immediately", writer->name);
            return ESP_ERR_NOT_ALLOWED;
        }
    }
    else if (policy == AUDIO_WRITE_BYPASS_OLD)
    {
        if (xSemaphoreTake(bus->write_mutex, 0) != pdTRUE)
        {
            /* 抢占：先丢弃 DMA 中尚未播放的旧数据，让新数据尽快上总线 */
            ESP_LOGW(TAG, "writer[%s] bypass: flushing old data", writer->name);
            i2s_bus_phy_flush(bus->phy);
            if (xSemaphoreTake(bus->write_mutex, wait_ticks) != pdTRUE)
            {
                return ESP_ERR_TIMEOUT;
            }
        }
    }
    else /* AUDIO_WRITE_BLOCK（默认） */
    {
        if (xSemaphoreTake(bus->write_mutex, wait_ticks) != pdTRUE)
        {
            ESP_LOGW(TAG, "writer[%s] lock timeout", writer->name);
            return ESP_ERR_TIMEOUT;
        }
    }

    /* 发送函数内部校验：从节点总线为 TX（功放），字节换算按总线配置 */
    uint8_t bytes_per_sample = audio_bus_bytes_per_sample(&bus->bus_cfg);
    size_t bytes = samples * bytes_per_sample;

    size_t bytes_written = 0;
    esp_err_t ret = i2s_bus_phy_write(bus->phy, (const uint8_t *)pcm, bytes,
                                      &bytes_written, wait_ticks);
    xSemaphoreGive(bus->write_mutex);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "writer[%s] phy write failed: %s",
                 writer->name, esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t audio_writer_unregister(audio_writer_handle_t writer)
{
    if (!audio_writer_valid(writer))
    {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "writer[%s] unregistered", writer->name);
    writer->magic = 0;
    writer->bus = NULL;
    free(writer);
    return ESP_OK;
}
