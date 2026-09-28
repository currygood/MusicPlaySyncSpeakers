/**
 * @file microphone.c
 * @brief 麦克风模块实现（基于 INMP441 MEMS 麦克风 + audio_bus 总线）
 *
 * 本模块封装 INMP441 MEMS 麦克风的 I2S 音频采集功能。
 *
 * 架构变化（总线化改造）：
 *   - 不再直接调用 i2s_driver 的旧 RX API，而是创建一条 RX 音频总线
 *     （middlewares/audio_bus），并注册为"读者"（消费者）；
 *   - audio_bus 内部任务持续读取麦克风并广播，其他模块（如语音识别、
 *     通话、回声消除）可以各自注册读者，互不影响；
 *   - 本模块只负责"采集"，不关心总线上其他读者的存在。
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
#include "audio_bus.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "MICROPHONE"  /**< 日志标签 */

/* ======================== 硬件引脚与总线配置 ===================================== */

/** INMP441 SD（DOUT）数据引脚 */
#define INMP441_SD_GPIO    34

/** 采集 FIFO 容量：16KB ≈ 93ms @44.1kHz×4 字节 */
#define MICROPHONE_FIFO_BYTES  (16 * 1024)

/** 麦克风物理引脚配置 */
static const i2s_pin_cfg_t Mic_PinCfg = {
    .mclk      = I2S_GPIO_UNUSED,   /* INMP441 不需要 MCLK */
    .bclk      = 2,                 /* 接 INMP441 SCK */
    .ws        = 5,                 /* 接 INMP441 WS */
    .dout      = I2S_GPIO_UNUSED,
    .din       = INMP441_SD_GPIO,   /* 接 INMP441 SD */
    .ws_pol    = false,
    .bit_shift = false,
};

/** 麦克风音频格式：44.1kHz / 32-bit / 立体声槽位（仅收 RIGHT） */
static const i2s_bus_cfg_t Mic_BusCfg = {
    .sample_rate    = MICROPHONE_SAMPLE_RATE,
    .bit_width      = I2S_DATA_BIT_WIDTH_32BIT,
    .slot_mode      = I2S_SLOT_MODE_STEREO,
    .slot_mask      = I2S_STD_SLOT_RIGHT,
    .slot_ws_pol    = true,     /* INMP441 时序需要反转 WS */
    .slot_bit_shift = false,
    .dma_desc_num   = 8,
    .dma_frame_num  = 256,
    .tx_auto_clear  = false,
};

/* ======================== 模块静态变量 ============================================= */

/** 麦克风所属音频总线与读者句柄 */
static audio_bus_handle_t Mic_Bus = NULL;
static audio_reader_handle_t Mic_Reader = NULL;

/* ======================== INMP441 硬件控制 ======================================= */

/**
 * @brief 配置 SD（DOUT）数据引脚内部下拉
 *
 * 根据 INMP441 数据手册：当麦克风不驱动数据时（例如在另一声道的
 * 时隙中），SD 引脚处于三态（高阻态）。如果没有下拉电阻，SD 线
 * 可能悬空并产生随机噪声。这里使用 ESP32 内部约 45kΩ 下拉作为
 * 软件备用方案（推荐仍外加 100kΩ 外部下拉）。
 *
 * 使用 gpio_set_pull_mode() 而非 gpio_config()，以免干扰 I2S 引脚功能。
 */
static void Inmp441_Sd_Pulldown_Enable(void)
{
    // 注意：GPIO34（ESP32 输入专用脚）不支持内部上下拉，返回错误时需外部下拉（推荐 100kΩ）
    esp_err_t err = gpio_set_pull_mode(INMP441_SD_GPIO, GPIO_PULLDOWN_ONLY);
    ESP_LOGW(TAG, "SD pin GPIO%d pull-down mode: %s", INMP441_SD_GPIO, esp_err_to_name(err));
}

/**
 * @brief 使能 INMP441 麦克风硬件
 *
 * 当前配置为 CHIPEN 硬连接 VDD（INMP441_CHIPEN_GPIO = -1），
 * SCK/WS 时钟刚刚启动，需要等待 2^14 SCK 周期（约 16ms）
 * 从待机模式恢复，实际上等待 100ms 确保充分稳定。
 */
static void Inmp441_Enable(void)
{
#if (INMP441_CHIPEN_GPIO >= 0)
    /* 模式 1：MCU 主动控制 CHIPEN 引脚 */
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
    /* 首次上电：2^18 SCK 周期 @SCK≈1.024MHz ≈ 256ms，等待 300ms 留余量 */
    vTaskDelay(pdMS_TO_TICKS(300));
    ESP_LOGI(TAG, "INMP441 enabled (CHIPEN=HIGH)");
#else
    /* 模式 2：CHIPEN 硬连接 VDD，等待待机恢复 */
    ESP_LOGI(TAG, "INMP441 CHIPEN hardwired to VDD, waiting for standby recovery...");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "INMP441 standby recovery complete");
