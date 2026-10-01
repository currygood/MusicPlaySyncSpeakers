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
#include <stdbool.h>
#include "bt_audio.h"
#include "call_phone.h"


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




/* ======================== 蓝牙 A2DP 测试（bt_audio 模块） ======================= */

static bool s_btA2dpConnected = false;
static TaskHandle_t s_btHfpTestTask = NULL;   /* HFP 语音测试任务句柄 */

/** bt_audio PCM 回调（bt_audio 任务上下文）：直喂功放验证音频链路 */
static void App_Bt_Pcm_Cb(bt_audio_pcm_source_t source, const int16_t *pcm,
                          size_t samples, uint32_t sample_rate, void *user_ctx)
{
    (void)source;
    (void)sample_rate;
    (void)user_ctx;

    if (pcm == NULL || samples == 0)
    {
        return;
    }
    /* A2DP 需等连接；HFP 下行（44.1kHz 单声道）直接播放便于测试应答 */
    if (source == BT_AUDIO_PCM_SOURCE_A2DP && !s_btA2dpConnected)
    {
        return;
    }
    Amplifier_Play_Buffer((const uint8_t *)pcm, samples * sizeof(int16_t), NULL, 200);
}

/** bt_audio 事件回调：打印连接/流状态，并同步播放开关 */
static void App_Bt_Evt_Cb(const bt_audio_event_t *evt, void *user_ctx)
{
    (void)user_ctx;
    if (evt == NULL)
    {
        return;
    }

    switch (evt->id)
    {
    case BT_AUDIO_EVT_A2DP_CONNECTED:
        ESP_LOGI(TAG, "A2DP connected");
        s_btA2dpConnected = true;
        break;
    case BT_AUDIO_EVT_A2DP_DISCONNECTED:
        ESP_LOGI(TAG, "A2DP disconnected");
        s_btA2dpConnected = false;
        break;
    case BT_AUDIO_EVT_A2DP_STREAM_STARTED:
        ESP_LOGI(TAG, "A2DP stream started");
        break;
    case BT_AUDIO_EVT_A2DP_STREAM_STOPPED:
        ESP_LOGI(TAG, "A2DP stream stopped");
        break;
    case BT_AUDIO_EVT_A2DP_SAMPLE_RATE_CHANGED:
        ESP_LOGI(TAG, "A2DP sample rate: %lu Hz", (unsigned long)evt->data.sample_rate);
        break;
    case BT_AUDIO_EVT_HFP_CONNECTED:
        ESP_LOGI(TAG, "HFP SLC connected");
        if (s_btHfpTestTask != NULL)
        {
            xTaskNotifyGive(s_btHfpTestTask);
        }
        break;
    case BT_AUDIO_EVT_HFP_DISCONNECTED:
        ESP_LOGI(TAG, "HFP SLC disconnected");
        break;
    case BT_AUDIO_EVT_HFP_AUDIO_OPEN:
        ESP_LOGI(TAG, "HFP SCO open (%lu Hz)", (unsigned long)evt->data.sample_rate);
        break;
    case BT_AUDIO_EVT_HFP_AUDIO_CLOSE:
        ESP_LOGI(TAG, "HFP SCO closed");
        break;
    case BT_AUDIO_EVT_PLAY_STATE_CHANGED:
        ESP_LOGI(TAG, "AVRCP play state: %d", evt->data.play_state);
        break;
    case BT_AUDIO_EVT_TRACK_CHANGED:
        ESP_LOGI(TAG, "AVRCP track changed");
        break;
    case BT_AUDIO_EVT_TRACK_INFO:
        ESP_LOGI(TAG, "AVRCP track: %s | %s | %s (dur=%lu ms)",
                 evt->data.track_info.title,
                 evt->data.track_info.artist,
                 evt->data.track_info.album,
                 (unsigned long)evt->data.track_info.duration_ms);
        break;
    case BT_AUDIO_EVT_VOLUME_CHANGED:
        ESP_LOGI(TAG, "AVRCP volume: %u", evt->data.volume);
        /* 手机侧调音量（绝对音量命令/通知）→ 同步本机功放 */
        Amplifier_Set_Volume(evt->data.volume);
        break;
    default:
        break;
    }
}

