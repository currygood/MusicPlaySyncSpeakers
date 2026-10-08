#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <sys/stat.h>
#include <errno.h>
#include "driver/gpio.h"
#include "StartAndStop.h"
#include "sd_card.h"
#include "amplifier.h"
#include "i2s_driver.h"
#include "microphone.h"
#include "LCD_Touch.h"
#include "i2c_driver.h"
#include <stdbool.h>
#include "bt_audio.h"
#include "call_phone.h"
#include "light_control.h"
#include "audio_decoder.h"
#include <ctype.h>
#include <dirent.h>
#include "nvs_flash.h"
#include "wifi_manager.h"
#include "node_role.h"
#include "sync_protocol.h"
#include "ui.h"


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
 * 有节奏的“嘟嘟”声即说明 microphone + 物理 I2S + 功放链路正常。
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

static int16_t s_hfpDnStereoBuf[2048];

/** bt_audio PCM 回调（bt_audio 任务上下文）：直喂功放验证音频链路 */
static void App_Bt_Pcm_Cb(bt_audio_pcm_source_t source, const int16_t *pcm,
                          size_t samples, uint32_t sample_rate, void *user_ctx)
{
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

    /* HFP 下行是 44.1kHz 单声道，而功放总线为双声道（L/R 写同一份）：
     * 单声道原样写入会被当作双声道帧解析（且字节数常非 4 的倍数，
     * i2s_channel_write 会拒绝），必须先扩为立体声再交给功放。 */
    if (source == BT_AUDIO_PCM_SOURCE_HFP_DOWNLINK)
    {
        if (samples > (sizeof(s_hfpDnStereoBuf) / (2 * sizeof(int16_t))))
        {
            ESP_LOGW(TAG, "HFP mono chunk too large: %u", (unsigned)samples);
            return;
        }
        int16_t *out = s_hfpDnStereoBuf;
        for (size_t i = 0; i < samples; i++)
        {
            out[i * 2]     = pcm[i];
            out[i * 2 + 1] = pcm[i];
        }
        Amplifier_Play_Buffer((const uint8_t *)out, samples * 4, NULL, 200);
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
    {
        /* 手机侧调音量（HFP +VGS / AVRCP 绝对音量，bt_audio 已统一为 0~100）
         * → 同步本机功放（0% 即静音，属正常用户设置，不做下限处理）。 */
        ESP_LOGI(TAG, "volume change: phone=%u%%", evt->data.volume);
        Amplifier_Set_Volume(evt->data.volume);
        break;
    }
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
 *   Microphone_Init()（INMP441 + 麦克风 FIFO）→ bt_audio_create
 *   → call_phone_create
 *
 * 说明：麦克风 FIFO 由 microphone 模块统一管理，call_phone 直接
 * 通过 Microphone_Read_* 读取，无需再暴露总线句柄。
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

    /* 4) CallPhone：esp-sr 唤醒词检测 + HFP 上行（读 microphone FIFO） */
    call_phone_cfg_t cp_cfg = {
        .audio          = audio,
        .sample_rate    = 44100,
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

/* ======================== audio_decoder（SD 卡 MP3 播放）测试 ===================== */

/** 测试音乐目录：SD 卡 FATFS 挂载点 /sdcard/music */
#define AUDIO_DEC_TEST_DIR  SD_CARD_MOUNT_POINT "/music"

/** 单声道→立体声扩展缓冲容量（minimp3 单帧上限 1152 采样/声道 × 2 声道 × 2 缓冲） */
#define AUDIO_DEC_STEREO_BUF_SAMPLES  (1152 * 2 * 2)

/** 测试超时基线：估不出时长时最多播放 60s */
#define AUDIO_DEC_TEST_TIMEOUT_MS     60000

/** 单声道→立体声扩展工作缓冲（测试回调使用） */
static int16_t *s_audDecStereoBuf = NULL;

/** 音频解码测试状态（事件回调写入，主测试循环轮询） */
static volatile bool s_audDecTestEof = false;
static volatile bool s_audDecTestErr = false;
static volatile uint64_t s_audDecTestBytes = 0;  /* 统计：累计回调输出字节 */

/** audio_decoder 事件回调：打印 READY，EOF/ERROR 记录到测试状态 */
static void Audio_Dec_Test_Evt_Cb(audio_decoder_event_id_t event,
                                  const audio_decoder_info_t *info,
                                  void *user_ctx)
{
    (void)user_ctx;
    if (event == AUDIO_DECODER_EVT_READY && info != NULL)
    {
        ESP_LOGI(TAG, "decoder READY: %luHz/%uch dur=%lums bitrate=%lubps",
                 (unsigned long)info->sample_rate, info->channels,
                 (unsigned long)info->duration_ms,
                 (unsigned long)info->bitrate_bps);
    }
    else if (event == AUDIO_DECODER_EVT_EOF)
    {
        s_audDecTestEof = true;
        ESP_LOGI(TAG, "decoder EOF");
    }
    else if (event == AUDIO_DECODER_EVT_ERROR)
    {
        s_audDecTestErr = true;
        ESP_LOGE(TAG, "decoder ERROR");
    }
}

/** audio_decoder PCM 回调：不经 sync_protocol，PCM 直接进功放播放 */
static void Audio_Dec_Test_Pcm_Cb(const int16_t *pcm, size_t samples,
                                  uint32_t sample_rate, uint8_t channels,
                                  void *user_ctx)
{
    (void)user_ctx;

    if (pcm == NULL || samples == 0)
    {
        return;
    }
    s_audDecTestBytes += samples * sizeof(int16_t);

    /* 全链路固定 44.1kHz：非 44.1kHz 文件播放速率会偏移，仅提示一次 */
    if (sample_rate != AMPLIFIER_SAMPLE_RATE)
    {
        static bool warned = false;
        if (!warned)
        {
            warned = true;
            ESP_LOGW(TAG, "mp3 sample_rate=%lu != %d，速率不准确（建议 44.1kHz）",
                     (unsigned long)sample_rate, AMPLIFIER_SAMPLE_RATE);
        }
    }

    /* 单声道 → 立体声（L/R 写同一份），保证全链路双声道语义 */
    if (channels == 1 && s_audDecStereoBuf != NULL)
    {
        size_t frames = samples;   /* 单声道：1 采样 = 1 帧 */
        if (frames > (AUDIO_DEC_STEREO_BUF_SAMPLES / 2))
        {
            frames = AUDIO_DEC_STEREO_BUF_SAMPLES / 2;
        }
        for (size_t i = 0; i < frames; i++)
        {
            s_audDecStereoBuf[i * 2 + 0] = pcm[i];
            s_audDecStereoBuf[i * 2 + 1] = pcm[i];
        }
        Amplifier_Play_Buffer((const uint8_t *)s_audDecStereoBuf,
                              frames * 4, NULL, 200);
        return;
    }

    Amplifier_Play_Buffer((const uint8_t *)pcm, samples * sizeof(uint16_t),
                          NULL, 200);
}

/** 在 /sdcard/music 下寻找第一个 .mp3 文件（大小写不敏感） */
static bool Audio_Dec_Test_Find_Mp3(char *path, size_t pathSize)
{
    DIR *dir = opendir(AUDIO_DEC_TEST_DIR);
    if (dir == NULL)
    {
        ESP_LOGE(TAG, "opendir failed: %s", AUDIO_DEC_TEST_DIR);
        return false;
    }

    bool found = false;
    struct dirent *ent = NULL;
    while ((ent = readdir(dir)) != NULL)
    {
        const char *dot = NULL;
        if (ent->d_name[0] == '.')
        {
            continue;
        }
        dot = strrchr(ent->d_name, '.');
        if (dot != NULL && strlen(dot) == 4 &&
            tolower((unsigned char)dot[1]) == 'm' &&
            tolower((unsigned char)dot[2]) == 'p' &&
            tolower((unsigned char)dot[3]) == '3')
        {
            /* 跳过目录项：FATFS 对“目录名伪装成 .mp3”也能打开成功，
             * 但 fseek/fread 全部失败（即本次遇到的故障模式） */
            if (ent->d_type == DT_DIR)
            {
                ESP_LOGW(TAG, "skip dir-like mp3 entry: %s", ent->d_name);
                continue;
            }
            snprintf(path, pathSize, "%s/%s", AUDIO_DEC_TEST_DIR, ent->d_name);
            found = true;
            break;
        }
    }
    closedir(dir);
    return found;
}

/**
 * @brief 测试：SD 卡 /sdcard/music → audio_decoder 解码 → 功放播放（不经 sync_protocol）
 */
static void Audio_Decoder_Test(void)
{
    char path[256] = {0};
    audio_decoder_handle_t dec = NULL;
    audio_decoder_info_t info;
    uint32_t pos_ms = 0;
    uint32_t cap_ms = 0;
    esp_err_t ret;

    ESP_LOGI(TAG, "=== Audio Decoder Test Start ===");

    ret = Amplifier_Init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Amplifier_Init failed: %s", esp_err_to_name(ret));
        return;
    }
    Amplifier_Set_Volume(80);

    ret = SD_Card_Init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "SD_Card_Init failed: %s", esp_err_to_name(ret));
        Amplifier_Deinit();
        return;
    }
    ESP_LOGI(TAG, "SD card mounted at %s", SD_Card_Get_Mount_Point());

    ret = audio_decoder_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "audio_decoder_init failed: %s", esp_err_to_name(ret));
        audio_decoder_deinit();
        Amplifier_Deinit();
        return;
    }

    /* 找一首测试曲目（music 文件夹下的第一个 .mp3） */
    if (!Audio_Dec_Test_Find_Mp3(path, sizeof(path)))
    {
        ESP_LOGE(TAG, "no .mp3 file found under %s", AUDIO_DEC_TEST_DIR);
        audio_decoder_deinit();
        Amplifier_Deinit();
        return;
    }
    ESP_LOGI(TAG, "test track: %s", path);

    /* 单声道→立体声扩展缓冲 */
    s_audDecStereoBuf = (int16_t *)malloc(AUDIO_DEC_STEREO_BUF_SAMPLES *
                                          sizeof(int16_t));
    if (s_audDecStereoBuf == NULL)
    {
        ESP_LOGE(TAG, "stereo buffer alloc failed");
        audio_decoder_deinit();
        Amplifier_Deinit();
        return;
    }

    /* 打开：内部创建解码任务，文件头解析完成后回调 READY */
    audio_decoder_cfg_t cfg = {
        .on_pcm    = Audio_Dec_Test_Pcm_Cb,
        .pcm_ctx   = NULL,
        .on_event  = Audio_Dec_Test_Evt_Cb,
        .event_ctx = NULL,
    };
    dec = audio_decoder_open(path, &cfg);
    if (dec == NULL)
    {
        ESP_LOGE(TAG, "audio_decoder_open failed: %s", path);
        goto test_exit;
    }

    memset(&info, 0, sizeof(info));
    if (audio_decoder_get_info(dec, &info) == ESP_OK)
    {
        ESP_LOGI(TAG, "track info: %luHz/%uch  dur=%lu ms  bitrate=%lubps",
                 (unsigned long)info.sample_rate, info.channels,
                 (unsigned long)info.duration_ms,
                 (unsigned long)info.bitrate_bps);
    }

    ret = audio_decoder_play(dec);
    ESP_LOGI(TAG, "audio_decoder_play: %s", esp_err_to_name(ret));

    /* 播放到 EOF（或兜底超时），每秒打印一次进度 */
    cap_ms = (info.duration_ms > 0) ? (info.duration_ms + 3000)
                                    : AUDIO_DEC_TEST_TIMEOUT_MS;
    for (uint32_t elapsed = 0; elapsed < cap_ms &&
                               !s_audDecTestEof && !s_audDecTestErr;
         elapsed += 1000)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (audio_decoder_get_position_ms(dec, &pos_ms) == ESP_OK)
        {
            ESP_LOGI(TAG, "position %lu / %lu ms  heap=%u  pcm_bytes=%llu",
                     (unsigned long)pos_ms, (unsigned long)info.duration_ms,
                     (unsigned int)esp_get_free_heap_size(),
                     (unsigned long long)s_audDecTestBytes);
        }
    }

    ESP_LOGI(TAG, "=== Audio Decoder Test Finished (eof=%d err=%d) ===",
             s_audDecTestEof, s_audDecTestErr);