#endif
}

/** @brief 把旧 API 的 tick 超时换算成毫秒 */
static uint32_t mic_timeout_to_ms(uint32_t timeout_ticks)
{
    if (timeout_ticks == UINT32_MAX)
    {
        return UINT32_MAX; /* 无限等待 */
    }
    return (uint32_t)(timeout_ticks * portTICK_PERIOD_MS);
}

/* ======================== 麦克风公共 API ========================================= */

esp_err_t Microphone_Init(void)
{
    /* 重复初始化保护 */
    if (Mic_Bus != NULL)
    {
        ESP_LOGW(TAG, "already initialized");
        return ESP_OK;
    }

    /*
     * 步骤 1：在 I2S 接管 GPIO 之前使能 SD 引脚下拉。
     */
    Inmp441_Sd_Pulldown_Enable();

    /*
     * 步骤 2：创建 RX 音频总线（I2S_NUM_0）。
     * audio_bus 内部会同时启动读取任务（此时麦克风尚未使能，
     * 读取任务只会拿到空数据，无影响），且保证 SCK/WS 时钟先行输出，
     * 这正是 INMP441 正常初始化的前提。
     */
    Mic_Bus = audio_bus_create(AUDIO_BUS_RX, I2S_NUM_0, &Mic_PinCfg, &Mic_BusCfg);
    if (Mic_Bus == NULL)
    {
        ESP_LOGE(TAG, "audio bus create failed");
        return ESP_FAIL;
    }

    /*
     * 步骤 2.5：重新使能 SD 引脚下拉（I2S 初始化可能重置 GPIO 配置）。
     */
    Inmp441_Sd_Pulldown_Enable();

    /*
     * 步骤 3：使能 INMP441。
     * 此时 SCK/WS 时钟已经运行，可以正常退出待机。
     */
    Inmp441_Enable();

    /*
     * 步骤 4：注册为读者（RX 消费者），获取 FIFO 句柄。
     * FIFO 满时按 DROP_OLDEST 丢弃最旧数据，保证实时性。
     */
    esp_err_t ret = audio_reader_register(Mic_Bus, "mic_main",
                                          MICROPHONE_FIFO_BYTES,
                                          AUDIO_FIFO_DROP_OLDEST, &Mic_Reader);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "reader register failed: %s", esp_err_to_name(ret));
        audio_bus_destroy(Mic_Bus);
        Mic_Bus = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "INMP441 microphone initialized, sample_rate=%d",
             MICROPHONE_SAMPLE_RATE);
    return ESP_OK;
}

esp_err_t Microphone_Deinit(void)
{
    if (Mic_Reader != NULL)
    {
        audio_reader_unregister(Mic_Reader);
        Mic_Reader = NULL;
    }
    if (Mic_Bus != NULL)
    {
        audio_bus_destroy(Mic_Bus);
        Mic_Bus = NULL;
    }
    ESP_LOGI(TAG, "INMP441 microphone deinitialized");
    return ESP_OK;
}

esp_err_t Microphone_Read_Raw(uint8_t *buffer, size_t size, size_t *bytes_read, uint32_t timeout)
{
    /* 参数校验 */
    if (buffer == NULL || size == 0)
    {
        ESP_LOGE(TAG, "Invalid buffer parameters");
        return ESP_ERR_INVALID_ARG;
    }
    if (Mic_Reader == NULL)
    {
        ESP_LOGE(TAG, "microphone not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * 从 reader 的私有 FIFO 读取原始 32-bit 数据。
     * 数据格式：每样本 4 字节，INMP441 24-bit 有效数据位于高 24 位。
     */
    return audio_reader_read_raw(Mic_Reader, buffer, size, bytes_read,
                                 mic_timeout_to_ms(timeout));
}

esp_err_t Microphone_Read_Pcm16(int16_t *pcmBuffer, size_t sampleCount, size_t *samplesRead, uint32_t timeout)
{
    if (pcmBuffer == NULL || sampleCount == 0)
    {
        ESP_LOGE(TAG, "Invalid PCM buffer parameters");
        return ESP_ERR_INVALID_ARG;
    }
    if (Mic_Reader == NULL)
    {
        ESP_LOGE(TAG, "microphone not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * audio_bus 内部完成 32-bit → 16-bit 转换（右移 16 位取高 16 位），
     * 与旧实现的转换逻辑完全一致。
     */
    size_t read = 0;
    esp_err_t ret = audio_reader_read_pcm16(Mic_Reader, pcmBuffer, sampleCount,
                                            &read, mic_timeout_to_ms(timeout));
    if (samplesRead != NULL)
    {
        *samplesRead = read;
    }
    return ret;
}