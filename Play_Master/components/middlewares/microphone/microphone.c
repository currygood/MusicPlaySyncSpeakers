/**
 * @file microphone.c
 * @brief 麦克风模块实现（INMP441 + 内部读取任务 + 单一 FIFO）
 *
 * 本模块封装 INMP441 MEMS 麦克风的 I2S 音频采集功能，并直接承担
 * 原 audio_bus RX 侧的职责（读取任务 + FIFO）：
 *   - Microphone_Init() 创建 I2S RX 物理通道并启动内部读取任务，
 *     持续把麦克风数据写入模块私有 FIFO（满时丢最旧）；
 *   - 消费者（当前为 CallPhone）通过 Microphone_Read_* 阻塞读取。
 *
 * 硬件连接（I2S RX）：
 *   - BCLK=GPIO2（连 INMP441 SCK）
 *   - LRCK=GPIO5（连 INMP441 WS）
 *   - DOUT=GPIO34（连 INMP441 SD）
 *
 * INMP441 关键电气特性：
 *   - 输出 24-bit 数据，I2S 以 32-bit 帧对齐（高 24 位有效）
 *   - 首次上电需要 2^18 SCK 周期（约 256ms）的稳定时间
 */

#include "microphone.h"
#include "i2s_driver.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define TAG "MICROPHONE"  /* 日志标签 */

/* ======================== 硬件引脚与格式配置 ===================================== */

/** INMP441 SD（DOUT）数据引脚 */
#define INMP441_SD_GPIO    34

/** FIFO 大小：16KB ≈ 93ms @44.1kHz×4 字节 */
#define MICROPHONE_FIFO_BYTES  (16 * 1024)

/** 读取任务每次从物理层搬运的采样数（32-bit 采样） */
#define MIC_RX_CHUNK_SAMPLES   512

/** 读取任务物理读取的阻塞上限（用于及时响应销毁请求，单位 ms） */
#define MIC_RX_READ_TIMEOUT_MS 100

/** 麦克风物理引脚配置 */
static const i2s_pin_cfg_t Mic_PinCfg = {
    .mclk      = I2S_GPIO_UNUSED,   /* INMP441 不需要 MCLK */
    .bclk      = 2,                 /* 接 SCK */
    .ws        = 5,                 /* 接 LRCK/WS */
    .dout      = I2S_GPIO_UNUSED,
    .din       = INMP441_SD_GPIO,   /* 接 SD */
    .ws_pol    = false,
    .bit_shift = false,
};

/** 麦克风音频格式：44.1kHz / 32-bit / 立体声槽位（仅收 RIGHT） */
static const i2s_bus_cfg_t Mic_BusCfg = {
    .sample_rate    = MICROPHONE_SAMPLE_RATE,
    .bit_width      = I2S_DATA_BIT_WIDTH_32BIT,
    .slot_mode      = I2S_SLOT_MODE_STEREO,
    .slot_mask      = I2S_STD_SLOT_RIGHT,
    .slot_ws_pol    = true,     /* INMP441 时序需要反转 LRCK */
    .slot_bit_shift = false,
    .dma_desc_num   = 8,
    .dma_frame_num  = 256,
    .tx_auto_clear  = false,
};

/* ======================== 模块静态变量 ============================================== */

/** I2S RX 物理句柄 / FIFO / 读取任务 */
static i2s_bus_handle_t      Mic_Phy    = NULL;
static StreamBufferHandle_t Mic_Fifo   = NULL;
static TaskHandle_t         Mic_RxTask = NULL;
static volatile bool        Mic_RxStop = false;

/** 读取任务采集缓冲（2KB） */
static uint8_t Mic_RxChunk[MIC_RX_CHUNK_SAMPLES * 4];

/* ======================== INMP 硬件控制 ============================================= */

/**
 * @brief 配置 SD（DOUT）数据引脚内部下拉
 *
 * 当麦克风不驱动数据时（例如在另一声道的时隙中），SD 引脚处于三态，
 * 可能悬空并产生随机噪声，这里用 ESP32 内部约 45kΩ 下拉兜底
 * （推荐仍外加 100kΩ 外部下拉）。
 */
