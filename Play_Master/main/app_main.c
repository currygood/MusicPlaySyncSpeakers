/**
 * @file app_main.c
 * @brief 主音频节点应用入口（十三A阶段：最终整合）
 *
 * 启动流程（《主音频节点开发过程》十三A阶段）：
 *   app_main:
 *     1) nvs_flash_init → node_role_init（NVS 唯一入口，WiFi/OTA 参数来源）
 *     2) wifi_manager_create/start → 等待拿到 IP
 *     3) OTA_Update_Init + CheckAndUpgrade（有新固件则下载校验后重启；无则继续）
 *     4) Microphone_Init → Amplifier_Init（麦克风 RX / 功放 TX 物理链路）
 *     5) 串行创建各功能任务（前一个 boot 任务跑完退出后再建下一个，
 *        避免多个 boot 栈同时占内部 DRAM；依赖关系经事件组就绪位衔接）：
 *        MusicPlay    : SD 卡 + 组播通道 + bt_audio + music_play_create（内部建 MusicPlay_Task）
 *        CallPhone    : 等蓝牙就绪 → esp-sr 唤醒词检测（内部建 CallPhone_Task）
 *        LightControl : 等 wifi 就绪 → 灯控组播通道 + light_control_create（内部建 SmartHome_Task）
 *        UI           : 等各句柄就绪 → ui_init（LCD/触摸屏，内部建 UI_Task）
 *
 * 线程/内存约定（见 note/MusicPlay调试笔记.md）：
 *   - main_task 栈仅 3584B，SD/蓝牙/LVGL/esp-sr 等重活一律单独起任务；
 *   - 内部 DRAM 是最稀缺资源（WiFi+BT 已占 ~90KB，见调试笔记 3.1 曲线）：
 *     非 DMA 的大缓冲 / 队列 / 信号量 / 模块句柄一律进 PSRAM，只有任务栈、
 *     中断上下文与 DMA 描述符留内部 RAM；boot 任务用完即退出回收栈；
 *   - 任务间只共享句柄与就绪事件位，模块内部常驻任务由各模块 create 自建。
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_clk_tree.h"
#include "nvs_flash.h"

#include "StartAndStop.h"
#include "node_role.h"
#include "wifi_manager.h"
#include "OTA_Update.h"
#include "microphone.h"
#include "amplifier.h"
#include "sd_card.h"
#include "bt_audio.h"
#include "sync_protocol.h"
#include "music_play.h"
#include "call_phone.h"
#include "light_control.h"
#include "ui.h"

static const char *TAG = "AppMain";

/* ======================== 任务就绪事件位 ======================== */
/* 各模块初始化在独立任务里完成，依赖方用事件组等待上游就绪 */

#define APP_EVT_WIFI_READY   (1 << 0)   /* wifi_manager 已连接并拿到 IP */
#define APP_EVT_BT_READY     (1 << 1)   /* bt_audio 已创建并启动 */
#define APP_EVT_MUSIC_READY  (1 << 2)   /* music_play 已创建 */
#define APP_EVT_CALL_READY   (1 << 3)   /* call_phone 已创建并开始监听 */
#define APP_EVT_LIGHT_READY  (1 << 4)   /* light_control 已创建 */

/* 失败位：供依赖方不再无限等待（值为高位，避免与就绪位混淆） */
#define APP_EVT_BT_FAILED    (1 << 8)
#define APP_EVT_MUSIC_FAILED (1 << 9)
#define APP_EVT_CALL_FAILED  (1 << 10)
#define APP_EVT_LIGHT_FAILED (1 << 11)

/* ======================== 常量 ======================== */

/** 蓝牙广播名（主节点） */
#define APP_BT_DEVICE_NAME      "MPS-Sync-Speaker"

/** 灯控组播通道（分层设计 3.4.2：239.0.0.2:8889，代码常量不入 NVS） */
#define APP_LIGHT_MCAST_GROUP   "239.0.0.2"
#define APP_LIGHT_MCAST_PORT    8889

/** WiFi 等待 IP 超时（轮询 100ms x 200 = 20s） */
#define APP_WIFI_WAIT_TRIES     200