/** HFP 语音测试任务：HFP SLC 连接后自动发送 AT+BVRA=1 唤醒手机语音助手 */
static void BtHfp_Test_Task(void *arg)
{
    bt_audio_handle_t audio = (bt_audio_handle_t)arg;

    ESP_LOGI(TAG, "HFP test: waiting for HFP SLC connected...");
    for (;;)
    {
        /* 等待 HFP SLC 建立（事件回调通知） */
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10000)) != pdTRUE)
        {
            continue;   /* 超时未连接，继续等待 */
        }

        /* SLC 刚建立，稍作延迟再发送激活命令 */
        vTaskDelay(pdMS_TO_TICKS(500));

        esp_err_t err = bt_audio_hfp_start_voice(audio);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "HFP start_voice failed: %s", esp_err_to_name(err));
            continue;
        }
        ESP_LOGI(TAG, "HFP start_voice OK, waiting SCO open...");

        /* 等待 SCO 打开（手机语音助手界面出现） */
        if (bt_audio_hfp_wait_audio_open(audio, 5000) == ESP_OK)
        {
            ESP_LOGI(TAG, "HFP SCO opened! 下行应答已送往功放（如手机有播报会听到）");
        }
        else
        {
            ESP_LOGW(TAG, "HFP SCO not opened within 5s");
        }
    }
}

/** ===== 临时测试：HFP 上行（麦克风 -> SCO -> 手机） =====
 *  验证用：读 INMP441 -> 去交错选槽 -> 44.1kHz 单声道 -> bt_audio_hfp_send_pcm
 *  等 CallPhone 实现后由它接管这条链路，本任务删除。
 */
