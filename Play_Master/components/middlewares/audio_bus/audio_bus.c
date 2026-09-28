/**
 * @file audio_bus.c
 * @brief 音频总线实现（多生产者 / 多消费者仲裁层）
 *
 * TX 侧：写锁 + 写入策略。一次 audio_writer_write() 调用 = 一个原子
 *       块，块内数据不会与其他 writer 交错。
 * RX 侧：内部任务阻塞读物理 I2S，把同一份采样广播到每个 reader 的
 *       私有 FreeRTOS StreamBuffer；某个 FIFO 满了只影响它自己，
 *       溢出行为由该 reader 的溢出策略决定。
 *
 * 本层不做任何业务逻辑：谁在写、谁在读都由上层 App 决定。
 */

#include "audio_bus.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdlib.h>
#include <string.h>

#define TAG "AUDIO_BUS"

/* 句柄魔数，用于校验非法/悬空句柄 */
#define AUDIO_BUS_MAGIC    0x41554253u /* 'AUBS' */
#define AUDIO_WRITER_MAGIC 0x57525452u /* 'WRTR' */
#define AUDIO_READER_MAGIC 0x52445254u /* 'RDRT' */

/** RX 读取任务每次从物理层搬运的采样数（32-bit 采样） */
#define AUDIO_BUS_RX_CHUNK_SAMPLES 512

/** RX 任务物理读取的阻塞上限，用于及时响应销毁请求 */
#define AUDIO_BUS_RX_READ_TIMEOUT_MS 100

/** 写者/读者名称最大长度 */
#define AUDIO_BUS_NAME_MAX 16

/* ======================== 结构定义 =============================================== */

struct audio_bus_s {
    uint32_t magic;             /**< 魔数 */
    audio_bus_dir_t dir;        /**< 总线方向 */
    i2s_port_t port;            /**< I2S 控制器端口 */
    i2s_bus_handle_t phy;       /**< 物理总线句柄 */
    i2s_pin_cfg_t pin_cfg;      /**< 引脚配置备份 */
    i2s_bus_cfg_t bus_cfg;      /**< 格式配置备份（字节换算用） */
    SemaphoreHandle_t write_mutex;  /**< TX 写锁 */
    /* ---- RX 专用 ---- */
    TaskHandle_t rx_task;           /**< 麦克风读取任务 */
    volatile bool destroying;       /**< 销毁标志，通知 RX 任务退出 */
    struct audio_reader_s **readers; /**< reader 列表 */
    size_t reader_count;            /**< reader 数量 */
    SemaphoreHandle_t reader_mutex; /**< reader 列表保护锁 */
    uint8_t rx_chunk[AUDIO_BUS_RX_CHUNK_SAMPLES * 4]; /**< RX 采集缓冲（2KB） */
};

struct audio_writer_s {
    uint32_t magic;             /**< 魔数 */
    audio_bus_handle_t bus;     /**< 所属总线 */
    char name[AUDIO_BUS_NAME_MAX];
};