/** 各 boot 任务栈/优先级（mp/cp/lc 创建完即退出；UI 任务常驻，见下）
 *
 *  boot 任务必须高于解码(10)/同步(11)：解码节流在"恰好追平"时余量 <1ms
 *  不再 vTaskDelay（连续不让步），prio 5 的 boot 任务会被饿死——实测表现
 *  为 CallPhone 60s 内既不就绪也不失败，UI 只能陪等满 60s 超时后才出屏 */
#define APP_BOOT_TASK_PRIO      11
#define APP_MUSIC_TASK_STACK    4096
#define APP_CALL_TASK_STACK     4096
#define APP_LIGHT_TASK_STACK    3072
/* UI 任务栈最大，必须趁内部堆还没被 WiFi/BT/其它任务切碎时最先创建；
 * 10240（原 UI_Task 13312）：省 3KB，实际水位由 ui_run() 打印 hwm_words 复核 */
#define APP_UI_TASK_STACK       10240
#define APP_UI_TASK_PRIO        12

/** 依赖等待超时（ms）：上游异常时避免永久挂起 */
#define APP_DEP_WAIT_MS         60000

/** 非关键依赖等待超时（ms）：UI 最多只等这么久，超时按占位降级先出屏
 *  （call_phone / light_control 晚到不影响显示与本地/蓝牙播放控制） */
#define APP_OPT_WAIT_MS         10000

/** 蓝牙就绪等待（ms）：BT 起不来时快速失败，不拖累 CallPhone 的依赖方 */
#define APP_BT_WAIT_MS          15000

/* ======================== 全局运行上下文 ======================== */

/** 各任务共享的句柄与就绪事件组（只写一次、读取方判空） */
typedef struct {
    EventGroupHandle_t     evt;         /* 就绪/失败事件位 */
    wifi_manager_handle_t  wifi;
    bt_audio_handle_t      bt;
    wifi_mcast_handle_t    audio_chan;  /* 音频通道 239.0.0.1:5678（只发） */
    wifi_mcast_handle_t    control_chan;/* 控制通道 239.0.0.1:5679（收发） */
    wifi_mcast_handle_t    light_chan;  /* 灯控通道 239.0.0.2:8889（收发） */
    music_play_handle_t    music;
    call_phone_handle_t    call;
    light_control_handle_t light;
} app_ctx_t;

static app_ctx_t s_app;

/**
 * HFP 下行单声道 → 立体声扩展缓冲（bt_audio 任务上下文，非 ISR/DMA）
 *
 * 内部 DRAM 是最稀缺资源（WiFi + BT 已占 ~90KB）：这类"大数据缓冲"一律放
 * PSRAM，只把任务栈 / DMA 描述符留给内部 RAM。分配失败时回退内部 RAM。
 */
#define APP_HFP_DN_STEREO_SAMPLES  2048
static int16_t *s_hfpDnStereoBuf = NULL;

/* ======================== 通用工具 ======================== */

/**
 * 打印内部 DRAM / DMA 可用内部 RAM / PSRAM 余量（内存紧张时定位用，见调试笔记）
 *
 * "dma" 那一栏最关键：SPI 等外设驱动的收发缓冲只能落在
 * MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL 区，这一栏一旦见底，任何一笔
 * "驱动私有 bounce 缓冲"的分配都会失败（原因见 spi_driver.c 顶部说明）。
 */
