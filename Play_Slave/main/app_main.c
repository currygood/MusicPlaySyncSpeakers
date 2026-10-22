#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "nvs_flash.h"

#include "StartAndStop.h"    /* 开关机（PMOS 自锁供电） */
#include "StatusLed.h"       /* 状态 LED */
#include "node_role.h"       /* NVS 运行参数（NVS 唯一入口） */
#include "wifi_manager.h"    /* STA 连接 + 统一 UDP 组播通道 */
#include "amplifier.h"       /* NS4168 功放 */
#include "udp_audio.h"       /* 最小 UDP 音频接收播放（MVP，当前链路） */
#include "debug.h"           /* 调试 / 自检函数（默认不调用） */

/* MVP 阶段只跑 udp_audio 这一条路径；从节点工程里未使用的 sync_protocol
 * 已删除（2026-10-22，历史见 git）。主节点工程的同名模块是 v2 单播发送端。 */

static const char *TAG = "MAIN";

/**
 * @brief 等待 WiFi 拿到 IP
 * @return true 已连接；false 超时（wifi_manager 仍会后台重连）
 */
static bool wait_wifi_ip(wifi_manager_handle_t wifi, uint32_t timeout_ms)
{
    uint32_t waited = 0;

    while (!wifi_manager_is_connected(wifi) && waited < timeout_ms)
    {
        vTaskDelay(pdMS_TO_TICKS(500));
        waited += 500;
    }
    return wifi_manager_is_connected(wifi);
}

/** 开机自检"滴"一声 —— 用来把"功放到底响不响"和 UDP 链路**分开**
 *
 *  它绕开网络，直接调 Amplifier_Play_Buffer()，所以只有两种结果：
 *    响   → 功放 / I2S / 喇叭 / 供电都正常，问题在收包那一侧
 *    不响 → 问题在功放供电或接线，跟 UDP 代码没有关系
 *           （见《功放模块独立供电》那篇：UVLO 欠压保护是静默的，不报错）
 *
 *  2026-10-18：靠它确认了功放供电没接 —— 接上之后 UDP 链路立刻出声。
 *  默认关掉，下次"收到了但没声音"时再临时打开，一分钟就能分清是硬件还是代码。 */
#define APP_BOOT_BEEP   (0)

#if APP_BOOT_BEEP
static void app_boot_beep(void)
{
    int16_t    chunk[512];
    const int  period = 44100 / 800;    /* 55 采样/周期 */
    const int  half   = period / 2;     /* 27 采样/半周期 */
    int        phase  = 0;

    for (int c = 0; c < 26; c++)        /* 26 × 11.6ms ≈ 300ms */
    {
        for (int i = 0; i < 512; i++)
        {
            chunk[i] = ((phase % period) < half) ? 6000 : -6000;
            phase++;
        }
        Amplifier_Play_Buffer((const uint8_t *)chunk, sizeof(chunk),
                              NULL, 200);
    }
}
#endif

void app_main(void)
{
    /* 1. 先锁住电源：板子靠 PMOS 自锁供电，不尽早拉高，松手就断电 */
    StartAndStop_Init();
    SystemStart();

    /* 2. PSRAM：Jitter Buffer、音量工作缓冲都指望它 */
    if (esp_psram_is_initialized())
    {
        printf("PSRAM 初始化成功！可用大小: %u 字节\n", (unsigned)esp_psram_get_size());
    }
    else
    {
        printf("PSRAM 未找到或未初始化！\n");
    }

    /* 调试 / 自检（实现在 debug.c）：默认全部注释掉，需要时放开一行。
     * 两者都会独占硬件资源（SPI2 总线 / 功放），别在同步链路跑起来之后调用。 */
    // Debug_SdRawTest();   /* SD 卡裸扇区回环 —— ⚠️ 探测点含扇区 0/1（MBR/分区表） */
    // Debug_BeepTest();    /* 5 秒"嘟嘟"提示音，脱网自检功放 + I2S 通路 */

    /* 3. NVS → 节点参数（wifi_manager 启动时会来读 WiFi 凭据） */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(TAG, "NVS 需要重建：%s", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(node_role_init(NULL));

    node_role_cfg_t nr;
    if (node_role_get(&nr) != ESP_OK)
    {
        memset(&nr, 0, sizeof(nr));
        nr.volume        = NODE_ROLE_DEFAULT_VOLUME;
        nr.sync_delay_ms = NODE_ROLE_DEFAULT_SYNC_DELAY_MS;
    }
    ESP_LOGI(TAG, "node: role=%u ssid=%s vol=%u%% D=%ums",
             nr.role, nr.wifi_ssid, nr.volume,
             (unsigned)(nr.sync_delay_ms ? nr.sync_delay_ms : NODE_ROLE_DEFAULT_SYNC_DELAY_MS));

    /* 4. 状态 LED（本阶段只是初始化到确定状态，播放联动留待后续） */
    StatusLed_Init();

    /* 5. WiFi（STA，断线自动重连） */
    wifi_manager_cfg_t wcfg = {
        .connect_timeout_ms    = 10000,
        .reconnect_interval_ms = 3000,
        .max_retries           = 0,      /* 0 = 无限重连 */
    };
    wifi_manager_handle_t wifi = wifi_manager_create(&wcfg);
    if (wifi == NULL)
    {
        ESP_LOGE(TAG, "wifi_manager_create failed");
        return;
    }
    ESP_ERROR_CHECK(wifi_manager_start(wifi));

    if (wait_wifi_ip(wifi, 30000))
    {
        char   ip[16] = {0};
        size_t iplen  = sizeof(ip);
        wifi_manager_get_ip(wifi, ip, &iplen);
        ESP_LOGI(TAG, "wifi connected: ip=%s rssi=%d", ip, wifi_manager_get_rssi(wifi));
    }
    else
    {
        ESP_LOGW(TAG, "30s 内没拿到 IP，继续往下走：wifi_manager 会一直重连，"
                      "连上后 UDP 接收任务自己会建 socket");
    }

    /* 6. 功放 + 软件音量（NS4168 无硬件增益，音量靠衰减 PCM 实现）
     *    必须先于 udp_audio_start：接收任务收到包就要往这里写。 */
    if (Amplifier_Init() != ESP_OK)
    {
        ESP_LOGE(TAG, "Amplifier_Init failed");
        goto idle;
    }
    Amplifier_Set_Volume(nr.volume);

#if APP_BOOT_BEEP
    ESP_LOGI(TAG, "boot beep: 如果这一声听不到，先把功放供电查清楚再看 UDP");
    app_boot_beep();
#endif

    /* 7. 接收链路：单播 UDP → jitter buffer → 按 t_play_us 对齐出声。
     *    数据面只走单播；时钟偏移由"最小单程延迟"单向估计，从节点不回包。 */
    ESP_ERROR_CHECK(udp_audio_start(UDIO_DEFAULT_PORT));

    {
        char   ip[16] = {0};
        size_t iplen  = sizeof(ip);
        wifi_manager_get_ip(wifi, ip, &iplen);
        ESP_LOGI(TAG, "audio ready: 发送到 udp://%s:%d (proto v%d)",
                 ip[0] ? ip : "<slave-ip>", UDIO_DEFAULT_PORT, UDIO_VERSION);
    }

idle:
    /* app_main 返回后任务被删除，同步任务必须已经跑起来 —— 主循环只负责挂着 */
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