static void BtHfp_UpTest_Task(void *arg)
{
    bt_audio_handle_t audio = (bt_audio_handle_t)arg;
    int16_t raw[512];    /* 交错双槽样本（每 I2S 帧 2 个槽） */
    int16_t mono[256];   /* 去交错后的单声道 PCM */
    int keepLane = -1;   /* 语音所在槽位（首次能量统计后固定） */
    uint32_t drops = 0;

    esp_err_t ret;
    ESP_LOGI(TAG, "HFP up test: mic ready, waiting SCO open...");

    for (;;)
    {
        /* SCO 未打开就不读麦克风，避免白丢数据 */
        if (!bt_audio_hfp_is_audio_open(audio))
        {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        size_t got = 0;
        ret = Microphone_Read_Pcm16(raw, sizeof(raw) / sizeof(raw[0]), &got, pdMS_TO_TICKS(1000));
        if (ret != ESP_OK || got < 2)
        {
            continue;
        }

        size_t frames = got / 2;   /* 每帧两个槽 */
        size_t lane0 = 0, lane1 = 0;
        for (size_t i = 0; i < frames; i++)
        {
            int a = raw[i * 2 + 0];
            int b = raw[i * 2 + 1];
            lane0 += (a < 0) ? (size_t)(-a) : (size_t)a;
            lane1 += (b < 0) ? (size_t)(-b) : (size_t)b;
        }
        if (keepLane < 0)
        {
            keepLane = (lane0 >= lane1) ? 0 : 1;
            ESP_LOGI(TAG, "HFP up test: keep I2S slot %d", keepLane);
        }
        for (size_t i = 0; i < frames; i++)
        {
            mono[i] = raw[i * 2 + keepLane];
        }

        esp_err_t sret = bt_audio_hfp_send_pcm(audio, mono, frames, 50);
        if (sret != ESP_OK)
        {
            if (++drops % 100 == 1)
            {
                ESP_LOGW(TAG, "HFP up test: send_pcm %s", esp_err_to_name(sret));
            }
        }
    }
}

/** AVRCP 测试任务：手机连接后依次发控制命令 + 分档调音量（UI 音量走 set_volume） */
static void BtAvrc_Test_Task(void *arg)
{
    bt_audio_handle_t audio = (bt_audio_handle_t)arg;
    static const bt_audio_cmd_t seq[] = {
        BT_AUDIO_CMD_TOGGLE_PLAY,   /* 按当前播放状态自动播放/暂停 */
        BT_AUDIO_CMD_PLAY,
        BT_AUDIO_CMD_PAUSE,
        BT_AUDIO_CMD_NEXT,
        BT_AUDIO_CMD_PREV,
        BT_AUDIO_CMD_VOLUME_UP,     /* 走 SetAbsoluteVolume */
        BT_AUDIO_CMD_VOLUME_DOWN,
    };

    ESP_LOGI(TAG, "AVRCP test: waiting for phone A2DP connected...");
    while (!s_btA2dpConnected)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelay(pdMS_TO_TICKS(1500));   /* 给 AVRCP 链路建立留时间 */

    for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++)
    {
        esp_err_t err = bt_audio_send_ctrl_cmd(audio, seq[i]);
        ESP_LOGI(TAG, "AVRCP ctrl cmd %d -> %s", seq[i], esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(1200));
    }

    /* 音量分档测试（UI 旋钮/按键最终都调 bt_audio_set_volume） */
    static const uint8_t vols[] = { 100, 70, 40, 10, 60, 85 };
    for (size_t i = 0; i < sizeof(vols) / sizeof(vols[0]); i++)
    {
        esp_err_t err = bt_audio_set_volume(audio, vols[i]);
        Amplifier_Set_Volume(vols[i]);   /* 测试：本机功放音量跟随 */
        ESP_LOGI(TAG, "set volume %u -> %s", vols[i], esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    /* 曲目/状态查询（元数据事件也已在 App_Bt_Evt_Cb 打印） */
    const bt_audio_track_info_t *ti = NULL;
    esp_err_t err = bt_audio_get_track_info(audio, &ti);
    if (err == ESP_OK && ti != NULL)
    {
        ESP_LOGI(TAG, "query track: %s | %s | %s", ti->title, ti->artist, ti->album);
    }
    else
    {
        ESP_LOGW(TAG, "get_track_info: %s", esp_err_to_name(err));
    }

    bt_audio_play_state_t st = BT_AUDIO_PLAY_STATE_UNKNOWN;
    err = bt_audio_get_play_state(audio, &st);
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "query play state: %d", st);
    }

    ESP_LOGI(TAG, "AVRCP test done, volume=%u", bt_audio_get_volume(audio));
    vTaskDelete(NULL);
}

/** 蓝牙 A2DP 测试：创建并启动 bt_audio，等手机连接后 on_pcm 直连功放 */
static void BtA2dp_Test(void)
{
    /* 注意：必须先初始化麦克风（I2S RX）再初始化功放（I2S TX），否则麦克风不可用 */
    esp_err_t ret = Microphone_Init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Microphone_Init failed: %s", esp_err_to_name(ret));
        return;
    }
    ret = Amplifier_Init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Amplifier_Init failed: %s", esp_err_to_name(ret));
        Microphone_Deinit();
        return;
    }
    Amplifier_Set_Volume(100);

    bt_audio_cfg_t cfg = {
        .device_name = "MPS-Sync-Test",
        .on_pcm      = App_Bt_Pcm_Cb,
        .on_event    = App_Bt_Evt_Cb,
    };
    bt_audio_handle_t audio = bt_audio_create(&cfg);
    if (audio == NULL)
    {
        ESP_LOGE(TAG, "bt_audio_create failed");
        Amplifier_Deinit();
        return;
    }

    ret = bt_audio_start(audio);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "bt_audio_start failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "A2DP test: waiting for phone...");
    ESP_LOGI(TAG, "free heap after bt start: internal=%lu B, psram=%lu B",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // /* HFP 语音测试任务：SLC 连接后自动唤醒手机语音助手 */
    // if (xTaskCreate(BtHfp_Test_Task, "bt_hfp_test", 8192, audio, 5,
    //                 &s_btHfpTestTask) != pdPASS)
    // {
    //     ESP_LOGE(TAG, "bt_hfp_test task create failed");
    // }

    /* AVRCP 测试任务：连接后自动发控制命令 + 音量分档 */
    if (xTaskCreate(BtAvrc_Test_Task, "bt_avrc_test", 8192, audio, 5, NULL) != pdPASS)
    {
        ESP_LOGE(TAG, "bt_avrc_test task create failed");
    }

    // /* ===== 临时测试：麦克风上行 -> bt_audio -> 手机（CallPhone 落地后删除） ===== */
    // if (xTaskCreatePinnedToCore(BtHfp_UpTest_Task, "bt_hfp_uptest",
    //                             8192, audio, 5, NULL, 1) != pdPASS)
    // {
    //     ESP_LOGE(TAG, "bt_hfp_uptest task create failed");
    // }
}

