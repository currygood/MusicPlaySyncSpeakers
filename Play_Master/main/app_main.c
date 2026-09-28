#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "StartAndStop.h"
#include "sd_card.h"
#include "amplifier.h"
#include "i2s_driver.h"
#include "microphone.h"
#include "LCD_Touch.h"
#include "i2c_driver.h"
#include "audio_bus.h"
#include "bt_test.h"


static const char *TAG = "AppMain";

/** 一次性格式化开关：第一次烧录置 1（会清空整张卡！）；确认正常后改 0 */
#define SD_CARD_FORMAT_ONCE 0

/** 输出数据前 16 字节 hex，便于与写入内容逐字节比对 */
static void Sd_Test_Hex(const char *label, const char *data, size_t len)
{
    char line[64];
    size_t n = 0;
    size_t show = (len > 16) ? 16 : len;
    for (size_t i = 0; i < show; i++)
    {
        n += snprintf(line + n, sizeof(line) - n, "%02X", (uint8_t)data[i]);
    }
    ESP_LOGI(TAG, "%s %s", label, line);
}

static void Sd_Card_Test(void)
{
    const char *testFilePath = "/sdcard/test.txt";
    const char *testData = "Hello SD Card! This is a test message.";
    uint8_t readBuffer[128] = {0};
    size_t bytesWritten = 0;
    size_t bytesRead = 0;

    ESP_LOGI(TAG, "=== SD Card Test Start ===");

#if SD_CARD_FORMAT_ONCE
    SD_Card_Set_Format_Once(true);
#endif

    esp_err_t ret = SD_Card_Init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "SD_Card_Init failed: %d", ret);
        return;
    }
    ESP_LOGI(TAG, "SD Card init success, mount point: %s", SD_Card_Get_Mount_Point());

    sd_card_file_handle_t file = SD_Card_Open(testFilePath, "w");
    if (file == NULL)
    {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", testFilePath);
        return;
    }

    ret = SD_Card_Write(file, testData, strlen(testData), &bytesWritten);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "SD_Card_Write failed: %d", ret);
        SD_Card_Close(file);
        return;
    }
    ESP_LOGI(TAG, "Write %d bytes to %s", bytesWritten, testFilePath);
    Sd_Test_Hex("written:  ", testData, strlen(testData));

    SD_Card_Close(file);
    ESP_LOGI(TAG, "File closed after writing");
    SD_Card_Diag_File();   /* FAT 层诊断：目录项/FAT/物理数据扇区 */

    file = SD_Card_Open(testFilePath, "r");
    if (file == NULL)
    {
        ESP_LOGE(TAG, "Failed to open file for reading: %s", testFilePath);
        return;
    }

    memset(readBuffer, 0, sizeof(readBuffer));
    ret = SD_Card_Read(file, readBuffer, sizeof(readBuffer) - 1, &bytesRead);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "SD_Card_Read failed: %d", ret);
        SD_Card_Close(file);
        return;
    }
    readBuffer[bytesRead] = '\0';
    ESP_LOGI(TAG, "Read %d bytes from %s: \"%s\"", bytesRead, testFilePath, (char *)readBuffer);
    Sd_Test_Hex("readback: ", (const char *)readBuffer, bytesRead);

    if (strcmp((char *)readBuffer, testData) == 0)
    {
        ESP_LOGI(TAG, "Read data matches written data! Test PASS");
    }
    else
    {
        ESP_LOGE(TAG, "Read data does NOT match! Test FAIL");
    }

    SD_Card_Close(file);
    ESP_LOGI(TAG, "File closed after reading");

    ret = remove(testFilePath);
    if (ret == 0)
    {
        ESP_LOGI(TAG, "File deleted successfully: %s", testFilePath);
    }
    else
    {
        ESP_LOGE(TAG, "Failed to delete file: %s (error: %d)", testFilePath, ret);
    }

    file = SD_Card_Open(testFilePath, "r");
    if (file == NULL)
    {
        ESP_LOGI(TAG, "File not found after deletion - confirmed! Test PASS");
    }
    else
    {
        ESP_LOGE(TAG, "File still exists after deletion! Test FAIL");
        SD_Card_Close(file);
    }

    ESP_LOGI(TAG, "=== SD Card Test End ===");
}