test_exit:
    if (dec != NULL)
    {
        audio_decoder_close(dec);
        dec = NULL;
    }
    free(s_audDecStereoBuf);
    s_audDecStereoBuf = NULL;
    /* 功放与 SD 卡保持挂起状态，便于后续其他顶层测试复用 */
}

/* ======================== WiFi / 组播测试 ======================== */

/** 测试组播组与端口（与分层设计 3.4.2 的音频通道约定一致） */
#define WIFI_TEST_MCAST_GROUP  "239.0.0.1"
#define WIFI_TEST_MCAST_PORT   5678

/** 测试发送次数 */
#define WIFI_TEST_SEND_COUNT   10

/** 测试用：wifi_manager 事件回调（仅打印状态变化，测试观察用） */
static void Wifi_Test_Evt_Cb(const wifi_mgr_event_t *evt, void *ctx)
{
    (void)ctx;

    switch (evt->id)
    {
    case WIFI_EVT_CONNECTED:
        ESP_LOGI(TAG, "[wifi] CONNECTED ip=%s rssi=%d", evt->ip, evt->rssi);
        break;
    case WIFI_EVT_DISCONNECTED:
        ESP_LOGI(TAG, "[wifi] DISCONNECTED");
        break;
    case WIFI_EVT_RECONNECTING:
        ESP_LOGI(TAG, "[wifi] RECONNECTING");
        break;
    case WIFI_EVT_IP_CHANGED:
        ESP_LOGI(TAG, "[wifi] IP_CHANGED ip=%s", evt->ip);
        break;
    default:
        break;
    }
}

/**
 * @brief 测试：wifi_manager 连接热点，并沿组播通道发送/接收 UDP 数据
 *
 * 说明：
 *   - 凭据：测试开头把整份运行参数（含 HW666）经 node_role_set 写入 NVS，
 *     wifi_manager 启动时经 node_role_get() 直接从 NVS 读取连接热点；
 *   - 自环：wifi_manager 已开 IP_MULTICAST_LOOP，本机发的包会经 lwIP 回环回本机，
 *     单机即可验证"发->收"闭环（从/灯控未实现时用）；
 *   - 串口按序打印 CONNECTED -> 每包 send 结果 -> recv 结果（应收到刚发的内容）。
 */