struct audio_reader_s {
    uint32_t magic;             /**< 魔数 */
    audio_bus_handle_t bus;     /**< 所属总线 */
    char name[AUDIO_BUS_NAME_MAX];
    StreamBufferHandle_t fifo;  /**< 私有 FIFO */
    audio_fifo_overflow_t overflow; /**< 溢出策略 */
    size_t dropped_bytes;       /**< 累计丢弃字节数（调试用） */
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

static bool audio_reader_valid(audio_reader_handle_t reader)
{
    return (reader != NULL) && (reader->magic == AUDIO_READER_MAGIC);
}

/** 每个 PCM 采样占据的字节数（按配置的位宽与声道数） */
static size_t audio_bus_bytes_per_sample(const i2s_bus_cfg_t *cfg)
{
    size_t channels = (cfg->slot_mode == I2S_SLOT_MODE_STEREO) ? 2 : 1;
    return (cfg->bit_width / 8) * channels;
}

/**
 * @brief 向 reader 的私有 FIFO 推送一块数据
 *
 * 空间足够：直接写入。
 * 空间不足：DROP_NEW 丢弃新块；DROP_OLDEST 先把最旧的数据
 * 读走丢弃，再把新块写入。
 */
static void audio_bus_fifo_push(audio_reader_handle_t reader,
                                const uint8_t *data, size_t len)
{
    size_t spaces = xStreamBufferSpacesAvailable(reader->fifo);

    if (spaces >= len)
    {
        xStreamBufferSend(reader->fifo, data, len, 0);
        return;
    }

    if (reader->overflow == AUDIO_FIFO_DROP_NEW)
    {
        /* 新数据整块丢弃，保留旧数据 */
        reader->dropped_bytes += len;
        ESP_LOGW(TAG, "reader[%s] FIFO full, dropped new %d bytes",
                 reader->name, (int)len);
        return;
    }

    /* DROP_OLDEST：把最旧的数据读走丢弃，腾出空间 */
    size_t need_drop = len - spaces;
    uint8_t tmp[128];
    while (need_drop > 0)
    {
        size_t drop_now = (need_drop > sizeof(tmp)) ? sizeof(tmp) : need_drop;
        size_t got = xStreamBufferReceive(reader->fifo, tmp, drop_now, 0);
        if (got == 0)
        {
            break; /* 理论不会发生：spaces 已显示有空位 */
        }
        reader->dropped_bytes += got;
        need_drop -= got;
    }
    xStreamBufferSend(reader->fifo, data, len, 0);
}

/* ======================== RX 内部读取任务 ========================================= */

static void audio_bus_rx_task(void *arg)
{
    audio_bus_handle_t bus = (audio_bus_handle_t)arg;

    ESP_LOGI(TAG, "RX task started");

    while (!bus->destroying)
    {
        /* 有限超时读取，保证销毁请求能被及时响应 */
        size_t bytes_read = 0;
        esp_err_t ret = i2s_bus_phy_read(bus->phy, bus->rx_chunk,
                                         sizeof(bus->rx_chunk), &bytes_read,
                                         AUDIO_BUS_RX_READ_TIMEOUT_MS);
        if (ret != ESP_OK)
        {
            if (ret == ESP_ERR_TIMEOUT)
            {
                continue; /* 没有新数据，回去检查销毁标志 */
            }
            ESP_LOGE(TAG, "phy read failed: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (bytes_read == 0)
        {
            continue;
        }

        /* 广播到所有 reader 的私有 FIFO */
        if (xSemaphoreTake(bus->reader_mutex, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }
        for (size_t i = 0; i < bus->reader_count; i++)
        {
            audio_bus_fifo_push(bus->readers[i], bus->rx_chunk, bytes_read);
        }
        xSemaphoreGive(bus->reader_mutex);
    }

    /* 自删前把句柄清空，通知 audio_bus_destroy() 可以回收 */
    bus->rx_task = NULL;
    ESP_LOGI(TAG, "RX task exit");
    vTaskDelete(NULL);
}

/* ======================== 总线创建/销毁 ======================= */

audio_bus_handle_t audio_bus_create(audio_bus_dir_t dir, i2s_port_t port,
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

    /* 采样率强制为全链路统一的 44.1kHz（防止调用方配错/遗漏） */
    i2s_bus_cfg_t cfg = *bus_cfg;
    if (cfg.sample_rate != AUDIO_BUS_SAMPLE_RATE)
    {
        ESP_LOGW(TAG, "sample_rate=%u != %u, forcing to unified 44.1kHz",
                 (unsigned)cfg.sample_rate, (unsigned)AUDIO_BUS_SAMPLE_RATE);
        cfg.sample_rate = AUDIO_BUS_SAMPLE_RATE;
    }

    bus->magic = AUDIO_BUS_MAGIC;
    bus->dir = dir;
    bus->port = port;
    memcpy(&bus->pin_cfg, pin_cfg, sizeof(bus->pin_cfg));
    memcpy(&bus->bus_cfg, &cfg, sizeof(bus->bus_cfg));

    /* 创建物理层句柄（一柄一方向） */
    esp_err_t ret = i2s_bus_phy_create(port, (dir == AUDIO_BUS_TX),
                                       pin_cfg, &cfg, &bus->phy);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "phy create failed: %s", esp_err_to_name(ret));
        free(bus);
        return NULL;
    }

    if (dir == AUDIO_BUS_TX)
    {
        /* TX：创建写锁，多生产者串行化 */
        bus->write_mutex = xSemaphoreCreateMutex();
        if (bus->write_mutex == NULL)
        {
            i2s_bus_phy_destroy(bus->phy);
            free(bus);
            return NULL;
        }
    }
    else
    {
        /* RX：创建 reader 列表锁 + 麦克风读取任务 */
        bus->reader_mutex = xSemaphoreCreateMutex();
        if (bus->reader_mutex == NULL)
        {
            i2s_bus_phy_destroy(bus->phy);
            free(bus);
            return NULL;
        }

        BaseType_t created = xTaskCreate(audio_bus_rx_task, "audio_bus_rx",
                                         4096, bus, 10, &bus->rx_task);
        if (created != pdPASS)
        {
            ESP_LOGE(TAG, "RX task create failed");
            i2s_bus_phy_destroy(bus->phy);
            vSemaphoreDelete(bus->reader_mutex);
            free(bus);
            return NULL;
        }
    }

    ESP_LOGI(TAG, "%s bus created: port=%d, sample_rate=%u",
             (dir == AUDIO_BUS_TX) ? "TX" : "RX",
             (int)port, (unsigned)bus_cfg->sample_rate);
    return bus;
}

esp_err_t audio_bus_destroy(audio_bus_handle_t bus)
{
    if (!audio_bus_valid(bus) )
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (bus->dir == AUDIO_BUS_RX)
    {
        /* 释放仍残留的 reader（调用方应先注销，这里兜底防泄漏） */
        xSemaphoreTake(bus->reader_mutex, portMAX_DELAY);
        for (size_t i = 0; i < bus->reader_count; i++)
        {
            ESP_LOGW(TAG, "reader[%s] was not unregistered, releasing",
                     bus->readers[i]->name);
            vStreamBufferDelete(bus->readers[i]->fifo);
            free(bus->readers[i]);
        }
        free(bus->readers);
        bus->readers = NULL;
        bus->reader_count = 0;
        xSemaphoreGive(bus->reader_mutex);

        /* 通知 RX 任务退出并等待其自删 */
        bus->destroying = true;
        while (bus->rx_task != NULL)
        {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        vSemaphoreDelete(bus->reader_mutex);
    }
    else
    {
        vSemaphoreDelete(bus->write_mutex);
    }

    i2s_bus_phy_destroy(bus->phy);
    bus->magic = 0;
    free(bus);
    ESP_LOGI(TAG, "bus destroyed");
    return ESP_OK;
}

i2s_bus_handle_t audio_bus_get_phy(audio_bus_handle_t bus)
{
    if (!audio_bus_valid(bus))
    {
        return NULL;
    }
    return bus->phy;
}

/* ======================== TX：多生产者接口 ========================================= */

esp_err_t audio_writer_register(audio_bus_handle_t bus, const char *name,
                                audio_writer_handle_t *out)
{
    if (!audio_bus_valid(bus) || name == NULL || out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (bus->dir != AUDIO_BUS_TX)
    {
        ESP_LOGE(TAG, "writer can only register on TX bus");
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
    /* 发送函数必须收到发送句柄，方向错误一律拒绝 */
    if (!audio_writer_valid(writer) || pcm == NULL || samples == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    audio_bus_handle_t bus = writer->bus;
    if (!audio_bus_valid(bus) || bus->dir != AUDIO_BUS_TX)
    {
        ESP_LOGE(TAG, "writer[%s] not on TX bus", writer->name);
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

    uint8_t bytes_per_sample = audio_bus_bytes_per_sample(&bus->bus_cfg);
    size_t bytes = samples * bytes_per_sample;

    size_t bytes_written = 0;
    /* i2s_bus_phy_write() forwards timeout to i2s_channel_write() as ms */
    esp_err_t ret = i2s_bus_phy_write(bus->phy, (const uint8_t *)pcm, bytes,
                                      &bytes_written, timeout_ms);
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

/* ======================== RX：多消费者接口 ========================================= */

esp_err_t audio_reader_register(audio_bus_handle_t bus, const char *name,
                                size_t fifo_bytes, audio_fifo_overflow_t overflow,
                                audio_reader_handle_t *out)
{
    if (!audio_bus_valid(bus) || name == NULL || out == NULL || fifo_bytes == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (bus->dir != AUDIO_BUS_RX)
    {
        ESP_LOGE(TAG, "reader can only register on RX bus");
        return ESP_ERR_INVALID_ARG;
    }

    audio_reader_handle_t reader = calloc(1, sizeof(struct audio_reader_s));
    if (reader == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    reader->magic = AUDIO_READER_MAGIC;
    reader->bus = bus;
    reader->overflow = overflow;
    strncpy(reader->name, name, AUDIO_BUS_NAME_MAX - 1);
    reader->name[AUDIO_BUS_NAME_MAX - 1] = '\0';

    reader->fifo = xStreamBufferCreate(fifo_bytes, 0);
    if (reader->fifo == NULL)
    {
        free(reader);
        return ESP_ERR_NO_MEM;
    }

    /* 加入 reader 列表（RX 任务可能正在并发广播） */
    xSemaphoreTake(bus->reader_mutex, portMAX_DELAY);
    struct audio_reader_s **new_list = realloc(bus->readers,
                                               (bus->reader_count + 1) * sizeof(void *));
    if (new_list == NULL)
    {
        xSemaphoreGive(bus->reader_mutex);
        vStreamBufferDelete(reader->fifo);
        free(reader);
        return ESP_ERR_NO_MEM;
    }
    bus->readers = new_list;
    bus->readers[bus->reader_count] = reader;
    bus->reader_count++;
    xSemaphoreGive(bus->reader_mutex);

    ESP_LOGI(TAG, "reader[%s] registered: fifo=%d bytes, overflow=%s",
             reader->name, (int)fifo_bytes,
             (overflow == AUDIO_FIFO_DROP_OLDEST) ? "DROP_OLDEST" : "DROP_NEW");
    *out = reader;
    return ESP_OK;
}

esp_err_t audio_reader_read_raw(audio_reader_handle_t reader, uint8_t *buffer,
                                size_t size, size_t *bytes_read, uint32_t timeout_ms)
{
    /* 接收函数必须校验其句柄是接收句柄 */
    if (!audio_reader_valid(reader) || buffer == NULL || size == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    audio_bus_handle_t bus = reader->bus;
    if (!audio_bus_valid(bus) || bus->dir != AUDIO_BUS_RX)
    {
        ESP_LOGE(TAG, "reader[%s] not on RX bus", reader->name);
        return ESP_ERR_INVALID_ARG;
    }

    TickType_t wait_ticks = (timeout_ms == UINT32_MAX)
        ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);

    size_t received = xStreamBufferReceive(reader->fifo, buffer, size, wait_ticks);
    if (bytes_read != NULL)
    {
        *bytes_read = received;
    }
    return (received > 0) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t audio_reader_read_pcm16(audio_reader_handle_t reader, int16_t *pcm,
                                  size_t samples, size_t *samples_read,
                                  uint32_t timeout_ms)
{
    if (!audio_reader_valid(reader) || pcm == NULL || samples == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    audio_bus_handle_t bus = reader->bus;
    if (!audio_bus_valid(bus) || bus->dir != AUDIO_BUS_RX)
    {
        ESP_LOGE(TAG, "reader[%s] not on RX bus", reader->name);
        return ESP_ERR_INVALID_ARG;
    }

    /* 计算截止时刻，保证总超时不超过 timeout_ms */
    TickType_t deadline;
    bool unlimited = (timeout_ms == UINT32_MAX);
    if (unlimited)
    {
        deadline = portMAX_DELAY;
    }
    else
    {
        deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    }

    size_t total = 0;
    while (total < samples)
    {
        size_t need = samples - total;
        size_t batch = (need > AUDIO_BUS_RX_CHUNK_SAMPLES)
            ? AUDIO_BUS_RX_CHUNK_SAMPLES : need;

        int32_t raw[AUDIO_BUS_RX_CHUNK_SAMPLES];
        {
            TickType_t now = xTaskGetTickCount();
            TickType_t wait = unlimited ? portMAX_DELAY : (deadline - now);
            if (!unlimited && (int32_t)(deadline - now) <= 0)
            {
                break; /* 总超时已到 */
            }
            size_t got_bytes = xStreamBufferReceive(reader->fifo, raw,
                                                    batch * sizeof(int32_t), wait);
            size_t got = got_bytes / sizeof(int32_t);
            for (size_t i = 0; i < got; i++)
            {
                pcm[total + i] = (int16_t)(raw[i] >> 16);
            }
            total += got;
            if (got < batch)
            {
                break; /* 超时或数据不足，返回已读部分 */
            }
        }
    }

    if (samples_read != NULL)
    {
        *samples_read = total;
    }
    return (total > 0) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t audio_reader_flush(audio_reader_handle_t reader)
{
    if (!audio_reader_valid(reader))
    {
        return ESP_ERR_INVALID_ARG;
    }
    /* 循环收空，绝不使用 xStreamBufferReset（避免阻塞等待者） */
    uint8_t tmp[256];
    size_t drained = 0;
    while (xStreamBufferReceive(reader->fifo, tmp, sizeof(tmp), 0) > 0)
    {
        drained++;
    }
    if (drained > 0)
    {
        ESP_LOGI(TAG, "reader[%s] flushed %d blocks", reader->name, (int)drained);
    }
    return ESP_OK;
}

esp_err_t audio_reader_unregister(audio_reader_handle_t reader)
{
    if (!audio_reader_valid(reader))
    {
        return ESP_ERR_INVALID_ARG;
    }
    audio_bus_handle_t bus = reader->bus;
    if (!audio_bus_valid(bus))
    {
        return ESP_ERR_INVALID_ARG;
    }

    /* 从列表中移除（最后一个成员搬过来补齐） */
    xSemaphoreTake(bus->reader_mutex, portMAX_DELAY);
    for (size_t i = 0; i < bus->reader_count; i++)
    {
        if (bus->readers[i] == reader)
        {
            bus->readers[i] = bus->readers[bus->reader_count - 1];
            bus->reader_count--;
            break;
        }
    }
    xSemaphoreGive(bus->reader_mutex);

    ESP_LOGI(TAG, "reader[%s] unregistered (dropped %d bytes total)",
             reader->name, (int)reader->dropped_bytes);
    vStreamBufferDelete(reader->fifo);
    reader->magic = 0;
    reader->bus = NULL;
    free(reader);
    return ESP_OK;
}
