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
#include "sync_protocol.h"   /* 音频同步协议（从节点侧） */
#include "debug.h"           /* 调试 / 自检函数（默认不调用） */

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

/**
 * @brief 打开两个组播通道（音频 5678 只收；控制 5679 收发，PING 从这里出去）
 */
static esp_err_t open_mcast_channels(wifi_manager_handle_t wifi,
                                     const char *group,
                                     wifi_mcast_handle_t *audio_out,
                                     wifi_mcast_handle_t *ctrl_out)
{
    wifi_mcast_cfg_t mc;
    esp_err_t        ret;
    int              attempt;

    /* 拿到 IP 之前 open 可能失败（要绑定本地接口），所以带重试 */
    for (attempt = 0; attempt < 3; attempt++)
    {
        memset(&mc, 0, sizeof(mc));
        mc.group          = group;
        mc.port           = SYNC_PROTOCOL_MCAST_PORT_AUDIO;
        mc.rx_enable      = true;
        mc.rx_fifo_bytes  = 16384;      /* 15ms 一包 ≈ 1340B，16KB ≈ 12 包余量 */
        ret = wifi_manager_mcast_open(wifi, &mc, audio_out);
        if (ret == ESP_OK)
        {
            break;
        }
        ESP_LOGW(TAG, "open audio mcast failed (%s), retry %d/3",
                 esp_err_to_name(ret), attempt + 1);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "音频组播通道 %s:%d 打开失败：%s",
                 group, SYNC_PROTOCOL_MCAST_PORT_AUDIO, esp_err_to_name(ret));
        return ret;
    }

    memset(&mc, 0, sizeof(mc));
    mc.group         = group;
    mc.port          = SYNC_PROTOCOL_MCAST_PORT_CTRL;
    mc.rx_enable     = true;
    mc.rx_fifo_bytes = 4096;
    ret = wifi_manager_mcast_open(wifi, &mc, ctrl_out);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "控制组播通道 %s:%d 打开失败：%s",
                 group, SYNC_PROTOCOL_MCAST_PORT_CTRL, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "mcast open: %s:%d (audio) / %s:%d (control)",
             group, SYNC_PROTOCOL_MCAST_PORT_AUDIO, group, SYNC_PROTOCOL_MCAST_PORT_CTRL);
    return ESP_OK;
}

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
    ESP_LOGI(TAG, "node: role=%u ssid=%s vol=%u%% D=%ums group=%s",
             nr.role, nr.wifi_ssid, nr.volume,
             (unsigned)(nr.sync_delay_ms ? nr.sync_delay_ms : NODE_ROLE_DEFAULT_SYNC_DELAY_MS),
             nr.multicast_group[0] ? nr.multicast_group : SYNC_PROTOCOL_MCAST_GROUP);

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
                      "连上后组播通道内部自动重建");
    }

    /* 6. 两个组播通道（socket 由 wifi_manager 持有，模块内自动跟随重连重建） */
    const char *group = nr.multicast_group[0] ? nr.multicast_group
                                              : SYNC_PROTOCOL_MCAST_GROUP;
    wifi_mcast_handle_t audio_chan = NULL;
    wifi_mcast_handle_t ctrl_chan  = NULL;
    if (open_mcast_channels(wifi, group, &audio_chan, &ctrl_chan) != ESP_OK)
    {
        ESP_LOGE(TAG, "组播通道不可用，音频同步链路未启动");
        goto idle;
    }

    /* 7. 功放 + 软件音量（NS4168 无硬件增益，音量靠衰减 PCM 实现） */
    if (Amplifier_Init() != ESP_OK)
    {
        ESP_LOGE(TAG, "Amplifier_Init failed");
        goto idle;
    }
    Amplifier_Set_Volume(nr.volume);

    /* 8. 音频同步（从节点侧）：起接收/心跳/播放三个任务 */
    sync_protocol_slave_cfg_t scfg = {
        .sync_delay_ms   = nr.sync_delay_ms ? nr.sync_delay_ms
                                            : NODE_ROLE_DEFAULT_SYNC_DELAY_MS,
        .ping_interval_ms = 1000,
        .jitter_slots     = 24,          /* 24×15ms = 360ms */
        .write_ahead_us   = 0,           /* 即时写：写时刻即出声时刻（见 sync_protocol.c） */
        .audio_chan       = audio_chan,
        .control_chan     = ctrl_chan,
    };
    if (sync_protocol_slave_init(&scfg) != ESP_OK)
    {
        ESP_LOGE(TAG, "sync_protocol_slave_init failed");
        goto idle;
    }
    ESP_ERROR_CHECK(sync_protocol_slave_start());

idle:
    /* app_main 返回后任务被删除，同步任务必须已经跑起来 —— 主循环只负责挂着 */
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