static void App_Log_Heap(const char *stage)
{
    ESP_LOGI(TAG, "[heap] %s: int free=%u largest=%u | dma free=%u largest=%u | psram free=%u",
             stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

/**
 * @brief 等待事件位（就绪位或失败位任一即可唤醒），超时仅告警
 *
 * @param bits       关心的位（就绪 | 失败）
 * @param timeout_ms 超时（ms）
 * @param what       日志用名称
 * @return 已置位的事件位
 */
static EventBits_t App_Wait_Bits(EventBits_t bits, uint32_t timeout_ms, const char *what)
{
    int64_t     t0  = esp_timer_get_time();
    EventBits_t got = xEventGroupWaitBits(s_app.evt, bits, pdFALSE, pdFALSE,
                                          pdMS_TO_TICKS(timeout_ms));
    uint32_t    elapsed_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    if ((got & bits) == 0)
    {
        ESP_LOGW(TAG, "wait %s timeout after %u ms (%u ms budget)",
                 what, (unsigned)elapsed_ms, (unsigned)timeout_ms);
    }
    else
    {
        /* 就绪耗时打点：启动期瓶颈定位（哪个模块/哪段等待拖慢了 UI） */
        ESP_LOGI(TAG, "wait %s ok in %u ms (bits=0x%lx)", what,
                 (unsigned)elapsed_ms, (unsigned long)(got & bits));
    }
    return got;
}

/**
 * @brief 创建 boot 引导任务，创建失败必须显式上报
 *
 * 内部 RAM 紧张时 xTaskCreatePinnedToCore 会返回失败；此前未检查返回值，
 * UI 引导任务静默消失（无任何日志），排查代价很高（见十三A记录）。
 * 失败时同步置 FAILED 位，依赖方不必等满超时即可降级继续。
 */
static bool App_Boot_Task_Create(TaskFunction_t entry, const char *name,
                                 uint32_t stack_bytes, UBaseType_t prio, BaseType_t core,
                                 EventBits_t failed_bit)
{
    if (xTaskCreatePinnedToCore(entry, name, stack_bytes, NULL, prio,
                                NULL, core) == pdPASS)
    {
        return true;
    }
    ESP_LOGE(TAG, "[boot] %s create failed (stack=%u, int free=%u largest=%u)",
             name, (unsigned)stack_bytes,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    xEventGroupSetBits(s_app.evt, failed_bit);
    return false;
}

/* ======================== WiFi ======================== */

/** wifi_manager 事件回调：仅打印状态变化 */
static void App_Wifi_Evt_Cb(const wifi_mgr_event_t *evt, void *ctx)
{
    (void)ctx;
    if (evt == NULL)
    {
        return;
    }

    switch (evt->id)
    {
    case WIFI_EVT_CONNECTED:
        ESP_LOGI(TAG, "[wifi] CONNECTED ip=%s rssi=%d", evt->ip, evt->rssi);
        break;
    case WIFI_EVT_DISCONNECTED:
        ESP_LOGW(TAG, "[wifi] DISCONNECTED");
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

/** 初始化 wifi_manager 并等待拿到 IP（凭据由 wifi_manager 经 node_role 读取） */
static void App_Wifi_Start(void)
{
    wifi_manager_cfg_t cfg = {
        .max_retries = 3,   /* 连接失败 3 次后放弃，等用户通过 UI 配网 */
    };
    esp_err_t          err;

    s_app.wifi = wifi_manager_create(&cfg);
    if (s_app.wifi == NULL)
    {
        ESP_LOGE(TAG, "wifi_manager_create failed");
        return;
    }
    wifi_manager_register_event_cb(s_app.wifi, App_Wifi_Evt_Cb, NULL);

    err = wifi_manager_start(s_app.wifi);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "wifi_manager_start failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "connecting to AP, waiting for IP ...");

    for (int i = 0; i < APP_WIFI_WAIT_TRIES; i++)
    {
        if (wifi_manager_is_connected(s_app.wifi))
        {
            char   ip[16] = {0};
            size_t ip_len = sizeof(ip);
            if (wifi_manager_get_ip(s_app.wifi, ip, &ip_len) == ESP_OK)
            {
                ESP_LOGI(TAG, "wifi connected: ip=%s rssi=%d",
                         ip, wifi_manager_get_rssi(s_app.wifi));
            }
            xEventGroupSetBits(s_app.evt, APP_EVT_WIFI_READY);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGE(TAG, "wait IP timeout (请检查 NVS 中的 SSID/密码)");
}

/* ======================== OTA ======================== */

static const char *App_Ota_Evt_Name(ota_update_event_t evt)
{
    switch (evt)
    {
    case OTA_UPDATE_EVT_CHECK_STARTED:     return "CHECK_STARTED";
    case OTA_UPDATE_EVT_UP_TO_DATE:        return "UP_TO_DATE";
    case OTA_UPDATE_EVT_UPDATE_AVAILABLE:  return "UPDATE_AVAILABLE";
    case OTA_UPDATE_EVT_DOWNLOAD_STARTED:  return "DOWNLOAD_STARTED";
    case OTA_UPDATE_EVT_DOWNLOAD_PROGRESS: return "DOWNLOAD_PROGRESS";
    case OTA_UPDATE_EVT_DOWNLOAD_FINISHED: return "DOWNLOAD_FINISHED";
    case OTA_UPDATE_EVT_READY_TO_REBOOT:   return "READY_TO_REBOOT";
    case OTA_UPDATE_EVT_ABORTED:           return "ABORTED";
    default:                               return "?";
    }
}

/** OTA 事件回调：打印状态/进度（正式 UI 展示后续接入） */
static void App_Ota_Evt_Cb(const ota_update_event_info_t *info, void *ctx)
{
    (void)ctx;
    if (info == NULL)
    {
        return;
    }
    ESP_LOGI(TAG, "[ota] %s version=%s progress=%d%% http=%d",
             App_Ota_Evt_Name(info->event), info->version,
             info->progress_percent, info->http_status);
}

/**
 * @brief WiFi 就绪后检查是否需要 OTA 升级
 *
 * 有新固件：模块内部下载→校验 SHA256→切换分区→esp_restart()（不返回）；
 * 已是最新或检测失败：记录日志后继续往下启动。
 */
static void App_Ota_Check(void)
{
    ota_update_config_t cfg = {0};
    esp_err_t           err;

    cfg.event_cb = App_Ota_Evt_Cb;
    err = OTA_Update_Init(&cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "OTA_Update_Init failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "checking OTA ...");
    err = OTA_Update_CheckAndUpgrade();
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "OTA: firmware up to date, continue boot");
    }
    else
    {
        ESP_LOGW(TAG, "OTA check failed (%s), continue boot", esp_err_to_name(err));
    }
}

/* ======================== 蓝牙 PCM / 事件胶水 ======================== */

/**
 * @brief bt_audio PCM 回调（bt_audio 任务上下文）
 *
 *   - A2DP 音乐：直连 sync_protocol_master_push_pcm()（不经过 MusicPlay），
 *     由 sync_protocol 统一打时间戳、组播并本地延迟播放；
 *   - HFP 下行（语音助手应答）：44.1kHz 单声道，而功放总线为立体声，
 *     先扩展为 L/R 相同再交功放（见 note/HFP下行无声-声道与音量bug.md）。
 */
static void App_Bt_Pcm_Cb(bt_audio_pcm_source_t source, const int16_t *pcm,
                          size_t samples, uint32_t sample_rate, void *user_ctx)
{
    (void)sample_rate;
    (void)user_ctx;

    if (pcm == NULL || samples == 0)
    {
        return;
    }

    if (source == BT_AUDIO_PCM_SOURCE_A2DP)
    {
        sync_protocol_master_push_pcm(pcm, samples);
        return;
    }

    if (source == BT_AUDIO_PCM_SOURCE_HFP_DOWNLINK)
    {
        if (s_hfpDnStereoBuf == NULL)
        {
            return;
        }
        if (samples > APP_HFP_DN_STEREO_SAMPLES)
        {
            ESP_LOGW(TAG, "HFP mono chunk too large: %u", (unsigned)samples);
            return;
        }
        for (size_t i = 0; i < samples; i++)
        {
            s_hfpDnStereoBuf[i * 2]     = pcm[i];
            s_hfpDnStereoBuf[i * 2 + 1] = pcm[i];
        }
        Amplifier_Play_Buffer((const uint8_t *)s_hfpDnStereoBuf,
                              samples * 2 * sizeof(int16_t), NULL, 200);
    }
}

/** bt_audio 事件回调：全部交给 MusicPlay（MusicPlay 未就绪前忽略） */
static void App_Bt_Evt_Cb(const bt_audio_event_t *evt, void *user_ctx)
{
    (void)user_ctx;
    if (evt == NULL)
    {
        return;
    }
    if (s_app.music != NULL)
    {
        music_play_on_bt_event(s_app.music, evt);
    }
}

/* ======================== 各功能 boot 任务 ======================== */

/**
 * @brief MusicPlay 任务：SD 卡 + 组播通道 + 蓝牙 + music_play_create
 *
 * 蓝牙句柄由本任务创建并经全局上下文共享给 CallPhone（唤醒后 HFP 上行）。
 */
static void App_MusicPlay_Task(void *arg)
{
    (void)arg;
    music_play_cfg_t mcfg = {0};
    wifi_mcast_cfg_t mc   = {0};
    bt_audio_cfg_t   bcfg = {0};
    bool             bt_ready = false;
    esp_err_t        err;

    ESP_LOGI(TAG, "[MusicPlay] boot: SD card / mcast / bt_audio / music_play");

    /* 1) SD 卡（本地播放依赖；失败仅告警，蓝牙源仍可用） */
    if (SD_Card_Init() != ESP_OK)
    {
        ESP_LOGW(TAG, "[MusicPlay] SD_Card_Init failed, 本地列表将为空");
    }

    /* 2) 通道：音频通道只用来**单播**发数据（绑定本机端口，不收）；
     *    控制走组播 239.0.0.1:5679，收发 */
    mc.group         = SYNC_PROTOCOL_MCAST_GROUP;
    mc.port          = SYNC_UNICAST_PORT_AUDIO;
    mc.rx_enable     = false;
    err = wifi_manager_mcast_open(s_app.wifi, &mc, &s_app.audio_chan);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "[MusicPlay] mcast_open audio failed: %s", esp_err_to_name(err));
        goto fail;
    }

    mc.group         = SYNC_PROTOCOL_MCAST_GROUP;
    mc.port          = SYNC_PROTOCOL_MCAST_PORT_CTRL;
    mc.rx_enable     = true;
    mc.rx_fifo_bytes = 4096;
    err = wifi_manager_mcast_open(s_app.wifi, &mc, &s_app.control_chan);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "[MusicPlay] mcast_open ctrl failed: %s", esp_err_to_name(err));
        goto fail;
    }

    /* 3) 蓝牙：on_pcm 直连 sync，on_event 胶水进 MusicPlay */
    bcfg.device_name = APP_BT_DEVICE_NAME;
    bcfg.on_pcm      = App_Bt_Pcm_Cb;
    bcfg.on_event    = App_Bt_Evt_Cb;
    s_app.bt = bt_audio_create(&bcfg);
    if (s_app.bt == NULL)
    {
        ESP_LOGE(TAG, "[MusicPlay] bt_audio_create failed");
        goto fail;
    }
    err = bt_audio_start(s_app.bt);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "[MusicPlay] bt_audio_start failed: %s", esp_err_to_name(err));
        goto fail;
    }

    /* 蓝牙就绪：CallPhone（esp-sr）依赖 bt_audio 句柄 */
    bt_ready = true;
    xEventGroupSetBits(s_app.evt, APP_EVT_BT_READY);
    App_Log_Heap("after bt_audio");

    /* 4) MusicPlay：内部自行 init/deinit sync_protocol（D=0 从 node_role 读） */
    mcfg.bt_audio      = s_app.bt;
    mcfg.audio_chan    = s_app.audio_chan;
    mcfg.control_chan  = s_app.control_chan;
    mcfg.sync_delay_ms = 0;
    mcfg.on_event      = ui_on_music_event;
    s_app.music = music_play_create(&mcfg);
    if (s_app.music == NULL)
    {
        ESP_LOGE(TAG, "[MusicPlay] music_play_create failed");
        goto fail;
    }

    /* 5) 开机即扫描本地列表（自动源选择未连蓝牙时落本地源） */
    music_play_scan_local(s_app.music);

    xEventGroupSetBits(s_app.evt, APP_EVT_MUSIC_READY);
    ESP_LOGI(TAG, "[MusicPlay] boot done");
    vTaskDelete(NULL);
    return;

fail:
    if (!bt_ready)
    {
        xEventGroupSetBits(s_app.evt, APP_EVT_BT_FAILED);
    }
    xEventGroupSetBits(s_app.evt, APP_EVT_MUSIC_FAILED);
    vTaskDelete(NULL);
}

