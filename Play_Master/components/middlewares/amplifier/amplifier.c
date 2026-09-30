/**
 * @file amplifier.c
 * @brief 功放模块实现（基于 NS4168 D 类功放芯片 + audio_bus 总线）
 *
 * 本模块封装 NS4168 功放芯片的音频输出功能。NS4168 是 5W 单声道
 * D 类功放，通过 I2S 标准协议接收 PCM 音频数据驱动扬声器。
 *
 * 架构变化（总线化改造）：
 *   - 不再直接调用 i2s_driver 的旧 API，而是通过 middlewares/audio_bus
 *     注册为一个"写者"（生产者）；
 *   - 功放是一条 TX 总线，未来蓝牙、组播同步等上层 App 都可以注册
 *     写者，由 audio_bus 仲裁，保证同一时刻只有一路数据在播放；
 *   - 本模块只负责"发声"，不关心总线上其他写者的存在。
 *
 * 硬件连接（I2S TX）：BCLK=GPIO26、LRCK=GPIO27、DIN=GPIO13
 */

#include "amplifier.h"
#include "audio_bus.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "i2s_driver.h"

/* ======================== 模块静态变量 =========================================== */

static const char *TAG = "AMPLIFIER";  /* 日志标签 */

/* 当前音量值（0~100），默认 80% */
static uint8_t Amplifier_Volume = 80;

/* 功放所属音频总线与写者句柄 */
static audio_bus_handle_t Amp_Bus = NULL;
static audio_writer_handle_t Amp_Writer = NULL;

/* 软件音量衰减工作缓冲（PSRAM；NS4168 无硬件增益，只能衰减 PCM） */
static int16_t *s_amp_scaled = NULL;
#define AMP_SCALED_CAP_BYTES  8192   /* 最大单块 PCM：2048 帧 × 2ch × 2B */

/* ======================== 硬编码引脚与格式配置 ==================================== */

/** I2S TX 位时钟 GPIO（BCLK 接 NS4168 BCLK） */
#define AMP_BCLK_GPIO  26

/** I2S TX 左右声道时钟 GPIO（接 NS4168 LRCK） */
#define AMP_WS_GPIO    27

/** I2S TX 数据输出 GPIO（接 NS4168 DIN） */
#define AMP_DOUT_GPIO  13

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

/** 功放音频格式：44.1kHz / 16-bit / 双声道（L/R 槽写同一份，NS4168 单声道） */
static const i2s_bus_cfg_t Amp_BusCfg = {
    .sample_rate    = AMPLIFIER_SAMPLE_RATE,
    .bit_width      = I2S_DATA_BIT_WIDTH_16BIT,
    /* STEREO: duplicate the same frame into both L/R slots */
    .slot_mode      = I2S_SLOT_MODE_STEREO,
    .slot_mask      = I2S_STD_SLOT_LEFT | I2S_STD_SLOT_RIGHT,
    .slot_ws_pol    = false,
    .slot_bit_shift = true,    /* Philips 标准默认行为（保持与原驱动一致） */
    .dma_desc_num   = 16,       /* 增大 DMA 描述符数量 */
    .dma_frame_num  = 512,      /* 增大每帧采样数 */
    .tx_auto_clear  = true,
};

/* ======================== 功放公共 API =========================================== */

esp_err_t Amplifier_Init(void)
{
    /* 重复初始化保护 */
    if (Amp_Bus != NULL)
    {
        ESP_LOGW(TAG, "already initialized");
        return ESP_OK;
    }

    /*
     * 创建 TX 音频总线（I2S_NUM_1，功放）。
     * 功放只负责输出，其他模块后续可以在同一条总线上注册写者。
     */
    Amp_Bus = audio_bus_create(AUDIO_BUS_TX, I2S_NUM_1, &Amp_PinCfg, &Amp_BusCfg);
    if (Amp_Bus == NULL)
    {
        ESP_LOGE(TAG, "audio bus create failed");
        return ESP_FAIL;
    }

    /* 以"amplifier"之名注册为写者 */
    esp_err_t ret = audio_writer_register(Amp_Bus, "amplifier", &Amp_Writer);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "writer register failed: %s", esp_err_to_name(ret));
        audio_bus_destroy(Amp_Bus);
        Amp_Bus = NULL;
        return ret;
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
    if (Amp_Writer != NULL)
    {
        audio_writer_unregister(Amp_Writer);
        Amp_Writer = NULL;
    }
    if (Amp_Bus != NULL)
    {
        audio_bus_destroy(Amp_Bus);
        Amp_Bus = NULL;
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
    if (Amp_Writer == NULL)
    {
        ESP_LOGE(TAG, "amplifier not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * 软件音量：NS4168 是固定增益 D 类功放，音量只能通过衰减 PCM 实现。
     * 采用平方曲线（更接近人耳等响感知）：gain = (vol/100)^2。
     * 音量 100 时原样直通（零开销），0 时输出静音。
     */
    size_t samples = size / (sizeof(int16_t) * AMPLIFIER_CHANNEL_NUM);
    const int16_t *out = (const int16_t *)buffer;

    if (Amplifier_Volume < AMPLIFIER_VOLUME_MAX)
    {
        uint16_t vol = (Amplifier_Volume > 0) ? Amplifier_Volume : 0;
        if (s_amp_scaled != NULL && vol > 0 && samples <=
            (AMP_SCALED_CAP_BYTES / (sizeof(int16_t) * AMPLIFIER_CHANNEL_NUM)))
        {
            /* 定点增益：(vol/100)^2 * 32768，右移 15 位保证不溢出 */
            uint32_t gain = ((uint32_t)vol * vol * 32768) / 10000u;
            const int16_t *in = (const int16_t *)buffer;
            size_t total = samples * AMPLIFIER_CHANNEL_NUM;

            for (size_t i = 0; i < total; i++)
            {
                s_amp_scaled[i] = (int16_t)(((int32_t)in[i] * (int32_t)gain) >> 15);
            }
            out = s_amp_scaled;
        }
        else if (s_amp_scaled != NULL && vol == 0)
        {
            memset(s_amp_scaled, 0, size);   /* 静音 */
            out = s_amp_scaled;
        }
    }

    esp_err_t ret = audio_writer_write(Amp_Writer, out, samples, timeout, AUDIO_WRITE_BLOCK);
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

void Amplifier_Debug_DMA(void)
{
    i2s_bus_handle_t phy = audio_bus_get_phy(Amp_Bus);
    if (phy != NULL)
    {
        i2s_bus_phy_debug_dma(phy);
    }
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