/* ======================== 第四阶段：CallPhone 接线测试 ======================== */

/*
 * 接线顺序（见《主音频节点开发过程》第四阶段）：
 *   Microphone_Init()（自建 RX 总线 + INMP441 使能）→ bt_audio_create
 *   → call_phone_create
 *
 * 说明：RX 总线句柄由 microphone 模块导出（Microphone_GetBus()），
 * 测试不再自建总线、不重复 INMP441 使能步骤。
 */

/* 监视任务上下文 */
typedef struct {
    call_phone_handle_t cp;
    bt_audio_handle_t   audio;   /* 供打印 SCO 状态 */
} call_test_ctx_t;

/** 演示用：STREAMING 持续 CALL_STREAM_KEEP_SECS 秒后自动挂断 */
#define CALL_STREAM_KEEP_SECS  20

static const char *Call_State_Name(call_phone_state_t state)
{
    switch (state)
    {
    case CALL_PHONE_STATE_IDLE:       return "IDLE";
    case CALL_PHONE_STATE_LISTENING:  return "LISTENING";
    case CALL_PHONE_STATE_CONNECTING: return "CONNECTING";
    case CALL_PHONE_STATE_STREAMING:  return "STREAMING";
    default:                          return "??";
    }
}

/** CallPhone 状态监视任务：打印状态迁移 + 自动挂断演示 + 周期打印内存 */
static void CallPhone_Test_Monitor(void *arg)
{
    call_test_ctx_t    *ctx  = (call_test_ctx_t *)arg;
    call_phone_state_t  last = CALL_PHONE_STATE_IDLE;
    uint32_t            stream_sec = 0;
    uint32_t            tick = 0;
    call_phone_state_t  st;

    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (call_phone_get_state(ctx->cp, &st) != ESP_OK)
        {
            continue;
        }

        if (st != last)
        {
            ESP_LOGI(TAG, "CallPhone state: %s -> %s",
                     Call_State_Name(last), Call_State_Name(st));
            last = st;
            stream_sec = 0;
        }

        if (st == CALL_PHONE_STATE_STREAMING)
        {
            stream_sec++;
            ESP_LOGI(TAG, "CallPhone streaming: SCO %s, %u s",
                     bt_audio_hfp_is_audio_open(ctx->audio) ? "open" : "closed",
                     (unsigned int)stream_sec);
            if (stream_sec >= CALL_STREAM_KEEP_SECS)
            {
                ESP_LOGI(TAG, "CallPhone demo: auto hangup after %u s",
                         (unsigned int)stream_sec);
                call_phone_hangup(ctx->cp);
                stream_sec = 0;
                last = CALL_PHONE_STATE_IDLE;   /* 强制下一轮打印回 LISTENING */
            }
        }
        else if (st == CALL_PHONE_STATE_CONNECTING && (++stream_sec % 5) == 0)
        {
            ESP_LOGI(TAG, "CallPhone connecting: waiting SCO open %u s...",
                     (unsigned int)stream_sec);
        }

        /* 每 10s 打印内存余量（对照阶段三问题6：内部 RAM 紧张） */
        if (++tick % 10 == 0)
        {
            ESP_LOGI(TAG, "CallPhone heap: internal=%lu B, psram=%lu B",
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        }
    }
}