/**
 * @brief CallPhone 任务：等蓝牙就绪 → esp-sr 唤醒词检测（麦克风上行）
 *
 * 先等 MusicPlay 完成蓝牙初始化（拿到 bt_audio 句柄），再创建 esp-sr
 * 唤醒检测链路；麦克风 FIFO 由 app_main 的 Microphone_Init() 提前备好。
 */
static void App_CallPhone_Task(void *arg)
{
    (void)arg;
    call_phone_cfg_t ccfg = {0};
    EventBits_t      bits;

    ESP_LOGI(TAG, "[CallPhone] boot: waiting for bluetooth ready ...");
    bits = App_Wait_Bits(APP_EVT_BT_READY | APP_EVT_BT_FAILED,
                         APP_BT_WAIT_MS, "bluetooth ready");

    if (s_app.bt == NULL || (bits & APP_EVT_BT_READY) == 0)
    {
        ESP_LOGE(TAG, "[CallPhone] bluetooth not ready, skip esp-sr init");
        xEventGroupSetBits(s_app.evt, APP_EVT_CALL_FAILED);
        vTaskDelete(NULL);
        return;
    }

    ccfg.audio       = s_app.bt;
    ccfg.sample_rate = 44100;   /* 全链路固定 44.1kHz，模块内重采样到 16k */

    s_app.call = call_phone_create(&ccfg);
    if (s_app.call == NULL)
    {
        ESP_LOGE(TAG, "[CallPhone] call_phone_create failed");
        xEventGroupSetBits(s_app.evt, APP_EVT_CALL_FAILED);
        vTaskDelete(NULL);
        return;
    }

    if (call_phone_start_listening(s_app.call) != ESP_OK)
    {
        ESP_LOGE(TAG, "[CallPhone] start_listening failed");
        xEventGroupSetBits(s_app.evt, APP_EVT_CALL_FAILED);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "[CallPhone] boot done (唤醒词: 你好小智)");
    xEventGroupSetBits(s_app.evt, APP_EVT_CALL_READY);
    vTaskDelete(NULL);
}

