/**
 * @file amplifier.c
 * @brief 功放模块实现（NS4168 D 类功放，直接驱动 I2S TX）
 *
 * 本模块封装 NS4168 功放芯片的音频输出功能。NS4168 是 5W 单声道
 * D 类功放，通过 I2S 标准协议接收 PCM 音频数据驱动扬声器。
 *
 * 架构说明（原 audio_bus 瘦身后）：
 *   - 本模块直接持有 i2s_driver 的物理 TX 句柄（I2S_NUM_1）；
 *   - 模块内部维护写锁，每块 Amplifier_Play_Buffer() 为一次原子写，
 *     多任务音源（组播同步播放、自检音等）经此入口串行化；
 *   - 从节点只有功放一个音频输出端，无麦克风/蓝牙/本地解码。
 *
 * 硬件连接（I2S TX）：BCLK=GPIO8、LRCK=GPIO9、DIN=GPIO10
 */

#include "amplifier.h"
#include "i2s_driver.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stddef.h>
#include <string.h>

/* ======================== 模块静态变量 ============================================= */

static const char *TAG = "AMPLIFIER";  /* 日志标签 */

/* I2S 物理 TX 句柄与写锁 */
static i2s_bus_handle_t  Amp_Phy        = NULL;
static SemaphoreHandle_t Amp_WriteMutex = NULL;

/* 当前软件音量（0~100），默认 80 */
static uint8_t Amplifier_Volume = 80;

/* 软件音量衰减工作缓冲（PSRAM；NS4168 无硬件增益，只能衰减 PCM）。
 * 单帧最大 662 采样 × 1ch × 2B = 1324B，8192B 留足余量。 */
static int16_t *s_amp_scaled = NULL;
#define AMP_SCALED_CAP_BYTES  8192

/* ======================== 硬编码引脚与格式配置 ===================================== */

/** I2S TX 位时钟 GPIO（BCLK 接 NS4168 BCLK） */
#define AMP_BCLK_GPIO  8

/** I2S TX 左右声道时钟 GPIO（接 NS4168 LRCK） */
#define AMP_WS_GPIO    9

/** I2S TX 数据输出 GPIO（接 NS4168 DIN） */
#define AMP_DOUT_GPIO  10

/** 功放物理引脚配置 */
static const i2s_pin_cfg_t Amp_PinCfg = {
    .mclk      = I2S_GPIO_UNUSED,   /* NS4168 不需要 MCLK */
    .bclk      = AMP_BCLK_GPIO,
    .ws        = AMP_WS_GPIO,
    .dout      = AMP_DOUT_GPIO,
    .din       = I2S_GPIO_UNUSED,
    .ws_pol    = false,
    .bit_shift = false,
};

/** 功放音频格式：44.1kHz / 16-bit / 单声道 */
static const i2s_bus_cfg_t Amp_BusCfg = {
    .sample_rate    = AMPLIFIER_SAMPLE_RATE,
    .bit_width      = I2S_DATA_BIT_WIDTH_16BIT,
    .slot_mode      = I2S_SLOT_MODE_MONO,
    .slot_mask      = I2S_STD_SLOT_LEFT | I2S_STD_SLOT_RIGHT,
    .slot_ws_pol    = false,
    .slot_bit_shift = true,    /* Philips 标准默认行为（保持与原驱动一致） */
    .dma_desc_num   = 8,
    .dma_frame_num  = 256,
    .tx_auto_clear  = true,
};

/* ======================== 功放公共 API ========================================== */

esp_err_t Amplifier_Init(void)
{
    /* 重复初始化保护 */
    if (Amp_Phy != NULL)
    {
        ESP_LOGW(TAG, "already initialized");
        return ESP_OK;
    }

    /* 创建 I2S TX 物理通道（I2S_NUM_1，功放） */
    esp_err_t ret = i2s_bus_phy_create(I2S_NUM_1, true, &Amp_PinCfg, &Amp_BusCfg, &Amp_Phy);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "i2s phy create failed: %s", esp_err_to_name(ret));
        Amp_Phy = NULL;
        return ESP_FAIL;
    }

    Amp_WriteMutex = xSemaphoreCreateMutex();
    if (Amp_WriteMutex == NULL)
    {
        ESP_LOGE(TAG, "write mutex create failed");
        i2s_bus_phy_destroy(Amp_Phy);
        Amp_Phy = NULL;
        return ESP_FAIL;
    }

    /* 音量衰减工作缓冲：优先 PSRAM（内部 RAM 紧张） */
    if (s_amp_scaled == NULL)
    {
        s_amp_scaled = heap_caps_malloc(AMP_SCALED_CAP_BYTES,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_amp_scaled == NULL)
        {
            s_amp_scaled = heap_caps_malloc(AMP_SCALED_CAP_BYTES, MALLOC_CAP_8BIT);
        }
        if (s_amp_scaled == NULL)
        {
            ESP_LOGW(TAG, "volume buffer alloc failed, volume will not take effect");
        }
    }

    ESP_LOGI(TAG, "NS4168 amplifier initialized, volume=%d%%", Amplifier_Volume);
    return ESP_OK;
}