static void Inmp441_Sd_Pulldown_Enable(void)
{
    /* GPIO34 为输入专用脚，不支持上下拉时返回错误（需外部下拉） */
    esp_err_t err = gpio_set_pull_mode(INMP441_SD_GPIO, GPIO_PULLDOWN_ONLY);
    ESP_LOGW(TAG, "SD pin GPIO%d pull-down: %s", INMP441_SD_GPIO, esp_err_to_name(err));
}

/**
 * @brief 使能 INMP441 硬件
 *
 * 当前 INMP441_CHIPEN_GPIO = -1（硬接 VDD），SCK/WS 已启动，
 * 等待 100ms 让传感器从待机恢复。
 */
static void Inmp441_Enable(void)
{
#if (INMP441_CHIPEN_GPIO >= 0)
    ESP_LOGI(TAG, "Enabling INMP441 via GPIO%d...", INMP441_CHIPEN_GPIO);
    gpio_config_t chipen_cfg = {
        .pin_bit_mask = (1ULL << INMP441_CHIPEN_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&chipen_cfg);
    gpio_set_level(INMP441_CHIPEN_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(300));   /* 首通：2^18 SCK ≈ 256ms，留余量 */
#else
    ESP_LOGI(TAG, "INMP441 CHIPEN=VDD, waiting standby recovery...");
    vTaskDelay(pdMS_TO_TICKS(100));
#endif
}

/* ======================== FIFO 推送与读取任务 ====================================== */

/** 向 FIFO 推一块数据；空间不足时丢最旧（DROP_OLDEST） */
static void Mic_Fifo_Push(const uint8_t *data, size_t len)
{
    size_t spaces = xStreamBufferSpacesAvailable(Mic_Fifo);
    if (spaces >= len)
    {
        xStreamBufferSend(Mic_Fifo, data, len, 0);
        return;
    }

    size_t need_drop = len - spaces;
    uint8_t tmp[128];
    while (need_drop > 0)
    {
        size_t drop_now = (need_drop > sizeof(tmp)) ? sizeof(tmp) : need_drop;
        size_t got = xStreamBufferReceive(Mic_Fifo, tmp, drop_now, 0);
        if (got == 0)
        {
            break; /* 理论不会发生 */
        }
        need_drop -= got;
    }
    xStreamBufferSend(Mic_Fifo, data, len, 0);
}

/** 读取任务：持续读物理 I2S，写入 FIFO */
static void Mic_Rx_Task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "RX task started");

    while (!Mic_RxStop)
    {
        size_t bytes_read = 0;
        esp_err_t ret = i2s_bus_phy_read(Mic_Phy, Mic_RxChunk,
                                         sizeof(Mic_RxChunk), &bytes_read,
                                         MIC_RX_READ_TIMEOUT_MS);
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
        Mic_Fifo_Push(Mic_RxChunk, bytes_read);
    }

    Mic_RxTask = NULL;
    ESP_LOGI(TAG, "RX task exit");
    vTaskDelete(NULL);
}

/* ======================== 麦克风公共 API ========================================== */

esp_err_t Microphone_Init(void)
{
    if (Mic_Phy != NULL)
    {
        ESP_LOGW(TAG, "already initialized");
        return ESP_OK;
    }

    /* 1) I2S 接管前先设置 SD 引脚下拉 */
    Inmp441_Sd_Pulldown_Enable();

    /* 2) 创建 I2S RX（启动 SCK/WS 时钟，INMP441 初始化的前提） */
    esp_err_t ret = i2s_bus_phy_create(I2S_NUM_0, false, &Mic_PinCfg, &Mic_BusCfg, &Mic_Phy);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "i2s phy create failed: %s", esp_err_to_name(ret));
        Mic_Phy = NULL;
        return ESP_FAIL;
    }

    /* 3. I2S 初始化可能重置 GPIO，重新设置下拉；再使能 INMP441 */
    Inmp441_Sd_Pulldown_Enable();
    Inmp441_Enable();

    /* 4. 创建 FIFO 并启动读取任务 */
    Mic_Fifo = xStreamBufferCreate(MICROPHONE_FIFO_BYTES, 1);
    if (Mic_Fifo == NULL)
    {
        ESP_LOGE(TAG, "fifo create failed");
        i2s_bus_phy_destroy(Mic_Phy);
        Mic_Phy = NULL;
        return ESP_FAIL;
    }

    Mic_RxStop = false;
    BaseType_t created = xTaskCreate(Mic_Rx_Task, "MIC-RX", 2048, NULL, 10, &Mic_RxTask);
    if (created != pdPASS)
    {
        ESP_LOGE(TAG, "rx task create failed");
        vStreamBufferDelete(Mic_Fifo);
        Mic_Fifo = NULL;
        i2s_bus_phy_destroy(Mic_Phy);
        Mic_Phy = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "INMP441 microphone initialized, sample_rate=%d", MICROPHONE_SAMPLE_RATE);
    return ESP_OK;
}