/**
 * @brief LightControl 任务：等 wifi_manager 就绪 → 灯控组播通道 + 灯控模块
 *
 * 灯控通道（239.0.0.2:8889）由 App 经 wifi_manager 打开并传入；上行 ack/status
 * 经 ui_on_light_event 通知 UI 刷新灯图标。
 */
static void App_LightControl_Task(void *arg)
{
    (void)arg;
    wifi_mcast_cfg_t    mc   = {0};
    light_control_cfg_t lcfg = {0};
    esp_err_t           err;

    ESP_LOGI(TAG, "[LightControl] boot: waiting for wifi ready ...");
    App_Wait_Bits(APP_EVT_WIFI_READY, APP_DEP_WAIT_MS, "wifi ready");

    if (s_app.wifi == NULL || !wifi_manager_is_connected(s_app.wifi))
    {
        ESP_LOGE(TAG, "[LightControl] wifi not ready, skip");
        xEventGroupSetBits(s_app.evt, APP_EVT_LIGHT_FAILED);
        vTaskDelete(NULL);
        return;
    }

    mc.group         = APP_LIGHT_MCAST_GROUP;
    mc.port          = APP_LIGHT_MCAST_PORT;
    mc.rx_enable     = true;
    mc.rx_fifo_bytes = 4096;
    err = wifi_manager_mcast_open(s_app.wifi, &mc, &s_app.light_chan);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "[LightControl] mcast_open failed: %s", esp_err_to_name(err));
        xEventGroupSetBits(s_app.evt, APP_EVT_LIGHT_FAILED);
        vTaskDelete(NULL);
        return;
    }

    lcfg.chan     = s_app.light_chan;
    lcfg.on_event = ui_on_light_event;
    s_app.light = light_control_create(&lcfg);
    if (s_app.light == NULL)
    {
        ESP_LOGE(TAG, "[LightControl] light_control_create failed");
        xEventGroupSetBits(s_app.evt, APP_EVT_LIGHT_FAILED);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "[LightControl] boot done");
    xEventGroupSetBits(s_app.evt, APP_EVT_LIGHT_READY);
    vTaskDelete(NULL);
}

