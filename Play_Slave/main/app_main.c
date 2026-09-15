#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_psram.h"

#include "driver/spi_common.h"
#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include "amplifier.h"

static const char *TAG = "MAIN";

/* ======================== 5 秒“嘟嘟”测试音 ======================================= */

#define BEEP_FREQ_HZ        800      /* 方波频率（Hz），听感接近“嘟” */
#define BEEP_SAMPLE_RATE    16000    /* 与功放总线采样率一致 */
#define BEEP_AMPLITUDE      6000     /* 幅度，避免大音量削波 */
#define BEEP_ON_MS          250      /* 每次鸣响时长 */
#define BEEP_OFF_MS         250      /* 每次静音时长 */
#define BEEP_TOTAL_MS       5000     /* 总时长：5 秒 → 10 声“嘟” */
#define BEEP_CHUNK_SAMPLES  512      /* 每块采样数（512/16k ≈ 32ms） */

/**
 * @brief 播放 5 秒“嘟嘟”提示音
 *
 * 生成 800Hz 方波 PCM（16-bit/16kHz/单声道），按 0.25s 响、0.25s 停
 * 的节奏通过功放播放，共 5 秒。用作总线化改造后的硬件验证：能听到
 * 有节奏的“嘟嘟”声即说明 audio_bus + 物理 I2S + 功放链路正常。
 */
static void play_beep_test(void)
{
    ESP_LOGI(TAG, "beep test start: 800Hz square, 5s total");

    if (Amplifier_Init() != ESP_OK)
    {
        ESP_LOGE(TAG, "Amplifier_Init failed");
        return;
    }

    int16_t chunk[BEEP_CHUNK_SAMPLES];
    const int period = BEEP_SAMPLE_RATE / BEEP_FREQ_HZ;   /* 20 样本/周期 */
    const int half   = period / 2;                        /* 10 样本/半周期 */
    int phase = 0;                                        /* 相位累加器，跨块连续 */
    uint32_t elapsed_ms = 0;

    while (elapsed_ms < BEEP_TOTAL_MS)
    {
        /* 节奏：前 0.25s 发声，后 0.25s 静音 */
        bool is_sounding = (elapsed_ms % (BEEP_ON_MS + BEEP_OFF_MS)) < BEEP_ON_MS;

        for (int i = 0; i < BEEP_CHUNK_SAMPLES; i++)
        {
            if (is_sounding)
            {
                chunk[i] = ((phase % period) < half) ? BEEP_AMPLITUDE : -BEEP_AMPLITUDE;
            }
            else
            {
                chunk[i] = 0;
            }
            phase++;   /* 静音期间也推进相位，保证再次发声时波形连续 */
        }

        esp_err_t ret = Amplifier_Play_Buffer((const uint8_t *)chunk,
                                              sizeof(chunk), NULL,
                                              pdMS_TO_TICKS(200));
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "beep play failed: %s", esp_err_to_name(ret));
            break;
        }

        elapsed_ms += (BEEP_CHUNK_SAMPLES * 1000) / BEEP_SAMPLE_RATE;   /* 每块约 32ms */
    }

    Amplifier_Deinit();
    ESP_LOGI(TAG, "beep test finished");
}


void app_main(void)
{
    if (esp_psram_is_initialized())
    {
        printf("PSRAM 初始化成功！可用大小: %d 字节\n", esp_psram_get_size());
    }
    else
    {
        printf("PSRAM 未找到或未初始化！\n");
    }

    // /* 再播放一次 5 秒“嘟嘟”测试音 */
    play_beep_test();

    while (1)
    {
        ESP_LOGI("Hello", "Hello world!");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