esp_err_t Microphone_Deinit(void)
{
    if (Mic_RxTask != NULL)
    {
        Mic_RxStop = true;
        while (Mic_RxTask != NULL)
        {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    if (Mic_Fifo != NULL)
    {
        vStreamBufferDelete(Mic_Fifo);
        Mic_Fifo = NULL;
    }
    if (Mic_Phy != NULL)
    {
        i2s_bus_phy_destroy(Mic_Phy);
        Mic_Phy = NULL;
    }
    ESP_LOGI(TAG, "INMP441 microphone deinitialized");
    return ESP_OK;
}

/** 旧接口兼容：timeout 单位 FreeRTOS ticks，UINT32_MAX 无限等待 */
esp_err_t Microphone_Read_Raw(uint8_t *buffer, size_t size, size_t *bytes_read, uint32_t timeout)
{
    if (buffer == NULL || size == 0)
    {
        ESP_LOGE(TAG, "Invalid buffer parameters");
        return ESP_ERR_INVALID_ARG;
    }
    if (Mic_Fifo == NULL)
    {
        ESP_LOGE(TAG, "microphone not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    TickType_t wait = (timeout == UINT32_MAX) ? portMAX_DELAY : timeout;
    size_t got = xStreamBufferReceive(Mic_Fifo, buffer, size, wait);
    if (bytes_read != NULL)
    {
        *bytes_read = got;
    }
    return (got > 0) ? ESP_OK : ESP_ERR_TIMEOUT;
}

/** 32-bit 采样取高 16 位输出 */
esp_err_t Microphone_Read_Pcm16(int16_t *pcmBuffer, size_t sampleCount, size_t *samplesRead, uint32_t timeout)
{
    if (pcmBuffer == NULL || sampleCount == 0)
    {
        ESP_LOGE(TAG, "Invalid PCM buffer parameters");
        return ESP_ERR_INVALID_ARG;
    }
    if (Mic_Fifo == NULL)
    {
        ESP_LOGE(TAG, "microphone not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    TickType_t deadline;
    bool unlimited = (timeout == UINT32_MAX);
    if (unlimited)
    {
        deadline = portMAX_DELAY;
    }
    else
    {
        deadline = xTaskGetTickCount() + timeout;
    }

    size_t total = 0;
    while (total < sampleCount)
    {
        size_t need = sampleCount - total;
        size_t batch = (need > MIC_RX_CHUNK_SAMPLES) ? MIC_RX_CHUNK_SAMPLES : need;

        int32_t raw[MIC_RX_CHUNK_SAMPLES];
        TickType_t now = xTaskGetTickCount();
        TickType_t wait = unlimited ? portMAX_DELAY : (TickType_t)(deadline - now);
        if (!unlimited && (int32_t)(deadline - now) <= 0)
        {
            break; /* 总超时已到 */
        }
        size_t got_bytes = xStreamBufferReceive(Mic_Fifo, raw, batch * sizeof(int32_t), wait);
        size_t got = got_bytes / sizeof(int32_t);
        for (size_t i = 0; i < got; i++)
        {
            pcmBuffer[total + i] = (int16_t)(raw[i] >> 16);
        }
        total += got;
        if (got < batch)
        {
            break; /* 超时或数据不足 */
        }
    }

    if (samplesRead != NULL)
    {
        *samplesRead = total;
    }
    return (total > 0) ? ESP_OK : ESP_ERR_TIMEOUT;
}

/** 清空 FIFO（DROP_OLDEST 语义下的旧数据不保留） */
esp_err_t Microphone_Flush(void)
{
    if (Mic_Fifo == NULL)
    {
        ESP_LOGE(TAG, "microphone not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t tmp[256];
    while (xStreamBufferReceive(Mic_Fifo, tmp, sizeof(tmp), 0) > 0)
    {
    }
    return ESP_OK;
}