static void Wifi_Mcast_Test(void)
{
    wifi_manager_handle_t wifi = NULL;
    wifi_mcast_handle_t   chan = NULL;
    wifi_manager_cfg_t    cfg  = {0};
    wifi_mcast_cfg_t      mc   = {0};
    char txbuf[64];
    char rxbuf[256];
    size_t rx_len = 0;
    node_role_cfg_t rcfg = {0};
    esp_err_t err;

    /* 1. 运行参数整体保存进 NVS（node_role 为唯一管理入口）：全部字段显式赋值
     *    （默认值来自 node_role.h 公共宏，与《nvs存储.md》一致），node_role_set 整包写回；
     *    wifi 连接凭据也在其中，wifi_manager 启动时经 node_role_get() 直接读取 */
    err = node_role_init(NULL);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "node_role_init failed: %s", esp_err_to_name(err));
        return;
    }

    snprintf(rcfg.wifi_ssid,         sizeof(rcfg.wifi_ssid),         "%s", NODE_ROLE_DEFAULT_SSID);
    snprintf(rcfg.wifi_password,     sizeof(rcfg.wifi_password),     "%s", NODE_ROLE_DEFAULT_PASSWORD);
    snprintf(rcfg.ota_server_url,    sizeof(rcfg.ota_server_url),    "%s", NODE_ROLE_DEFAULT_OTA_URL);
    snprintf(rcfg.multicast_group,   sizeof(rcfg.multicast_group),   "%s", NODE_ROLE_DEFAULT_GROUP);
    rcfg.role              = NODE_ROLE_MASTER;
    rcfg.volume            = NODE_ROLE_DEFAULT_VOLUME;
    rcfg.play_mode         = NODE_ROLE_DEFAULT_PLAY_MODE;
    rcfg.sync_delay_ms     = NODE_ROLE_DEFAULT_SYNC_DELAY_MS;
    rcfg.audio_sample_rate = NODE_ROLE_DEFAULT_SAMPLE_RATE;

    err = node_role_set(&rcfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "node_role_set failed: %s", esp_err_to_name(err));
        return;
    }

    /* 读回并打印整份配置，确认 NVS 里保存的是完整参数（不只是 WiFi 两个字段） */
    if (node_role_get(&rcfg) == ESP_OK)
    {
        ESP_LOGI(TAG, "node_role saved to NVS: role=%u ssid=\"%s\" vol=%u rate=%u delay=%u grp=\"%s\" ota=\"%s\"",
                 (unsigned)rcfg.role, rcfg.wifi_ssid, (unsigned)rcfg.volume,
                 (unsigned)rcfg.audio_sample_rate, (unsigned)rcfg.sync_delay_ms,
                 rcfg.multicast_group, rcfg.ota_server_url);
    }

    ESP_LOGI(TAG, "=== Wifi Multicast Test Start (SSID=\"%s\") ===",
             rcfg.wifi_ssid);

    /* 默认配置：连接超时 10s、重连间隔 3s、max_retries=0 无限重连 */
    wifi = wifi_manager_create(&cfg);
    if (wifi == NULL)
    {
        ESP_LOGE(TAG, "wifi_manager_create failed");
        goto test_exit;
    }
    wifi_manager_register_event_cb(wifi, Wifi_Test_Evt_Cb, NULL);

    err = wifi_manager_start(wifi);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "wifi_manager_start failed: %s", esp_err_to_name(err));
        goto test_exit;
    }
    ESP_LOGI(TAG, "connecting to AP, waiting for IP...");

    /* 轮询等待拿到 IP（最多 15s，超时即退出测试） */
    for (int i = 0; i < 150; i++)
    {
        if (wifi_manager_is_connected(wifi))
        {
            char ip[16] = {0};
            size_t ip_len = sizeof(ip);
            if (wifi_manager_get_ip(wifi, ip, &ip_len) == ESP_OK)
            {
                ESP_LOGI(TAG, "wifi connected, ip=%s rssi=%d",
                         ip, wifi_manager_get_rssi(wifi));
            }
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        if (i == 149)
        {
            ESP_LOGE(TAG, "wait ip timeout, please check SSID/PWD");
            goto test_exit;
        }
    }

    /* 打开音频组播通道：rx_enable=true，自环回包会进 RX FIFO */
    mc.group          = WIFI_TEST_MCAST_GROUP;
    mc.port           = WIFI_TEST_MCAST_PORT;
    mc.rx_enable      = true;
    mc.rx_fifo_bytes  = 4096;
    err = wifi_manager_mcast_open(wifi, &mc, &chan);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "mcast_open %s:%u failed: %s",
                 WIFI_TEST_MCAST_GROUP, WIFI_TEST_MCAST_PORT,
                 esp_err_to_name(err));
        goto test_exit;
    }

    for (int seq = 0; seq < WIFI_TEST_SEND_COUNT; seq++)
    {
        int n = snprintf(txbuf, sizeof(txbuf),
                         "WIFI_TEST seq=%d cnt=%d", seq, WIFI_TEST_SEND_COUNT);

        err = wifi_manager_mcast_send(chan, txbuf, (size_t)n, 1000);
        ESP_LOGI(TAG, "mcast send[%d/%d] %d bytes -> %s",
                 seq + 1, WIFI_TEST_SEND_COUNT, n,
                 (err == ESP_OK) ? "OK" : esp_err_to_name(err));

        /* 接收本机刚发出去的包（lwIP 组播回环），200ms 超时 */
        memset(rxbuf, 0, sizeof(rxbuf));
        err = wifi_manager_mcast_recv(chan, rxbuf, sizeof(rxbuf) - 1,
                                      &rx_len, 200);
        if (err == ESP_OK)
        {
            rxbuf[rx_len] = '\0';
            ESP_LOGI(TAG, "mcast recv %d bytes: \"%s\"", (int)rx_len, rxbuf);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "=== Wifi Multicast Test Finished ===");

test_exit:
    if (chan != NULL)
    {
        wifi_manager_mcast_close(chan);
        chan = NULL;
    }
    wifi_manager_destroy(wifi);
}

/* ======================== LightControl 灯控回环测试 ======================== */

/** 灯控组播通道（分层设计 3.4.2：灯控 239.0.0.2:8889，代码常量不入 NVS） */
#define LIGHT_TEST_MCAST_GROUP      "239.0.0.2"
#define LIGHT_TEST_MCAST_PORT       8889

/** 测试灯名（尚未实现的灯控端按此名开关） */
#define LIGHT_TEST_LIGHT            "bathroom"

/** 测试用状态上报超时（ms）：缩短至 3s，便于验证超时回落 UNKNOWN 路径 */
#define LIGHT_TEST_STATE_TIMEOUT_MS 3000

/** 每步等待模块收包/处理的时间（ms） */
#define LIGHT_TEST_STEP_DELAY_MS    300

/** 测试用：LightControl 事件回调（正式实现由 UI 刷灯图标，这里仅打印） */
static void Light_Test_Evt_Cb(const light_event_t *evt, void *user_ctx)
{
    (void)user_ctx;
    const char *id_str = (evt->id == LIGHT_EVT_ACK)    ? "ACK"    :
                         (evt->id == LIGHT_EVT_STATUS) ? "STATUS" : "ERROR";
    const char *st_str = (evt->data.state == LIGHT_STATE_ON)  ? "ON"     :
                         (evt->data.state == LIGHT_STATE_OFF) ? "OFF"    : "UNKNOWN";

    ESP_LOGI(TAG, "[light] EVENT %s light=\"%s\" ok=%d state=%s",
             id_str, evt->data.light, evt->data.ok, st_str);
}

