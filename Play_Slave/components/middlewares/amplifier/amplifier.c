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
 *   - 功放是一条 TX 总线，未来组播同步、测试音等上层 App 都可以注册
 *     写者，由 audio_bus 仲裁，保证同一时刻只有一路数据在播放；
 *   - 本模块只负责"发声"，不关心总线上其他写者的存在。
 *
 * 硬件连接（I2S TX）：BCLK=GPIO8、LRCK=GPIO9、DIN=GPIO10
 */

#include "amplifier.h"
#include "audio_bus.h"
#include "esp_log.h"

/* ======================== 模块静态变量 =========================================== */

static const char *TAG = "AMPLIFIER";  /* 日志标签 */

/* 功放所属音频总线与写者句柄 */
static audio_bus_handle_t Amp_Bus = NULL;
static audio_writer_handle_t Amp_Writer = NULL;

/* ======================== 硬编码引脚与格式配置 ==================================== */

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

/** 功放音频格式：16kHz / 16-bit / 单声道 */
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
     * 从节点总线固定为 TX（仅功放），其他模块后续可以在同一条
     * 总线上注册写者。
     */
    Amp_Bus = audio_bus_create(I2S_NUM_1, &Amp_PinCfg, &Amp_BusCfg);
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

    ESP_LOGI(TAG, "NS4168 amplifier initialized");
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
     * 通过音频总线写入（原子块，默认 BLOCK 策略）：
     *  PCM 为 16-bit 单声道，采样数 = 字节数 / 2
     */
    size_t samples = size / sizeof(int16_t);
    esp_err_t ret = audio_writer_write(Amp_Writer, (const int16_t *)buffer,
                                       samples, timeout, AUDIO_WRITE_BLOCK);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Play buffer failed: %s", esp_err_to_name(ret));
        return ret;
    }
    if (bytes_written != NULL)
    {
        *bytes_written = samples * sizeof(int16_t);
    }
    return ESP_OK;
}