/** 第四阶段接线测试：Microphone_Init()（RX 总线）→ bt_audio_create → call_phone_create */
static void CallPhone_Test(void)
{
    esp_err_t ret;

    /* 1) microphone 模块初始化（自建 RX 总线 + INMP441 使能；先于 TX） */
    ret = Microphone_Init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Microphone_Init failed: %s", esp_err_to_name(ret));
        return;
    }
    audio_bus_handle_t rx_bus = Microphone_GetBus();
    if (rx_bus == NULL)
    {
        ESP_LOGE(TAG, "Microphone_GetBus failed");
        Microphone_Deinit();
        return;
    }

    /* 2) 功放（TX）——HFP 下行经 on_pcm 播放 */
    ret = Amplifier_Init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Amplifier_Init failed: %s", esp_err_to_name(ret));
        Microphone_Deinit();
        return;
    }
    Amplifier_Set_Volume(100);

    /* 3) bt_audio：复用 HFP 能力 + 下行/事件回调 */
    bt_audio_cfg_t bt_cfg = {
        .device_name = "MPS-Sync-Test",
        .on_pcm      = App_Bt_Pcm_Cb,
        .on_event    = App_Bt_Evt_Cb,
    };
    bt_audio_handle_t audio = bt_audio_create(&bt_cfg);
    if (audio == NULL)
    {
        ESP_LOGE(TAG, "bt_audio_create failed");
        Amplifier_Deinit();
        Microphone_Deinit();
        return;
    }

    ret = bt_audio_start(audio);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "bt_audio_start failed: %s", esp_err_to_name(ret));
        return;
    }

    /* 4) CallPhone：注册 reader + esp-sr 唤醒词检测 + HFP 上行 */
    call_phone_cfg_t cp_cfg = {
        .bus            = rx_bus,
        .audio          = audio,
        .sample_rate    = 44100,
        .pcm_fifo_bytes = 0,   /* 0 = 默认 16KB */
    };
    call_phone_handle_t cp = call_phone_create(&cp_cfg);
    if (cp == NULL)
    {
        ESP_LOGE(TAG, "call_phone_create failed");
        return;
    }

    ret = call_phone_start_listening(cp);
    ESP_LOGI(TAG, "CallPhone start listening: %s (唤醒词: 你好小智)",
             esp_err_to_name(ret));
    if (ret != ESP_OK)
    {
        return;
    }

    /* 5) 状态监视任务（状态迁移 / 自动挂断 / 内存监视） */
    call_test_ctx_t *ctx = (call_test_ctx_t *)calloc(1, sizeof(*ctx));
    if (ctx != NULL)
    {
        ctx->cp = cp;
        ctx->audio = audio;
        if (xTaskCreatePinnedToCore(CallPhone_Test_Monitor, "call_mon",
                                    4096, ctx, 5, NULL, 1) != pdPASS)
        {
            ESP_LOGE(TAG, "call_phone monitor task create failed");
            free(ctx);
        }
    }

    ESP_LOGI(TAG, "CallPhone phase-4 test ready: 请连接手机并说 你好小智");
    ESP_LOGI(TAG, "free heap after bt+callphone: internal=%lu B, psram=%lu B",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
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

    /* 蓝牙 A2DP 测试（bt_audio 模块） */
    // BtA2dp_Test();

    /* 第四阶段：CallPhone 接线测试（完成后请注释掉） */
    CallPhone_Test();

	/* SD 卡测试与 LCD 测试各自为任务：同一条 SPI2 总线，SD 测完再通知 LCD */
	// xTaskCreatePinnedToCore(Sd_Card_Test_Task, "sd_test", 8192, NULL, 5, NULL, 0);
	// xTaskCreatePinnedToCore(Lcd_Touch_Test_Task, "lcd_test", 8192, NULL, 4, &s_lcdTestTask, 0);

	
	while(1)
	{
		vTaskDelay(pdMS_TO_TICKS(1000));
	}
}