/** 测试模拟灯控端回包：直接向灯控组播通道发一帧 JSON（ack/status） */
static void Light_Test_Simulate_Device(wifi_mcast_handle_t chan, const char *json)
{
    esp_err_t err = wifi_manager_mcast_send(chan, json, strlen(json), 1000);
    ESP_LOGI(TAG, "[light] simulate device %d bytes: %s -> %s",
             (int)strlen(json), json,
             (err == ESP_OK) ? "OK" : esp_err_to_name(err));
}

/** 测试查询并打印某灯当前状态（模块内部缓存） */
static void Light_Test_Show_State(light_control_handle_t lc, const char *label)
{
    light_state_t st = LIGHT_STATE_UNKNOWN;
    const char *st_str;
    esp_err_t err = light_control_get_state(lc, LIGHT_TEST_LIGHT, &st);

    st_str = (st == LIGHT_STATE_ON)  ? "ON"  :
             (st == LIGHT_STATE_OFF) ? "OFF" : "UNKNOWN";
    ESP_LOGI(TAG, "[light] %s: get_state=%s (%s)", label, st_str,
             (err == ESP_OK) ? "OK" : esp_err_to_name(err));
}

/**
 * @brief 第七阶段测试：LightControl 灯控模块回环测试
 *
 * 说明：
 *   - 灯控端尚未实现，测试采用"单机回环 + 模拟回包"：
 *     1) light_control_send() 下发的 on/off 指令经组播回环，模块自己的
 *        SmartHome_Task 应收到原始帧（日志 "recv ... {\"cmd\":\"on\"...}"）；
 *     2) 由本测试代替灯控端回 ack/status 帧，验证解析与事件回调链路
 *        （LIGHT_EVT_ACK / LIGHT_EVT_STATUS / 超时 LIGHT_EVT_ERROR）；
 *     3) state_timeout_ms 临时缩短为 3s，验证超时后状态落回 UNKNOWN。
 */
static void Light_Control_Test(void)
{
    wifi_manager_handle_t  wifi  = NULL;
    wifi_mcast_handle_t    chan  = NULL;
    light_control_handle_t lc    = NULL;
    wifi_manager_cfg_t     cfg   = {0};
    wifi_mcast_cfg_t       mc    = {0};
    light_control_cfg_t    lcfg  = {0};
    node_role_cfg_t        rcfg  = {0};
    esp_err_t              err;

    /* 1. 运行参数写入 NVS（与 Wifi_Mcast_Test 一致，wifi_manager 启动时读凭据） */
    err = node_role_init(NULL);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "node_role_init failed: %s", esp_err_to_name(err));
        return;
    }

    snprintf(rcfg.wifi_ssid,       sizeof(rcfg.wifi_ssid),       "%s", NODE_ROLE_DEFAULT_SSID);
    snprintf(rcfg.wifi_password,   sizeof(rcfg.wifi_password),   "%s", NODE_ROLE_DEFAULT_PASSWORD);
    snprintf(rcfg.ota_server_url,  sizeof(rcfg.ota_server_url),  "%s", NODE_ROLE_DEFAULT_OTA_URL);
    snprintf(rcfg.multicast_group, sizeof(rcfg.multicast_group), "%s", NODE_ROLE_DEFAULT_GROUP);
    rcfg.role              = NODE_ROLE_MASTER;
    rcfg.volume            = NODE_ROLE_DEFAULT_VOLUME;
    rcfg.play_mode         = NODE_ROLE_DEFAULT_PLAY_MODE;
    rcfg.sync_delay_ms     = NODE_ROLE_DEFAULT_SYNC_DELAY_MS;
    rcfg.audio_sample_rate = NODE_ROLE_DEFAULT_SAMPLE_RATE;

    err = node_role_set(&rcfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "node_role_set failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "=== Light Control Loopback Test Start (SSID=\"%s\") ===",
             rcfg.wifi_ssid);

    /* 2. WiFi 连接（与 mcast 测试一致，等待 IP） */
    wifi = wifi_manager_create(&cfg);
    if (wifi == NULL)
    {
        ESP_LOGE(TAG, "wifi_manager_create failed");
        goto test_exit;
    }
    wifi_manager_register_event_cb(wifi, Wifi_Test_Evt_Cb, NULL);

    err = wifi_manager_start(wifi);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "wifi_manager_start failed: %s", esp_err_to_name(err));
        goto test_exit;
    }
    ESP_LOGI(TAG, "connecting to AP, waiting for IP...");

    for (int i = 0; i < 150; i++)
    {
        if (wifi_manager_is_connected(wifi))
        {
            char ip[16] = {0};
            size_t ip_len = sizeof(ip);
            if (wifi_manager_get_ip(wifi, ip, &ip_len) == ESP_OK)
            {
                ESP_LOGI(TAG, "wifi connected, ip=%s rssi=%d",
                         ip, wifi_manager_get_rssi(wifi));
            }
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        if (i == 149)
        {
            ESP_LOGE(TAG, "wait ip timeout, please check SSID/PWD");
            goto test_exit;
        }
    }

    /* 3. 灯控组播通道：0x8889, rx_enable 打开（本机回环/模拟回包都进接收 FIFO） */
    mc.group        = LIGHT_TEST_MCAST_GROUP;
    mc.port         = LIGHT_TEST_MCAST_PORT;
    mc.rx_enable    = true;
    mc.rx_fifo_bytes = 4096;
    err = wifi_manager_mcast_open(wifi, &mc, &chan);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "mcast_open %s:%u failed: %s",
                 LIGHT_TEST_MCAST_GROUP, LIGHT_TEST_MCAST_PORT,
                 esp_err_to_name(err));
        goto test_exit;
    }

    /* 4. 创建 LightControl（内部创建 SmartHome_Task，Core 0） */
    lcfg.chan              = chan;
    lcfg.tx_timeout_ms     = 100;       /* 与模块默认一致（100ms） */
    lcfg.state_timeout_ms  = LIGHT_TEST_STATE_TIMEOUT_MS;   /* 3s（生产默认 90s） */
    lcfg.on_event          = Light_Test_Evt_Cb;
    lc = light_control_create(&lcfg);
    if (lc == NULL)
    {
        ESP_LOGE(TAG, "light_control_create failed");
        goto test_exit;
    }

    /* 5. 未收到任何上行：状态应为 UNKNOWN */
    Light_Test_Show_State(lc, "before report");

    /* 6. 发送开灯指令：包组播回环，模块收底打印 "recv ... {\"cmd\":\"on\"...}" */
    err = light_control_send(lc, LIGHT_CMD_ON, LIGHT_TEST_LIGHT);
    ESP_LOGI(TAG, "[light] light_control_send(ON) -> %s",
             (err == ESP_OK) ? "OK" : esp_err_to_name(err));
    vTaskDelay(pdMS_TO_TICKS(LIGHT_TEST_STEP_DELAY_MS));

    /* 7. 模拟灯控端回 ack ok=true -> LIGHT_EVT_ACK，状态 ON */
    Light_Test_Simulate_Device(chan,
        "{\"cmd\":\"ack\",\"light\":\"bathroom\",\"ok\":true}");
    vTaskDelay(pdMS_TO_TICKS(LIGHT_TEST_STEP_DELAY_MS));
    Light_Test_Show_State(lc, "after ack ok=true");

    /* 8. OFF 指令 + ack ok=false -> 状态 OFF */
    err = light_control_send(lc, LIGHT_CMD_OFF, LIGHT_TEST_LIGHT);
    ESP_LOGI(TAG, "[light] light_control_send(OFF) -> %s",
             (err == ESP_OK) ? "OK" : esp_err_to_name(err));
    vTaskDelay(pdMS_TO_TICKS(LIGHT_TEST_STEP_DELAY_MS));
    Light_Test_Simulate_Device(chan,
        "{\"cmd\":\"ack\",\"light\":\"bathroom\",\"ok\":false}");
    vTaskDelay(pdMS_TO_TICKS(LIGHT_TEST_STEP_DELAY_MS));
    Light_Test_Show_State(lc, "after ack ok=false");

    /* 9. 模拟周期 status 上报 ok=true -> LIGHT_EVT_STATUS，状态 ON */
    Light_Test_Simulate_Device(chan,
        "{\"cmd\":\"status\",\"light\":\"bathroom\",\"ok\":true}");
    vTaskDelay(pdMS_TO_TICKS(LIGHT_TEST_STEP_DELAY_MS));
    Light_Test_Show_State(lc, "after status ok=true");

    /* 10. 超时（3s 无上报）-> LIGHT_EVT_ERROR，状态 UNKNOWN */
    ESP_LOGI(TAG, "[light] wait %d ms with no report, expect timeout -> UNKNOWN",
             LIGHT_TEST_STATE_TIMEOUT_MS);
    vTaskDelay(pdMS_TO_TICKS(LIGHT_TEST_STATE_TIMEOUT_MS + 1000));
    Light_Test_Show_State(lc, "after timeout");

    ESP_LOGI(TAG, "=== Light Control Loopback Test Finished ===");