#define BEEP_FREQ_HZ        800      /* 方波频率（Hz），听感接近“嘟” */
#define BEEP_SAMPLE_RATE    44100    /* 与功放总线采样率一致（全链路固定 44.1kHz） */
#define BEEP_AMPLITUDE      6000     /* 幅度，避免大音量削波 */
#define BEEP_ON_MS          250      /* 每次鸣响时长 */
#define BEEP_OFF_MS         250      /* 每次静音时长 */
#define BEEP_TOTAL_MS       5000     /* 总时长：5 秒 → 10 声“嘟” */
#define BEEP_CHUNK_SAMPLES  512      /* 每块采样数（512/44.1k ≈ 11.6ms） */

/* ======================== 持续正弦波输出测试（示波器） ============================ */

#define SINE_FREQ_HZ     1000     /* 正弦波频率（Hz） */
#define SINE_AMPLITUDE   32000    /* 满幅输出：排除“音量太小”听不见的可能 */


/* ======================== 5 秒"嘟嘟"测试音 ======================================= */


/**
 * @brief 播放 5 秒“嘟嘟”提示音
 *
 * 生成 800Hz 方波 PCM（16-bit/44.1kHz/双声道），按 0.25s 响、0.25s 停
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

    int16_t chunk[BEEP_CHUNK_SAMPLES * 2];
    const int period = BEEP_SAMPLE_RATE / BEEP_FREQ_HZ;   /* 55 样本/周期（44.1k/800Hz） */
    const int half   = period / 2;                        /* 10 样本/半周期 */
    int phase = 0;                                        /* 相位累加器，跨块连续 */
    uint32_t elapsed_ms = 0;

    while (elapsed_ms < BEEP_TOTAL_MS)
    {
        /* 节奏：前 0.25s 发声，后 0.25s 静音 */
        bool is_sounding = (elapsed_ms % (BEEP_ON_MS + BEEP_OFF_MS)) < BEEP_ON_MS;

        for (int i = 0; i < BEEP_CHUNK_SAMPLES; i++)
        {
            int16_t v = 0;
            if (is_sounding)
            {
                v = ((phase % period) < half) ? BEEP_AMPLITUDE : -BEEP_AMPLITUDE;
            }
            chunk[i * 2]     = v;   /* left  slot */
            chunk[i * 2 + 1] = v;   /* right slot */
            phase++;   /* 静音期间也推进相位，保证再次发声时波形连续 */
        }

        esp_err_t ret = Amplifier_Play_Buffer((const uint8_t *)chunk,
                                              sizeof(chunk), NULL, 1000);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "beep play failed at %lums: %s (DMA: desc=16, frame=512)",
                     (unsigned long)elapsed_ms, esp_err_to_name(ret));
            break;
        }

        elapsed_ms += (BEEP_CHUNK_SAMPLES * 1000) / BEEP_SAMPLE_RATE;   /* 每块约 32ms */
    }

    Amplifier_Deinit();
    ESP_LOGI(TAG, "beep test finished");
}


/* ======================== 共用 SPI2 的测试任务 ========================
 * SD 卡测试与 LCD+触摸测试分时复用同一条 SPI2 总线：
 * 先跑 SD 任务（初始化总线），完成后通知 LCD 任务开始初始化/刷屏。
 */

#if LCD_TOUCH_SELF_TEST
extern void Lcd_Touch_Test(void);   /* LCD_Touch.c 内部自检入口，非公共 API */
#endif

static TaskHandle_t s_lcdTestTask = NULL;

/** SD 卡测试任务：跑完文件系统测试后通知 LCD 任务 */
static void Sd_Card_Test_Task(void *arg)
{
    Sd_Card_Test();

    if (s_lcdTestTask != NULL)
    {
        xTaskNotifyGive(s_lcdTestTask);
    }
    vTaskDelete(NULL);
}

/** LCD + 触摸测试任务：等 SD 测试完成通知后开始（内部 while(1)） */
static void Lcd_Touch_Test_Task(void *arg)
{
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(60000));   /* 最多等 60s，防死锁 */

#if LCD_TOUCH_SELF_TEST
    Lcd_Touch_Test();
#endif
    vTaskDelete(NULL);
}

/* ======================== 麦克风录音 -> 功放回放 测试 ======================== */

