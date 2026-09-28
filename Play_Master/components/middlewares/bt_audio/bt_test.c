/* ================================================================
 * bt_a2dp_amp_test.c  （临时测试，不属于正式模块）
 * 目标：验证「手机 A2DP 连接 → ESP32 → 功放出声」整条链路。
 * 注意：
 *  - 采样率仅为测试，直接按 A2DP 原始 PCM(44.1k/48k双声道) 写本机 44.1k 功放，
 *    声音会变调 / 速率不均，仅验证链路，明确不是正式音路；
 *  - 正式实现会经由 bt_audio 内部任务 + sync_protocol 重采样后播放。
 * ================================================================ */

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"

#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"          /* 可选，注册后可收到手机音量/播放状态 */

#include "amplifier.h"

#define BT_TEST_TAG            "BT_AMP_TEST"
#define BT_TEST_DEVICE_NAME    "MPS-Sync-Test"

/* A2DP 数据暂存缓冲区（StreamBuffer 大小，字节） */
#define BT_TEST_PCM_BUF_BYTES  96 * 1024
/* 播放任务优先级：需高于普通任务，保证 PCM 连续 */
#define BT_TEST_PLAY_PRIO      8
/* 功放初始化采样率（与 Amplifier_Init 一致，44.1kHz 双声道总线） */
#define BT_TEST_SAMPLE_RATE    44100U

static StreamBufferHandle_t s_a2dp_stream = NULL;
static volatile bool        s_bt_connected  = false;

/* ------------------------------------------------------------------ */
/* A2DP 数据回调：在蓝牙协议栈上下文被调用，只入流，不做 I/O           */
/* ------------------------------------------------------------------ */
static void bt_a2dp_data_cb(const uint8_t *data, uint32_t len)
{
    if (s_a2dp_stream == NULL || data == NULL || len == 0) {
        return;
    }
    /* 阻塞发送：若播放任务 500ms 内没消费，丢当前块（DMA 满时降速） */
    xStreamBufferSend(s_a2dp_stream, data, len, pdMS_TO_TICKS(50));
}

/* ------------------------------------------------------------------ */
/* A2DP 连接状态回调                                                   */
/* ------------------------------------------------------------------ */
static void bt_a2dp_event_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event)
    {
    case ESP_A2D_CONNECTION_STATE_EVT:
    {
        s_bt_connected = (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED);
        const char *p = param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED ? "CONNECTED"
                      : "disconnected/connecting";
        ESP_LOGI(BT_TEST_TAG, "A2DP conn state: %s", p);
        break;
    }
    case ESP_A2D_AUDIO_STATE_EVT:
        ESP_LOGI(BT_TEST_TAG, "A2DP audio state: %d", param->audio_stat.state);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* GAP 回调：配对 / 发现设置                                          */
/* ------------------------------------------------------------------ */
static void bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        ESP_LOGI(BT_TEST_TAG, "pairing %s (addr %02x:%02x:...)",
                 param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS ? "ok" : "failed",
                 param->auth_cmpl.bda[0], param->auth_cmpl.bda[1]);
        break;
    case ESP_BT_GAP_DISC_RES_EVT:
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
    case ESP_BT_GAP_RMT_SRVCS_EVT:   /* 测试阶段忽略 */
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* 播放器：从 StreamBuffer 取 PCM → 直接喂功放                        */
/* ------------------------------------------------------------------ */
static void bt_amp_playback_task(void *arg)
{
    uint8_t *buf = malloc(4096);
    if (buf == NULL) {vTaskDelete(NULL); return; }

    ESP_LOGI(BT_TEST_TAG, "start A2DP PCM stream");
    size_t total = 0;
    for (;;) {
        size_t n = xStreamBufferReceive(s_a2dp_stream, buf, 2048*2, pdMS_TO_TICKS(100));
        if (n == 0) {
            /* 未连接 / 手机暂停 → 功放静音，依靠 tx_auto_clear */
            total = 0;
            continue;
        }
        Amplifier_Play_Buffer(buf, n, NULL, 200);
        total += n;
    }
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* 主任务：蓝牙初始化 → 广播 → 等连接                                */
/* ------------------------------------------------------------------ */
static void bt_sink_test_task(void *arg)
{
    /* 1) NVS（蓝牙挺保存） */
    nvs_flash_init();   /* 若已在别处初始化过会幂等失败，忽略即可 */

    /* 2) 功放 */
    ESP_ERROR_CHECK(Amplifier_Init());
    Amplifier_Set_Volume(80);

    /* 3) 蓝牙控制器：仅 Classic（释放 BLE 内存） */
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BTDM));

    /* 4) Bluedroid */
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    /* 5) A2DP Sink */
    ESP_ERROR_CHECK(esp_a2d_sink_init());
    ESP_ERROR_CHECK(esp_a2d_register_callback(bt_a2dp_event_cb));
    ESP_ERROR_CHECK(esp_a2d_sink_register_data_callback(bt_a2dp_data_cb));

    /* 6) 广播设置：可发现 + 可连接 */
    esp_bt_gap_register_callback(bt_gap_cb);
    esp_bt_gap_set_device_name(BT_TEST_DEVICE_NAME);
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

    /* 7) FIFO + 播放任务 */
    s_a2dp_stream = xStreamBufferCreate(BT_TEST_PCM_BUF_BYTES, 1);
    xTaskCreatePinnedToCore(bt_amp_playback_task, "amp_test", 4096, NULL, 9, NULL, 0);

    ESP_LOGI(BT_TEST_TAG, "device name=\"%s\" 等待手机连接...", BT_TEST_DEVICE_NAME);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ------------------------------------------------------------------ */
/* 入口：在 app_main 调用这一句即可                                   */
/* ------------------------------------------------------------------ */
void BtAmpTest_Start(void)
{
    xTaskCreate(bt_sink_test_task, "bt_sink_test", 6144, NULL, 5, NULL);
}