test_exit:
    if (lc != NULL)
    {
        light_control_destroy(lc);
        lc = NULL;
    }
    if (chan != NULL)
    {
        wifi_manager_mcast_close(chan);
        chan = NULL;
    }
    wifi_manager_destroy(wifi);
}

/* ======================== 第八阶段：UI 模块联调测试 ======================== */

/**
 * @brief 第八阶段测试：UI 模块（LVGL 9.6 自持 UI_Task）
 *
 * 说明：
 *   - ui_init(NULL)：全占位模式（未接 LightControl/CallPhone/wifi_manager 句柄），
 *     显示/触摸/切页为真实功能，业务按键按下仅打 [占位] 日志注明将来调用的接口；
 *   - UI 内部自举顺序：SPIFFS storage 挂载（图片）→ I2C 总线 → LCD/触摸
 *     → LVGL 9.6 移植 → 页面构建 → UI_Task（Core 1 / 优先级 12 / 栈 13312）；
 *   - 观察点：①屏幕出现"状态栏 + 播放页 + Tab 栏"，封面/灯泡 PNG 正常显示
 *     （需 menuconfig 开 CONFIG_LV_USE_LODEPNG）；②三个 Tab 切换、本地列表
 *     进出；③按播放键/拨灯开关/点配网等，串口打印对应 [占位] 日志；
 *   - 无 SD 卡也不影响本测试（图片已烧录 flash storage 分区，SD 仅音乐用）；
 *   - 与 MP3 解码测试互斥：两者任务同在 Core 1 会互相抢占，分开验证。
 */