/**
 * @brief UI 任务：等各模块句柄就绪 → ui_init（LCD / 触摸屏 / LVGL）
 *
 * 未就绪的句柄保持 NULL，UI 按"占位"降级显示；ui_init 内部自建 UI_Task。
 */
static void App_UI_Task(void *arg)
{
    (void)arg;
    ui_cfg_t  ucfg = {0};
    esp_err_t err;

    ESP_LOGI(TAG, "[UI] boot: waiting for module handles ...");
    /* 逐个等 READY|FAILED：某模块挂了立刻继续，UI 按占位降级显示；
     * 若只等 READY 位，CallPhone 失败会空等满超时导致长时间黑屏 */
    App_Wait_Bits(APP_EVT_MUSIC_READY | APP_EVT_MUSIC_FAILED,
                  APP_DEP_WAIT_MS, "music_play ready");
    App_Wait_Bits(APP_EVT_CALL_READY | APP_EVT_CALL_FAILED,
                  APP_OPT_WAIT_MS, "call_phone ready");
    App_Wait_Bits(APP_EVT_LIGHT_READY | APP_EVT_LIGHT_FAILED,
                  APP_OPT_WAIT_MS, "light_control ready");

    ucfg.wifi       = s_app.wifi;
    ucfg.light      = s_app.light;
    ucfg.call_phone = s_app.call;
    ucfg.music      = s_app.music;

    App_Log_Heap("before ui_init");
    err = ui_init(&ucfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "[UI] ui_init failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    App_Log_Heap("after ui_init");
    ESP_LOGI(TAG, "[UI] boot done");
    /* UI 任务常驻：进入主题/页面构建 + LVGL 渲染循环（不再返回） */
    ui_run();
}

/* ======================== 应用入口 ======================== */

void app_main(void)
{
    esp_err_t      err;
    node_role_cfg_t nr = {0};

    SystemStart();
    StartAndStop_Init();

    /* 1. NVS：node_role / wifi_manager / ota_manager 的持久化基础 */
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    if (esp_psram_is_initialized())
    {
        ESP_LOGI(TAG, "PSRAM ready, size=%u bytes", (unsigned)esp_psram_get_size());
    }
    else
    {
        ESP_LOGW(TAG, "PSRAM not initialized");
    }

    /* CPU / Flash 频率打点：解码吞吐（MP3 ~1x@160MHz）与 UI 流畅度直接相关 */
    {
        uint32_t cpu_hz = 0;
        if (esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_CPU,
                                         ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED,
                                         &cpu_hz) != ESP_OK)
        {
            cpu_hz = 0;
        }
        ESP_LOGI(TAG, "clk: cpu=%u MHz flash=%s psram=%d MHz",
                 (unsigned)(cpu_hz / 1000000), CONFIG_ESPTOOLPY_FLASHFREQ,
                 CONFIG_SPIRAM_SPEED);
    }

    /* 2. node_role：wifi_manager 启动时经 node_role_get() 读取 WiFi 凭据 */
    ESP_ERROR_CHECK(node_role_init(NULL));

    /* 3. 就绪事件组：各模块 boot 任务与依赖方同步用 */
    s_app.evt = xEventGroupCreate();
    if (s_app.evt == NULL)
    {
        ESP_LOGE(TAG, "event group create failed");
        return;
    }

    App_Log_Heap("boot start");

    /* 4. WiFi：连接热点并等待拿到 IP */
    App_Wifi_Start();
    App_Log_Heap("after wifi");

    /* 5. OTA：有新固件则升级重启，否则继续 */
    App_Ota_Check();
    App_Log_Heap("after ota");

    /* 6. 麦克风（RX）与功放（TX）：物理音频链路 */
    if (Microphone_Init() != ESP_OK)
    {
        ESP_LOGE(TAG, "Microphone_Init failed (语音唤醒不可用)");
    }
    if (Amplifier_Init() != ESP_OK)
    {
        ESP_LOGE(TAG, "Amplifier_Init failed (无音频输出)");
    }
    if (node_role_get(&nr) == ESP_OK)
    {
        Amplifier_Set_Volume(nr.volume);   /* MusicPlay create 时还会再同步一次 */
    }
    App_Log_Heap("after mic/amp");

    /* 7. HFP 下行立体声扩展缓冲：非 DMA，放 PSRAM（内部 RAM 留给栈/DMA） */
    s_hfpDnStereoBuf = (int16_t *)heap_caps_malloc(
        APP_HFP_DN_STEREO_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_hfpDnStereoBuf == NULL)
    {
        s_hfpDnStereoBuf = (int16_t *)heap_caps_malloc(
            APP_HFP_DN_STEREO_SAMPLES * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (s_hfpDnStereoBuf == NULL)
    {
        ESP_LOGW(TAG, "HFP stereo expand buffer alloc failed (通话下行无声)");
    }

    /*
     * 8. 创建功能任务：UI 任务最先建 —— 它的栈最大，必须趁内部堆还没被
     *    WiFi/BT/其它任务切碎时把这块连续内存占住（创建后立即阻塞等各模块
     *    就绪，不参与启动期 CPU 竞争）。其余模块仍严格串行：前一个 boot
     *    任务跑完并退出后再建下一个；依赖 MusicPlay（建蓝牙）→ CallPhone
     *    （等蓝牙）→ LightControl（等 WiFi）。
     */
    (void)App_Boot_Task_Create(App_UI_Task, "UI_Task",
                               APP_UI_TASK_STACK, APP_UI_TASK_PRIO, 1, 0);

    (void)App_Boot_Task_Create(App_MusicPlay_Task, "mp_boot",
                               APP_MUSIC_TASK_STACK, APP_BOOT_TASK_PRIO, 1,
                               APP_EVT_MUSIC_FAILED);
    App_Wait_Bits(APP_EVT_MUSIC_READY | APP_EVT_MUSIC_FAILED,
                  APP_DEP_WAIT_MS, "music_play ready");
    vTaskDelay(pdMS_TO_TICKS(50));   /* 让 IDLE 回收 mp_boot 栈，再建下一个 */
    App_Log_Heap("after mp_boot");

    (void)App_Boot_Task_Create(App_CallPhone_Task, "cp_boot",
                               APP_CALL_TASK_STACK, APP_BOOT_TASK_PRIO, 1,
                               APP_EVT_CALL_FAILED);
    App_Wait_Bits(APP_EVT_CALL_READY | APP_EVT_CALL_FAILED,
                  APP_DEP_WAIT_MS, "call_phone ready");
    vTaskDelay(pdMS_TO_TICKS(50));
    App_Log_Heap("after cp_boot");

    (void)App_Boot_Task_Create(App_LightControl_Task, "lc_boot",
                               APP_LIGHT_TASK_STACK, APP_BOOT_TASK_PRIO, 0,
                               APP_EVT_LIGHT_FAILED);
    App_Wait_Bits(APP_EVT_LIGHT_READY | APP_EVT_LIGHT_FAILED,
                  APP_DEP_WAIT_MS, "light_control ready");
    vTaskDelay(pdMS_TO_TICKS(50));
    App_Log_Heap("after lc_boot");

    ESP_LOGI(TAG, "all boot tasks dispatched");

    /* 9. 新固件确认运行正常：取消自动回滚（仅 CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE 时生效） */
    if (OTA_Update_MarkAppValid() == ESP_OK)
    {
        ESP_LOGI(TAG, "app marked valid (rollback cancelled)");
    }

    /* app_main 返回后 IDF 主任务自删，回收其约 4KB 内部栈（内部 RAM 紧张时可观） */
	vTaskDelete(NULL);
	return;
}