#define MIC_TEST_SECONDS       10                   /* 录音时长（秒） */
#define MIC_TEST_SAMPLE_RATE   44100                /* 采样率：全链路统一 44.1kHz */
#define MIC_TEST_CHUNK_SAMPLES 512                  /* 每次读取的采样数（单声道 PCM16） */
#define MIC_TEST_GAIN          16                   /* 录音放大倍数（8/12/16/24/32 自行实验） */

/** 用麦克风录音 10s，再通过功放播放出来 */
static void Mic_Record_Playback_Test(void)
{
    ESP_LOGI(TAG, "=== Mic Record(10s) -> Amplifier Playback Test Start ===");

    /* I2S RX 每个 44.1kHz 帧输出 2 个槽位样本（L/R 各 32bit）：
     * 先全量采集（2 x 441000），之后按能量去交错还原真正的 44.1kHz 语音 */
    const size_t totalSamples = MIC_TEST_SECONDS * MIC_TEST_SAMPLE_RATE * 2;
    const size_t monoBytes    = totalSamples * sizeof(int16_t);          /* 640KB */

    /* 1) 分配录音缓冲：优先 PSRAM，失败退回内部 RAM */
    int16_t *rec = heap_caps_malloc(monoBytes, MALLOC_CAP_SPIRAM);
    if (rec == NULL)
    {
        ESP_LOGW(TAG, "PSRAM alloc failed (%uB), fallback to internal RAM...", (unsigned)monoBytes);
        rec = (int16_t *)malloc(monoBytes);
    }
    if (rec == NULL)
    {
        ESP_LOGE(TAG, "No memory for recording buffer (%uB)", (unsigned)monoBytes);
        return;
    }

    int16_t *chunk = (int16_t *)malloc(MIC_TEST_CHUNK_SAMPLES * sizeof(int16_t));
    if (chunk == NULL)
    {
        ESP_LOGE(TAG, "No memory for read chunk buffer");
        free(rec);
        return;
    }

    /* 2) 初始化麦克风（I2S0 RX）和功放（I2S1 TX），采样率均为 44.1kHz */
    esp_err_t ret = Microphone_Init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Microphone_Init failed: %s", esp_err_to_name(ret));
        free(chunk);
        free(rec);
        return;
    }
    ret = Amplifier_Init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Amplifier_Init failed: %s", esp_err_to_name(ret));
        Microphone_Deinit();
        free(chunk);
        free(rec);
        return;
    }
    Amplifier_Set_Volume(80);

    /* 3) 录音 10s：分块读取单声道 PCM16，写入录音缓冲 */
    size_t recorded = 0;
    size_t quietCnt = 0;
    ESP_LOGI(TAG, "Recording %d seconds, please speak/tap near the mic...",
             MIC_TEST_SECONDS);
    while (recorded < totalSamples)
    {
        size_t want = MIC_TEST_CHUNK_SAMPLES;
        if (want > totalSamples - recorded)
        {
            want = totalSamples - recorded;
        }
        size_t got = 0;
        ret = Microphone_Read_Pcm16(chunk, want, &got, pdMS_TO_TICKS(1000));
        if (ret != ESP_OK || got == 0)
        {
            if (++quietCnt > 50)    /* 连续失败约 50 次才放弃，抗瞬时超时 */
            {
                ESP_LOGE(TAG, "Mic read keeps failing: %s, abort",
                         esp_err_to_name(ret));
                break;
            }
            continue;
        }
        quietCnt = 0;
        memcpy((uint8_t *)rec + recorded * sizeof(int16_t), chunk,
               got * sizeof(int16_t));
        recorded += got;

        if (recorded % (MIC_TEST_SAMPLE_RATE * 2) < MIC_TEST_CHUNK_SAMPLES)
        {
            ESP_LOGI(TAG, "record: %u/%u samples",
                     (unsigned)recorded, (unsigned)totalSamples);
        }
    }
    ESP_LOGI(TAG, "Recorded %u samples (%u bytes)",
             (unsigned)recorded, (unsigned)(recorded * sizeof(int16_t)));

    /* 4) 去交错：每 2 个样本是一帧（两个槽位），取能量大的那个槽，还原 44.1kHz 语音 */
    {
        int64_t evenEnergy = 0;
        int64_t oddEnergy  = 0;
        for (size_t i = 0; i + 1 < recorded; i += 2)
        {
            int64_t a = rec[i];
            int64_t b = rec[i + 1];
            evenEnergy += (a < 0) ? -a : a;
            oddEnergy  += (b < 0) ? -b : b;
        }
        int keepLane = (evenEnergy >= oddEnergy) ? 0 : 1;   /* 槽位偏移 0/1 */
        size_t frames = recorded / 2;
        for (size_t i = 0; i < frames; i++)
        {
            rec[i] = rec[i * 2 + keepLane];
        }
        recorded = frames;
        ESP_LOGI(TAG, "de-interleave: keep slot %d, frames=%u (even=%lld odd=%lld)",
                 keepLane, (unsigned)recorded,
                 (long long)evenEnergy, (long long)oddEnergy);
    }

    /* 5) 波形诊断：基于原始录音的峰值统计（改动后 rec 已是 44.1kHz 单声道） */
    int32_t peak = 0;
    uint32_t activeCnt = 0;
    for (size_t i = 0; i < recorded; i++)
    {
        int32_t v = rec[i];
        if (v < 0)
        {
            v = -v;
        }
        if (v > peak)
        {
            peak = v;
        }
        if (v > 200)
        {
            activeCnt++;            /* 明显大于静止底噪的采样数 */
        }
    }
    int32_t peakAfter = peak * MIC_TEST_GAIN;
    if (peakAfter > 32767)
    {
        peakAfter = 32767;
    }
    ESP_LOGI(TAG, "record raw peak=%d active=%u/%u (raw x%d = %d, 会削顶)",
             (int)peak, (unsigned)activeCnt, (unsigned)recorded,
             MIC_TEST_GAIN, (int)peakAfter);

    /* 6) 回放：单声道 -> 立体声（L/R 各一份），分块写入功放。
     *    不做滤波，仅按 MIC_TEST_GAIN 放大，原始 rec 不被修改。 */
    ESP_LOGI(TAG, "Playback through NS4168 (~%d seconds) ...",
             (int)(recorded / MIC_TEST_SAMPLE_RATE));
    int16_t *stereo = (int16_t *)malloc(MIC_TEST_CHUNK_SAMPLES * 2 * sizeof(int16_t));
    if (stereo == NULL)
    {
        ESP_LOGE(TAG, "No memory for stereo chunk");
        free(chunk);
        free(rec);
        return;
    }
    for (size_t off = 0; off < recorded; off += MIC_TEST_CHUNK_SAMPLES)
    {
        size_t n = MIC_TEST_CHUNK_SAMPLES;
        if (n > recorded - off)
        {
            n = recorded - off;
        }
        for (size_t i = 0; i < n; i++)
        {
            int32_t amp = rec[off + i] * MIC_TEST_GAIN;
            if (amp > 32767)
            {
                amp = 32767;
            }
            else if (amp < -32768)
            {
                amp = -32768;
            }
            stereo[i * 2 + 0] = (int16_t)amp;
            stereo[i * 2 + 1] = (int16_t)amp;
        }
        size_t written = 0;
        ret = Amplifier_Play_Buffer((const uint8_t *)stereo, n * 4,
                                    &written, 500);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Playback failed at offset %u: %s",
                     (unsigned)off, esp_err_to_name(ret));
            break;
        }
    }
    ESP_LOGI(TAG, "=== Mic Record -> Playback Test Finished ===");

    /* 7) 清理：先停功放，再停麦克风 */
    Amplifier_Deinit();
    Microphone_Deinit();
    free(stereo);
    free(chunk);
    free(rec);
}


void app_main(void)
{
	SystemStart();
	StartAndStop_Init();

	if (esp_psram_is_initialized()) {
        printf("PSRAM 初始化成功！可用大小: %d 字节\n", esp_psram_get_size());
    } else {
        printf("PSRAM 未找到或未初始化！\n");
    }

	/* 麦克风录音 10s -> 功放回放测试（测试完请注释掉） */
	// Mic_Record_Playback_Test();

	/* 蓝牙功放测试 */
	BtAmpTest_Start();

	/* SD 卡测试与 LCD 测试各自为任务：同一条 SPI2 总线，SD 测完再通知 LCD */
	//xTaskCreatePinnedToCore(Sd_Card_Test_Task, "sd_test", 8192, NULL, 5, NULL, 0);
	//xTaskCreatePinnedToCore(Lcd_Touch_Test_Task, "lcd_test", 8192, NULL, 4, &s_lcdTestTask, 0);

	while(1)
	{
		vTaskDelay(pdMS_TO_TICKS(1000));
	}
}