static void UI_Test(void)
{
    esp_err_t ret;

    ESP_LOGI(TAG, "=== UI Test Start（第八阶段：界面联调） ===");
    ESP_LOGI(TAG, "free heap before UI: internal=%lu B, psram=%lu B",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    ret = ui_init(NULL);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "ui_init failed: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "free heap after UI: internal=%lu B, psram=%lu B",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "=== UI Test running: 触摸切 Tab / 按播放键 / 拨灯开关，观察 [占位] 日志 ===");
}

/* ======================== 第九阶段：sync_protocol 同步播放模块测试 ======================== */

/** 测试用正弦波参数：1kHz / 幅度 8000（约 49% 满幅，便于听感对比） */
#define SYNC_TEST_SINE_HZ      (1000)
#define SYNC_TEST_SINE_AMP     (8000)
#define SYNC_TEST_2PI          (6.2831853f)

/** 测试采样率：与全链路一致（AMPLIFIER_SAMPLE_RATE = 44.1kHz） */
#define SYNC_TEST_SAMPLE_RATE  (44100)

/** 每 10ms 推 441 个 mono 采样 = 44.1kHz（与模块消费速率相等，不欠载） */
#define SYNC_TEST_PUSH_SAMPLES (441)

/** 音频回环校验包数（每帧 15ms，20 包约 300ms） */
#define SYNC_TEST_AUDIO_PKTS   (20)

/** 回环接收超时（ms） */
#define SYNC_TEST_RECV_TMO_MS  (500)

/** 测试喂音任务停止标志（测试主流程退出时置位） */
static volatile bool s_sync_test_stop = false;

/** 正弦相位游标（喂音任务独占写入） */
static uint32_t s_sync_test_phase = 0;

/** 测试用：sync_protocol 事件回调（仅打印状态变化，观察状态机用） */
static void Sync_Test_Evt_Cb(const sync_protocol_event_t *evt, void *ctx)
{
    (void)ctx;
    const char *id_str = (evt->id == SYNC_EVT_STATE_CHANGED)   ? "STATE_CHANGED" :
                         (evt->id == SYNC_EVT_STREAM_CHANGED)  ? "STREAM_CHANGED" :
                         (evt->id == SYNC_EVT_SLAVE_SYNCED)    ? "SLAVE_SYNCED" :
                         (evt->id == SYNC_EVT_SLAVE_LOST)      ? "SLAVE_LOST" :
                         (evt->id == SYNC_EVT_ERROR)           ? "ERROR" : "UNKNOWN";
    ESP_LOGI(TAG, "[sync] EVT %s state=%d stream=%u", id_str,
             (int)evt->status.state, evt->status.stream_id);
}

/** 测试：持续把 1kHz 正弦 PCM 推进 sync_protocol。
 * 按"距上一拍经过的实时间"补推采样（欠长采样数累积）,长期平均速率严格
 * 等于 44.1kHz——不再用"固定 10ms + 现算 441 点"（那样实际喂入 < 消费，
 * 本地 D 缓冲永远灌不满）。 */
static void Sync_Test_Feed_Task(void *arg)
{
    int16_t chunk[SYNC_TEST_PUSH_SAMPLES];
    (void)arg;
    int64_t last_us = esp_timer_get_time();
    int64_t owed    = 0;   /* 与 44.1kHz 对齐的累计欠账采样数 */

    while (!s_sync_test_stop)
    {
        vTaskDelay(pdMS_TO_TICKS(10));

        int64_t now_us = esp_timer_get_time();
        owed += ((now_us - last_us) * SYNC_TEST_SAMPLE_RATE + 500000) / 1000000;
        last_us = now_us;

        while (owed >= (int64_t)SYNC_TEST_PUSH_SAMPLES)
        {
            for (size_t i = 0; i < SYNC_TEST_PUSH_SAMPLES; i++)
            {
                chunk[i] = (int16_t)(SYNC_TEST_SINE_AMP *
                            sinf(SYNC_TEST_2PI * SYNC_TEST_SINE_HZ *
                                 (float)s_sync_test_phase / SYNC_TEST_SAMPLE_RATE));
                s_sync_test_phase++;
            }
            sync_protocol_master_push_pcm(chunk, SYNC_TEST_PUSH_SAMPLES);
            owed -= (int64_t)SYNC_TEST_PUSH_SAMPLES;
        }
    }
    vTaskDelete(NULL);
}

/**
 * @brief 第九阶段测试：sync_protocol 同步播放模块（单机自环）
 *
 * 说明：
 *   - 测试以独立任务运行（app_main 主任务栈仅 3584B，本测试函数的
 *     pkt[2048] 等栈占用 + 深日志调用会发生栈溢出，故按现有惯例
 *     xTaskCreatePinnedToCore 单独起任务，栈给 16KB）；
 *   - 单机即可验证“发→收”闭环：wifi_manager 已开 IP_MULTICAST_LOOP，且
 *     SO_REUSEADDR 允许同一组播端口绑定多个 socket，每个组播包会向该端口
 *     每个成员拷一份——因此模块 TX 用的发送 socket（只发）与测试自用的
 *     嗅探 socket（只收）可并行存在；
 *   - 控制通道必须开 rx：sync_protocol 主节点侧要收从节点的 PING 并回 PONG；
 *   - 覆盖点（从节点未实现，先验证主节点协议行为）：
 *     ① 音频帧自环：magic/stream_id/seq 连续/帧长 1322|1324/时间戳间隔≈15ms/载荷非 0；
 *     ② 控制命令自环：broadcast PLAY/PAUSE/NEXT/VOLUME 的 4 字节基础帧到 5679；
 *     ③ PING→PONG 应答：构造 t1=now 的 PING 发到控制通道，模块自动回 PONG（t1<t2<=t3）；
 *     ④ pause/resume 状态机：暂停期间不再发包，恢复后 D 缓冲重灌回到 PLAYING；
 *   - 本地最终出声：直接走 amplifier（Amplifier_Play_Buffer），应能听到 1kHz
 *     正弦——为后续主从同拍播放做前置验证。
 */
static void Sync_Protocol_Test(void *arg)
{
    (void)arg;
    wifi_manager_handle_t wifi = NULL;
    wifi_mcast_handle_t   audio_tx = NULL;   /* 传给 sync_protocol：音频只发 */
    wifi_mcast_handle_t   ctrl_tx  = NULL;   /* 传给 sync_protocol：控制收发 */
    wifi_mcast_handle_t   audio_rx = NULL;   /* 测试自环：嗅探音频帧 */
    wifi_mcast_handle_t   ctrl_rx  = NULL;   /* 测试自环：嗅探控制帧 */
    wifi_manager_cfg_t    cfg  = {0};
    wifi_mcast_cfg_t      mc   = {0};
    node_role_cfg_t       rcfg = {0};
    sync_protocol_master_cfg_t scfg = {0};
    sync_protocol_master_status_t st;
    uint8_t pkt[2048];
    size_t  len = 0;
    uint16_t stream_id = 0;
    uint16_t prev_seq  = 0;
    uint64_t prev_ts   = 0;
    bool got_prev = false;
    int  fail  = 0;
    esp_err_t err;

    ESP_LOGI(TAG, "=== Sync Protocol Test Start（第九阶段：同步播放自环） ===");

    /* 0. 功放初始化：sync_protocol 本地播放最终经 amplifier 写 I2S */
    if (Amplifier_Init() != ESP_OK)
    {
        ESP_LOGE(TAG, "Amplifier_Init failed");
        goto test_exit;
    }
    Amplifier_Set_Volume(80);

    /* 1. 运行参数写入 NVS + 连接热点（与 Wifi_Mcast_Test 一致） */
    err = node_role_init(NULL);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "node_role_init failed: %s", esp_err_to_name(err));
        goto test_exit;
    }

    snprintf(rcfg.wifi_ssid,         sizeof(rcfg.wifi_ssid),         "%s", NODE_ROLE_DEFAULT_SSID);
    snprintf(rcfg.wifi_password,     sizeof(rcfg.wifi_password),     "%s", NODE_ROLE_DEFAULT_PASSWORD);
    snprintf(rcfg.ota_server_url,    sizeof(rcfg.ota_server_url),    "%s", NODE_ROLE_DEFAULT_OTA_URL);
    snprintf(rcfg.multicast_group,   sizeof(rcfg.multicast_group),   "%s", NODE_ROLE_DEFAULT_GROUP);
    rcfg.role              = NODE_ROLE_MASTER;
    rcfg.volume            = NODE_ROLE_DEFAULT_VOLUME;
    rcfg.play_mode         = NODE_ROLE_DEFAULT_PLAY_MODE;
    rcfg.sync_delay_ms     = NODE_ROLE_DEFAULT_SYNC_DELAY_MS;
    rcfg.audio_sample_rate = NODE_ROLE_DEFAULT_SAMPLE_RATE;
    err = node_role_set(&rcfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "node_role_set failed: %s", esp_err_to_name(err));
        goto test_exit;
    }

    wifi = wifi_manager_create(&cfg);
    if (wifi == NULL)
    {
        ESP_LOGE(TAG, "wifi_manager_create failed");
        goto test_exit;
    }
    wifi_manager_register_event_cb(wifi, Wifi_Test_Evt_Cb, NULL);
    err = wifi_manager_start(wifi);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "wifi_manager_start failed: %s", esp_err_to_name(err));
        goto test_exit;
    }
    ESP_LOGI(TAG, "connecting to AP, waiting for IP...");
    for (int i = 0; i < 150; i++)
    {
        if (wifi_manager_is_connected(wifi))
        {
            char ip[16] = {0};
            size_t ip_len = sizeof(ip);
            if (wifi_manager_get_ip(wifi, ip, &ip_len) == ESP_OK)
            {
                ESP_LOGI(TAG, "wifi connected, ip=%s rssi=%d",
                         ip, wifi_manager_get_rssi(wifi));
            }
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        if (i == 149)
        {
            ESP_LOGE(TAG, "wait ip timeout, check SSID/PWD");
            goto test_exit;
        }
    }

    /* 2. 打开组播通道（同端口允许多个 socket：每个组播包各收一份拷贝） */
    mc.group = SYNC_PROTOCOL_MCAST_GROUP;
    mc.port  = SYNC_PROTOCOL_MCAST_PORT_AUDIO;
    mc.rx_enable = false;                  /* 模块音频通道只发 */
    err = wifi_manager_mcast_open(wifi, &mc, &audio_tx);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "mcast_open audio_tx failed: %s", esp_err_to_name(err));
        goto test_exit;
    }

    mc.group = SYNC_PROTOCOL_MCAST_GROUP;
    mc.port  = SYNC_PROTOCOL_MCAST_PORT_CTRL;
    mc.rx_enable     = true;               /* 模块控制通道要收 PING→回 PONG */
    mc.rx_fifo_bytes = 4096;
    err = wifi_manager_mcast_open(wifi, &mc, &ctrl_tx);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "mcast_open ctrl_tx failed: %s", esp_err_to_name(err));
        goto test_exit;
    }

    mc.group = SYNC_PROTOCOL_MCAST_GROUP;
    mc.port  = SYNC_PROTOCOL_MCAST_PORT_AUDIO;
    mc.rx_enable = true;                   /* 测试自环：嗅探音频帧 */
    mc.rx_fifo_bytes = 8192;
    err = wifi_manager_mcast_open(wifi, &mc, &audio_rx);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "mcast_open audio_rx failed: %s", esp_err_to_name(err));
        goto test_exit;
    }

    mc.group = SYNC_PROTOCOL_MCAST_GROUP;
    mc.port  = SYNC_PROTOCOL_MCAST_PORT_CTRL;
    mc.rx_enable = true;   /* 测试自环：嗅探控制帧 */
    mc.rx_fifo_bytes = 4096;
    err = wifi_manager_mcast_open(wifi, &mc, &ctrl_rx);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "mcast_open ctrl_rx failed: %s", esp_err_to_name(err));
        goto test_exit;
    }

    /* 3. sync_protocol 初始化 + 启动流（44.1kHz mono）*/
    scfg.sync_delay_ms    = rcfg.sync_delay_ms;   /* 200ms（NVS 参数） */
    scfg.frame_duration_ms = 15;
    scfg.delay_queue_pkts  = 0;                   /* 默认 16 */
    scfg.ingress_bytes     = 0;                   /* 默认 24KB */
    scfg.audio_chan        = audio_tx;
    scfg.control_chan      = ctrl_tx;
    err = sync_protocol_master_init(&scfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "sync_protocol_master_init failed: %s", esp_err_to_name(err));
        goto test_exit;
    }
    sync_protocol_master_register_event_cb(Sync_Test_Evt_Cb, NULL);

    err = sync_protocol_master_start(0, SYNC_TEST_SAMPLE_RATE, 1);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "sync_protocol_master_start failed: %s", esp_err_to_name(err));
        goto test_exit;
    }
    sync_protocol_master_get_status(&st);
    stream_id = st.stream_id;
    ESP_LOGI(TAG, "stream started: id=%u state=%d(0=PREPARING)", stream_id, (int)st.state);

    /* 4. 启动正弦喂入任务（独立任务，不阻塞本测试的阻塞接收） */
    s_sync_test_stop  = false;
    s_sync_test_phase = 0;
    if (xTaskCreatePinnedToCore(Sync_Test_Feed_Task, "sync_feed", 4096,
                                NULL, 5, NULL, 0) != pdPASS)
    {
        ESP_LOGE(TAG, "Sync_Test_Feed_Task create failed");
        fail++;
    }

    /* 5. 阶段 A：音频帧自环逐包校验 */
    ESP_LOGI(TAG, "[A] audio frame loopback: %d pkts", SYNC_TEST_AUDIO_PKTS);
    for (int i = 0; i < SYNC_TEST_AUDIO_PKTS; i++)
    {
        sync_audio_pkt_hdr_t *h = (sync_audio_pkt_hdr_t *)pkt;
        const int16_t *pcm;
        int64_t delta_us = 0;

        len = 0;
        err = wifi_manager_mcast_recv(audio_rx, pkt, sizeof(pkt), &len,
                                      SYNC_TEST_RECV_TMO_MS * 4);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "[A][%d] recv failed: %s", i, esp_err_to_name(err));
            fail++;
            continue;
        }
        if (len < SYNC_AUDIO_HDR_SIZE || h->magic != SYNC_MAGIC)
        {
            ESP_LOGE(TAG, "[A][%d] bad magic/len=%u", i, (unsigned)len);
            fail++;
            continue;
        }
        if (h->stream_id != stream_id)
        {
            ESP_LOGE(TAG, "[A][%d] stream=%u expect=%u", i, h->stream_id, stream_id);
            fail++;
        }
        if (len != SYNC_AUDIO_HDR_SIZE + h->data_len ||
            (h->data_len != 1322 && h->data_len != 1324))
        {
            ESP_LOGE(TAG, "[A][%d] data_len=%u len=%u", i, h->data_len, (unsigned)len);
            fail++;
        }
        if (got_prev && (uint16_t)(h->seq - prev_seq) != 1)
        {
            ESP_LOGE(TAG, "[A][%d] seq jump %u->%u", i, prev_seq, h->seq);
            fail++;
        }
        if (got_prev)
        {
            delta_us = (int64_t)h->timestamp_us - (int64_t)prev_ts;
            if (delta_us < 3000 || delta_us > 40000)
            {
                ESP_LOGE(TAG, "[A][%d] frame interval %lldus", i, (long long)delta_us);
                fail++;
            }
        }
        pcm = (const int16_t *)(pkt + SYNC_AUDIO_HDR_SIZE);
        if (pcm[0] == 0 && pcm[1] == 0 && pcm[2] == 0 && pcm[3] == 0)
        {
            ESP_LOGE(TAG, "[A][%d] payload all zero", i);
            fail++;
        }
        got_prev = true;
        prev_seq = h->seq;
        prev_ts  = h->timestamp_us;
        if (i % 5 == 0)
        {
            ESP_LOGI(TAG, "[A][%d] seq=%u len=%u ts=%llu delta=%lldus",
                     i, h->seq, h->data_len, (unsigned long long)h->timestamp_us,
                     (long long)delta_us);
        }
    }

    /* 6. 阶段 B：控制命令帧自环（PLAY/PAUSE/NEXT/VOLUME） */
    ESP_LOGI(TAG, "[B] control cmd loopback");
    {
        static const sync_protocol_cmd_t cmds[] = {
            SYNC_CMD_PLAY, SYNC_CMD_PAUSE, SYNC_CMD_NEXT, SYNC_CMD_VOLUME,
        };
        for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        {
            uint8_t param = (cmds[i] == SYNC_CMD_VOLUME) ? 0x3C : 0;
            bool got = false;

            err = sync_protocol_master_broadcast_cmd(cmds[i], param);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "[B] send cmd 0x%02X failed: %s",
                         cmds[i], esp_err_to_name(err));
                fail++;
                continue;
            }
            for (int tries = 0; tries < 20 && !got; tries++)
            {
                len = 0;
                err = wifi_manager_mcast_recv(ctrl_rx, pkt, sizeof(pkt), &len, 200);
                if (err != ESP_OK)
                {
                    break;
                }
                if (len != sizeof(sync_ctrl_basic_t))
                {
                    continue;   /* 扩展帧（START/HEARTBEAT/PING/PONG）跳过 */
                }
                sync_ctrl_basic_t *b = (sync_ctrl_basic_t *)pkt;
                if (b->magic != SYNC_MAGIC || b->cmd != (uint8_t)cmds[i] ||
                    b->param != param)
                {
                    ESP_LOGW(TAG, "[B] unexpected basic frame magic=%04X cmd=%02X param=%u",
                             b->magic, b->cmd, b->param);
                    continue;
                }
                ESP_LOGI(TAG, "[B] cmd=0x%02X param=%u loopback OK", b->cmd, b->param);
                got = true;
            }
            if (!got)
            {
                ESP_LOGE(TAG, "[B] cmd 0x%02X not seen on loopback", cmds[i]);
                fail++;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    /* 7. 阶段 C：PING→PONG 自环（模块应收 PING 并自动回 PONG） */
    ESP_LOGI(TAG, "[C] PING/PONG roundtrip");
    {
        sync_ctrl_ext_t     *ext = (sync_ctrl_ext_t *)pkt;
        sync_ping_payload_t  ping_payload;
        uint64_t t1 = (uint64_t)esp_timer_get_time();
        bool got_pong = false;

        ping_payload.t1_us = t1;
        memset(pkt, 0, sizeof(pkt));
        ext->magic = SYNC_MAGIC;
        ext->type  = SYNC_MSG_PING;
        ext->len   = sizeof(ping_payload);
        memcpy(ext->data, &ping_payload, sizeof(ping_payload));
        err = wifi_manager_mcast_send(ctrl_tx, pkt,
                                      sizeof(sync_ctrl_ext_t) + sizeof(ping_payload), 100);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "[C] PING send failed: %s", esp_err_to_name(err));
            fail++;
        }
        else
        {
            for (int tries = 0; tries < 40 && !got_pong; tries++)
            {
                len = 0;
                err = wifi_manager_mcast_recv(ctrl_rx, pkt, sizeof(pkt), &len, 200);
                if (err != ESP_OK)
                {
                    break;
                }
                if (len < sizeof(sync_ctrl_ext_t) ||
                    ((sync_ctrl_ext_t *)pkt)->magic != SYNC_MAGIC)
                {
                    continue;
                }
                ext = (sync_ctrl_ext_t *)pkt;
                if (ext->type != SYNC_MSG_PONG ||
                    ext->len != sizeof(sync_pong_payload_t))
                {
                    continue;
                }
                sync_pong_payload_t *pong = (sync_pong_payload_t *)ext->data;
                if (pong->t1_us != t1)
                {
                    continue;
                }
                if (pong->t1_us <= pong->t2_us && pong->t2_us <= pong->t3_us &&
                    (pong->t2_us - t1) < 5000000u &&
                    (pong->t3_us - pong->t2_us) < 1000000u)
                {
                    ESP_LOGI(TAG, "[C] PONG OK: t1=%llu t2=%llu t3=%llu",
                             (unsigned long long)pong->t1_us,
                             (unsigned long long)pong->t2_us,
                             (unsigned long long)pong->t3_us);
                    got_pong = true;
                }
                else
                {
                    ESP_LOGE(TAG, "[C] PONG timing bad: t1=%llu t2=%llu t3=%llu",
                             (unsigned long long)pong->t1_us,
                             (unsigned long long)pong->t2_us,
                             (unsigned long long)pong->t3_us);
                    fail++;
                }
            }
            if (!got_pong)
            {
                ESP_LOGE(TAG, "[C] no PONG on loopback");
                fail++;
            }
        }
    }

    /* 8. 阶段 D：pause/resume 状态机 */
    ESP_LOGI(TAG, "[D] pause/resume state machine");
    err = sync_protocol_master_pause();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "[D] pause failed: %s", esp_err_to_name(err));
        fail++;
    }
    sync_protocol_master_get_status(&st);
    if (st.state != SYNC_STATE_PAUSED)
    {
        ESP_LOGE(TAG, "[D] pause -> state=%d", (int)st.state);
        fail++;
    }
    else
    {
        ESP_LOGI(TAG, "[D] paused OK");
    }
    /* 暂停期间 TX 应停止发包：取两个时刻对比 */
    vTaskDelay(pdMS_TO_TICKS(300));
    sync_protocol_master_get_status(&st);
    uint32_t paused_tx = st.tx_pkts;
    vTaskDelay(pdMS_TO_TICKS(400));
    sync_protocol_master_get_status(&st);
    if (st.tx_pkts != paused_tx)
    {
        ESP_LOGE(TAG, "[D] tx advanced while paused: %u -> %u", paused_tx, st.tx_pkts);
        fail++;
    }

    err = sync_protocol_master_resume();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "[D] resume failed: %s", esp_err_to_name(err));
        fail++;
    }
    vTaskDelay(pdMS_TO_TICKS(700));   /* 等待 D 缓冲重新灌满 */
    sync_protocol_master_get_status(&st);
    if (st.state != SYNC_STATE_PLAYING || st.tx_pkts == 0 || st.dropped_pkts != 0)
    {
        ESP_LOGE(TAG, "[D] resume misbehave: state=%d tx=%u dropped=%u",
                 (int)st.state, st.tx_pkts, st.dropped_pkts);
        fail++;
    }
    else
    {
        ESP_LOGI(TAG, "[D] resumed OK: state=%d tx=%u dropped=%u",
                 (int)st.state, st.tx_pkts, st.dropped_pkts);
    }

    /* 9. 阶段 E：本地出声观察 + 收尾状态快照 */
    ESP_LOGI(TAG, "[E] keep playing 2s（应能听到 1kHz 正弦，主节点本地功放出声）");
    vTaskDelay(pdMS_TO_TICKS(2000));
    sync_protocol_master_get_status(&st);
    ESP_LOGI(TAG, "final status: state=%d stream=%u tx=%u dropped=%u "
             "ingress=%uB dq_used=%u delta_us=%lld slave_online=%d",
             (int)st.state, st.stream_id, st.tx_pkts, st.dropped_pkts,
             (unsigned)st.ingress_used, (unsigned)st.delay_queue_used,
             (long long)st.play_delta_us, st.slave_online ? 1 : 0);

    /* 收尾：停喂入 → 停流 → 反初始化 */
    s_sync_test_stop = true;
    vTaskDelay(pdMS_TO_TICKS(30));
    sync_protocol_master_stop();
    sync_protocol_master_deinit();
    ESP_LOGI(TAG, "=== Sync Protocol Test Finished (fail=%d) ===", fail);