esp_err_t Amplifier_Deinit(void)
{
    if (Amp_WriteMutex != NULL)
    {
        vSemaphoreDelete(Amp_WriteMutex);
        Amp_WriteMutex = NULL;
    }
    if (Amp_Phy != NULL)
    {
        i2s_bus_phy_destroy(Amp_Phy);
        Amp_Phy = NULL;
    }
    if (s_amp_scaled != NULL)
    {
        heap_caps_free(s_amp_scaled);
        s_amp_scaled = NULL;
    }
    ESP_LOGI(TAG, "NS4168 amplifier deinitialized");
    return ESP_OK;
}

esp_err_t Amplifier_Play_Buffer(const uint8_t *buffer, size_t size, size_t *bytes_written, uint32_t timeout)
{
    /* 参数校验 */
    if (buffer == NULL || size == 0)
    {
        ESP_LOGE(TAG, "Invalid buffer parameters");
        return ESP_ERR_INVALID_ARG;
    }
    if (Amp_Phy == NULL || Amp_WriteMutex == NULL)
    {
        ESP_LOGE(TAG, "amplifier not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * 写锁要连"软件音量缩放"一起罩住：s_amp_scaled 是模块级共享工作缓冲，
     * 两个音源并发进来会互相踩踏。缩放本身只是定点乘加，微秒级，不违反
     * "锁内不做重活"的约定。
     */
    TickType_t wait_ticks = (timeout == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout);
    if (xSemaphoreTake(Amp_WriteMutex, wait_ticks) != pdTRUE)
    {
        ESP_LOGW(TAG, "write lock timeout");
        return ESP_ERR_TIMEOUT;
    }

    /*
     * 软件音量：NS4168 是固定增益 D 类功放，没有硬件音量寄存器，
     * 音量只能通过衰减 PCM 实现。平方曲线（更接近人耳等响感知）：
     * gain = (vol/100)^2。音量 100 原样直通（零开销），0 输出静音。
     */
    size_t samples = size / (sizeof(int16_t) * AMPLIFIER_CHANNEL_NUM);
    const int16_t *out = (const int16_t *)buffer;

    if (Amplifier_Volume < AMPLIFIER_VOLUME_MAX)
    {
        uint16_t vol = Amplifier_Volume;
        if (s_amp_scaled != NULL && vol == 0)
        {
            memset(s_amp_scaled, 0, size);   /* 静音 */
            out = s_amp_scaled;
        }
        else if (s_amp_scaled != NULL &&
                 samples <= (AMP_SCALED_CAP_BYTES / (sizeof(int16_t) * AMPLIFIER_CHANNEL_NUM)))
        {
            /* 定点增益：(vol/100)^2 * 32768，右移 15 位保证不溢出 */
            uint32_t gain = ((uint32_t)vol * vol * 32768) / 10000u;
            const int16_t *in = (const int16_t *)buffer;

            for (size_t i = 0; i < samples * AMPLIFIER_CHANNEL_NUM; i++)
            {
                s_amp_scaled[i] = (int16_t)(((int32_t)in[i] * (int32_t)gain) >> 15);
            }
            out = s_amp_scaled;
        }
    }

    /* 物理写入：PCM 16-bit 单声道，size 直接为字节数 */
    size_t written = 0;
    esp_err_t ret = i2s_bus_phy_write(Amp_Phy, (const uint8_t *)out, size, &written, timeout);
    xSemaphoreGive(Amp_WriteMutex);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Play buffer failed: %s", esp_err_to_name(ret));
        return ret;
    }
    if (bytes_written != NULL)
    {
        *bytes_written = size;
    }
    return ESP_OK;
}

esp_err_t Amplifier_Set_Volume(uint8_t volume)
{
    if (volume > AMPLIFIER_VOLUME_MAX)
    {
        volume = AMPLIFIER_VOLUME_MAX;
    }
    Amplifier_Volume = volume;
    ESP_LOGI(TAG, "Volume set to %d%%", Amplifier_Volume);
    return ESP_OK;
}

uint8_t Amplifier_Get_Volume(void)
{
    return Amplifier_Volume;
}

esp_err_t Amplifier_Set_Sample_Rate(uint32_t sample_rate)
{
    if (sample_rate == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (Amp_Phy == NULL || Amp_WriteMutex == NULL)
    {
        ESP_LOGE(TAG, "amplifier not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * 必须与 Amplifier_Play_Buffer() 共用同一把写锁。
     * i2s_bus_phy_set_sample_rate() 内部是 disable → reconfig → enable，
     * 会把 TX 队列整个复位（DMA 里未播的音频全部丢弃），与并发写天然冲突；
     * i2s_driver 层零锁，互斥只能由本模块提供。
     * 这里用 portMAX_DELAY：宁可多等一会儿，也不能在重配时插进去写。
     */
    if (xSemaphoreTake(Amp_WriteMutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t ret = i2s_bus_phy_set_sample_rate(Amp_Phy, sample_rate);
    xSemaphoreGive(Amp_WriteMutex);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "set sample rate %lu failed: %s",
                 (unsigned long)sample_rate, esp_err_to_name(ret));
    }
    return ret;
}