test_exit:
    if (audio_rx != NULL) wifi_manager_mcast_close(audio_rx);
    if (ctrl_rx  != NULL) wifi_manager_mcast_close(ctrl_rx);
    if (audio_tx != NULL) wifi_manager_mcast_close(audio_tx);
    if (ctrl_tx  != NULL) wifi_manager_mcast_close(ctrl_tx);
    wifi_manager_destroy(wifi);
    vTaskDelete(NULL);
}

void app_main(void)
{
	SystemStart();
	StartAndStop_Init();

	nvs_flash_init();

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
    // CallPhone_Test();   /* 与 MP3 解码测试共用功放，跑 audio_decoder 测试时临时屏蔽 */

	/* SD 卡测试与 LCD 测试各自为任务：同一条 SPI2 总线，SD 测完再通知 LCD */
	// xTaskCreatePinnedToCore(Sd_Card_Test_Task, "sd_test", 8192, NULL, 5, NULL, 0);
	// xTaskCreatePinnedToCore(Lcd_Touch_Test_Task, "lcd_test", 8192, NULL, 4, &s_lcdTestTask, 0);

	/* SD 卡 MP3 播放测试（audio_decoder 模块）：不经 sync_protocol，直接经功放播放 */
    // Audio_Decoder_Test();   /* 测 UI 时临时屏蔽：解码与 UI_Task 同在 Core 1 会互相抢占 */

	/* 第六阶段：wifi_manager 连接 + UDP 组播收发测试（SSID: HW666） */
	// Wifi_Mcast_Test();

	/* 第七阶段：LightControl 灯控模块回环测试 */
	// Light_Control_Test();

	/* 第八阶段：UI 模块联调测试（LVGL 9.6 自持 UI_Task，全占位模式） */
	// UI_Test();

	/* 第九阶段：sync_protocol 同步播放模块自环测试（音频/控制组播自环 + 本地功放）。
	 * 测试函数自带大栈缓冲，按惯例单独起任务（16KB 栈，避开 main_task 3.5KB 限制） */
	// xTaskCreatePinnedToCore(Sync_Protocol_Test, "sync_test", 16384, NULL, 5, NULL, 0);
	
	while(1)
	{
		vTaskDelay(pdMS_TO_TICKS(1000));
	}
}
