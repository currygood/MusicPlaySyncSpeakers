/**
 * @file wifi_manager.c
 * @brief WiFi 系统服务模块实现：STA 连接/自动重连/配网 + 统一 UDP 组播通道
 *
 * 实现要点：
 *   - 事件驱动状态机：连接/断开/重连由 esp_event（WIFI_EVENT / IP_EVENT）
 *     与 esp_timer（连接超时、重连调度）驱动，不额外创建连接控制任务；
 *   - 组播收包任务 wifi_mcast_rx_Task 按分层设计第 6 节任务表创建：
 *     优先级 6、栈 4096、Core 0，rx 通道收包入 FIFO（ringbuf）；
 *   - 凭据：start 时经 node_role_get() 读取（node_role 为 NVS 唯一管理入口）；
 *     NVS 未配网（ssid 为空）时回退使用本文件头兜底宏，本模块不直接写 NVS；
 *   - 组播：每个通道一个 UDP socket（bind + IP_ADD_MEMBERSHIP 加组），
 *     WiFi 重连 / IP 变化后由模块内部整体重建并重新加组，业务模块无感知。
 *
 * 接口依据（esp-idf 6.0.1）：
 *   - esp_wifi.h / esp_wifi_types_generic.h：STA 配置、连接、事件枚举；
 *   - esp_netif.h / esp_netif_types.h：IP 事件；
 *   - freertos/ringbuf.h：接收 FIFO；
 *   - lwip/sockets.h 组播用法参考官方示例
 *     examples/protocols/sockets/udp_multicast（IP_ADD_MEMBERSHIP 等）。
 */

#include "wifi_manager.h"
#include "node_role.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "nvs_flash.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"

#define TAG "wifi_manager"

/* ======================== 默认参数 ======================== */

/** 单次连接超时（含 DHCP 拿 IP），cfg 为 0 时的默认值，单位 ms */
#define WIFI_MGR_CFG_DEFAULT_CONNECT_TIMEOUT_MS     (10000)

/** 断后自动重连间隔，cfg 为 0 时的默认值，单位 ms */
#define WIFI_MGR_CFG_DEFAULT_RECONNECT_INTERVAL_MS  (3000)

/** 连续失败上限，cfg 为 0 时 = 无限重连 */
#define WIFI_MGR_CFG_DEFAULT_MAX_RETRIES            (0)

/** 组播接收 FIFO 默认大小（cfg->rx_fifo_bytes 为 0 时使用） */
#define WIFI_MCAST_RX_FIFO_DEFAULT_BYTES            (16384)

/** RX 任务单次 recvfrom 缓冲（灯控 JSON / 同步控制帧足够） */
#define WIFI_MCAST_RX_BUF_BYTES                     (4096)

/** wifi_mcast_rx_Task：优先级/栈/核（对应分层设计任务表） */
#define WIFI_MCAST_RX_TASK_PRIO                     (6)
#define WIFI_MCAST_RX_TASK_STACK                    (4096)
#define WIFI_MCAST_RX_TASK_CORE                     (0)

/** RX 任务轮询间隔（ms） */
#define WIFI_MCAST_RX_POLL_MS                       (2)

/** 组播组地址字符串最大长度（含结束符） */
#define WIFI_MCAST_GROUP_LEN                        (16)

/** RSSI 无效值（未连接/查询失败时） */
#define WIFI_RSSI_INVALID                           ((int8_t)-127)

/* ======================== 结构体 ======================== */

struct wifi_mcast_s {
    struct wifi_mcast_s *next;      /* 通道链表（挂在 wifi_manager 上） */
    wifi_manager_handle_t owner;    /* 所属 wifi_manager 实例 */

    char     group[WIFI_MCAST_GROUP_LEN];
    uint16_t port;
    bool     rx_enable;
    uint32_t rx_fifo_bytes;

    int             fd;           /* UDP socket，-1 = WiFi 未连接/未建立 */
    RingbufHandle_t rx_fifo;      /* rx_enable 时的接收 FIFO */
    uint32_t        rx_dropped;   /* FIFO 满丢弃统计（调试） */
};

struct wifi_manager_s {
    wifi_manager_cfg_t cfg;
    SemaphoreHandle_t  lock;      /* 保护 state/凭据/IP/通道链表/fd */

    wifi_state_t state;
    char ssid[33];                /* 配网用，最长 32 + '\0' */
    char password[65];            /* 最长 64 + '\0' */
    char ip[16];                  /* 当前 IP 字符串，未连接 "0.0.0.0" */
    int8_t rssi;                  /* 最近一次查询的 rssi */

    bool running;                 /* start 后为 true，stop/destroy 置 false */
    bool wifi_inited;             /* 驱动已 init（start 内一次），destroy 前 deinit */
    bool netif_created;           /* 默认 STA netif 已创建 */

    uint32_t retry_count;         /* 连续失败次数（连上清零） */

    wifi_event_cb_t event_cb;
    void           *event_ctx;

    esp_timer_handle_t connect_timer;   /* 单次连接超时 */
    esp_timer_handle_t reconnect_timer; /* 重连调度 */

    esp_event_handler_instance_t wifi_evt_inst;
    esp_event_handler_instance_t ip_evt_inst;

    struct wifi_mcast_s *mcast_list;
    TaskHandle_t         rx_task;
    bool                 rx_stop;   /* destroy() 置位，RX 任务自行退出 */
    SemaphoreHandle_t    rx_done;   /* RX 任务退出信号（destroy 等待） */
};

/* ======================== 前置声明 ======================== */

static void wifi_mgr_handle_wifi(void *arg, esp_event_base_t base,
                                 int32_t event_id, void *event_data);
static void wifi_mgr_handle_ip(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data);
static esp_err_t wifi_mgr_apply_config(wifi_manager_handle_t h);
static void wifi_mgr_connect(wifi_manager_handle_t h);
static void wifi_mgr_arm_connect_timeout(wifi_manager_handle_t h);
static void wifi_mgr_schedule_reconnect(wifi_manager_handle_t h);
static void wifi_mgr_fire_event(wifi_manager_handle_t h, wifi_event_id_t id);
static void wifi_mgr_close_all_mcast(wifi_manager_handle_t h);
static void wifi_mgr_reopen_all_mcast(wifi_manager_handle_t h);
static int  wifi_mcast_socket_create(const char *group, uint16_t port);
static void wifi_mcast_sock_destroy(struct wifi_mcast_s *ch);
/* ======================== 事件回调（esp_event 任务上下文） ============ */

static void wifi_mgr_handle_wifi(void *arg, esp_event_base_t base,
                                 int32_t event_id, void *event_data)
{
    wifi_manager_handle_t h = arg;

    switch (event_id)
    {
    case WIFI_EVENT_STA_START:
        ESP_LOGI(TAG, "sta started, connecting to \"%s\"", h->ssid);
        wifi_mgr_connect(h);
        break;

    case WIFI_EVENT_STA_STOP:
        xSemaphoreTake(h->lock, portMAX_DELAY);
        if (h->running)
        {
            /* 用户在 STOP 事件送达前已重新 start，忽略本次停止 */
            xSemaphoreGive(h->lock);
            break;
        }
        h->state = WIFI_STATE_IDLE;
        wifi_mgr_close_all_mcast(h);       /* 需持锁调用 */
        xSemaphoreGive(h->lock);
        ESP_LOGI(TAG, "sta stopped");
        break;

    case WIFI_EVENT_STA_CONNECTED:
        /* 链路已通，等待 DHCP 拿 IP；状态保持 CONNECTING */
        ESP_LOGI(TAG, "sta connected, waiting IP...");
        break;

    case WIFI_EVENT_STA_DISCONNECTED:
    {
        wifi_event_sta_disconnected_t *evt = event_data;
        bool was_connected;

        xSemaphoreTake(h->lock, portMAX_DELAY);
        was_connected = (h->state == WIFI_STATE_CONNECTED);
        if (!h->running)
        {
            xSemaphoreGive(h->lock);
            return;
        }
        esp_timer_stop(h->connect_timer);
        wifi_mgr_close_all_mcast(h);
        h->state = WIFI_STATE_DISCONNECTED;
        strncpy(h->ip, "0.0.0.0", sizeof(h->ip) - 1);
        xSemaphoreGive(h->lock);

        if (was_connected)
        {
            wifi_mgr_fire_event(h, WIFI_EVT_DISCONNECTED);
        }
        ESP_LOGW(TAG, "disconnected, reason=%d", evt->reason);
        wifi_mgr_schedule_reconnect(h);
        break;
    }

    default:
        break;
    }
}

static void wifi_mgr_handle_ip(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    wifi_manager_handle_t h = arg;

    if (event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *got = event_data;
        char ipstr[16] = {0};
        bool first, ip_changed;

        esp_ip4addr_ntoa(&got->ip_info.ip, ipstr, sizeof(ipstr));

        xSemaphoreTake(h->lock, portMAX_DELAY);
        if (!h->running)
        {
            xSemaphoreGive(h->lock);
            return;
        }
        first      = (h->state != WIFI_STATE_CONNECTED);
        ip_changed = (strcmp(h->ip, ipstr) != 0);

        strncpy(h->ip, ipstr, sizeof(h->ip) - 1);
        h->ip[sizeof(h->ip) - 1] = '\0';
        h->state        = WIFI_STATE_CONNECTED;
        h->retry_count  = 0;
        esp_timer_stop(h->connect_timer);

        /* IP 就绪：整体重建（关闭后重新创建）组播 socket */
        wifi_mgr_reopen_all_mcast(h);
        xSemaphoreGive(h->lock);

        ESP_LOGI(TAG, "connected, ip=%s (first=%d, changed=%d)",
                 ipstr, first, ip_changed);

        if (first)
        {
            wifi_mgr_fire_event(h, WIFI_EVT_CONNECTED);
        }
        else if (ip_changed)
        {
            wifi_mgr_fire_event(h, WIFI_EVT_IP_CHANGED);
        }
    }
    else if (event_id == IP_EVENT_STA_LOST_IP)
    {
        xSemaphoreTake(h->lock, portMAX_DELAY);
        strncpy(h->ip, "0.0.0.0", sizeof(h->ip) - 1);
        xSemaphoreGive(h->lock);
        ESP_LOGI(TAG, "sta lost ip");
    }
}

/* ======================== 连接状态机 ======================== */

/** 发起一次连接（STA_START / 重连定时器 / set_sta 共用） */
static void wifi_mgr_connect(wifi_manager_handle_t h)
{
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
        wifi_mgr_schedule_reconnect(h);
        return;
    }
    wifi_mgr_arm_connect_timeout(h);

    xSemaphoreTake(h->lock, portMAX_DELAY);
    h->state = WIFI_STATE_CONNECTING;
    xSemaphoreGive(h->lock);
}

/** 启动单次连接超时定时器 */
static void wifi_mgr_arm_connect_timeout(wifi_manager_handle_t h)
{
    if (h->cfg.connect_timeout_ms == 0)
    {
        return;
    }
    esp_timer_stop(h->connect_timer);
    esp_timer_start_once(h->connect_timer,
                         (uint64_t)h->cfg.connect_timeout_ms * 1000);
}

/** 连接超时回调：仍未进入 CONNECTED，断开走 DISCONNECTED 重连流程 */
static void wifi_mgr_on_connect_timeout(void *arg)
{
    wifi_manager_handle_t h = arg;
    bool need_disconnect = false;

    xSemaphoreTake(h->lock, portMAX_DELAY);
    if (h->running && h->state != WIFI_STATE_CONNECTED)
    {
        need_disconnect = true;
    }
    xSemaphoreGive(h->lock);

    if (need_disconnect)
    {
        ESP_LOGW(TAG, "connect timeout (%lu ms), disconnect & retry",
                 (unsigned long)h->cfg.connect_timeout_ms);
        esp_wifi_disconnect();
    }
}

/** 重连定时器回调：发起新一次连接 */
static void wifi_mgr_on_reconnect(void *arg)
{
    wifi_manager_handle_t h = arg;
    ESP_LOGI(TAG, "auto reconnect...");
    wifi_mgr_fire_event(h, WIFI_EVT_RECONNECTING);
    /* 每次重连前先下发最新凭据：set_sta() 修改后无需等待下次启动即生效 */
    wifi_mgr_apply_config(h);
    wifi_mgr_connect(h);
}

/** 安排自动重连；超过 max_retries 则进入 ERROR 状态 */
static void wifi_mgr_schedule_reconnect(wifi_manager_handle_t h)
{
    bool give_up = false;

    xSemaphoreTake(h->lock, portMAX_DELAY);
    if (!h->running)
    {
        xSemaphoreGive(h->lock);
        return;
    }

    if (h->cfg.max_retries > 0 && h->retry_count >= h->cfg.max_retries)
    {
        h->state = WIFI_STATE_ERROR;
        give_up  = true;
    }
    else
    {
        h->retry_count++;
        h->state = WIFI_STATE_DISCONNECTED;
        esp_timer_stop(h->reconnect_timer);
        esp_timer_start_once(h->reconnect_timer,
                             (uint64_t)h->cfg.reconnect_interval_ms * 1000);
        ESP_LOGI(TAG, "reconnect scheduled in %lu ms (fail=%lu)",
                 (unsigned long)h->cfg.reconnect_interval_ms,
                 (unsigned long)h->retry_count);
    }
    xSemaphoreGive(h->lock);

    if (give_up)
    {
        ESP_LOGE(TAG, "reconnect retries exhausted (max=%lu), enter ERROR",
                 (unsigned long)h->cfg.max_retries);
    }
}

/* ======================== 事件工具 ======================== */

static void wifi_mgr_fire_event(wifi_manager_handle_t h, wifi_event_id_t id)
{
    wifi_mgr_event_t evt = {0};
    wifi_ap_record_t ap = {0};

    if (h == NULL || h->event_cb == NULL)
    {
        return;
    }

    xSemaphoreTake(h->lock, portMAX_DELAY);
    strncpy(evt.ssid, h->ssid, sizeof(evt.ssid) - 1);
    strncpy(evt.ip,   h->ip,   sizeof(evt.ip) - 1);
    evt.id   = id;
    evt.rssi = WIFI_RSSI_INVALID;
    xSemaphoreGive(h->lock);

    /* RSSI 仅在已连接时有意义，查询失败保持无效值 */
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
    {
        evt.rssi = ap.rssi;
    }

    h->event_cb(&evt, h->event_ctx);
}
/* ======================== 组播通道（socket 生命周期） ================== */

/** 创建组播 socket：加组 + 绑定端口 + TTL + 非阻塞（参考官方 udp_multicast 示例） */
static int wifi_mcast_socket_create(const char *group, uint16_t port)
{
    struct sockaddr_in saddr;
    struct ip_mreq imreq;
    int sock;
    int on = 1;
    uint8_t ttl = 1;
    uint8_t loop = 1;

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0)
    {
        ESP_LOGE(TAG, "socket create failed, errno=%d", errno);
        return -1;
    }

    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    /* 加入组播组（监听所有接口） */
    memset(&imreq, 0, sizeof(imreq));
    if (inet_aton(group, &imreq.imr_multiaddr.s_addr) != 1)
    {
        ESP_LOGE(TAG, "invalid mcast group: \"%s\"", group);
        close(sock);
        return -1;
    }
    imreq.imr_interface.s_addr = IPADDR_ANY;
    if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   &imreq, sizeof(imreq)) < 0)
    {
        ESP_LOGE(TAG, "IP_ADD_MEMBERSHIP(\"%s\") failed, errno=%d",
                 group, errno);
        close(sock);
        return -1;
    }

    /* 绑定端口（任意地址） */
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family      = AF_INET;
    saddr.sin_port        = htons(port);
    saddr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&saddr, sizeof(saddr)) < 0)
    {
        ESP_LOGE(TAG, "bind port %u failed, errno=%d", port, errno);
        close(sock);
        return -1;
    }

    /* 局域网组播 TTL=1；接收侧非阻塞，由 RX 任务轮询 */
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    /* 组播回环：本机已加组的 socket 能收到自己发出的包（测试/自环验证用，
     * lwIP 默认不回环，需显式置 IP_MULTICAST_LOOP） */
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
    fcntl(sock, F_SETFL, O_NONBLOCK);

    return sock;
}

/** 关闭一个通道的 socket 与 FIFO（不释放结构体） */
static void wifi_mcast_sock_destroy(struct wifi_mcast_s *ch)
{
    if (ch->fd >= 0)
    {
        close(ch->fd);
        ch->fd = -1;
    }
    if (ch->rx_fifo != NULL)
    {
        vRingbufferDelete(ch->rx_fifo);
        ch->rx_fifo = NULL;
    }
}

/** 关闭本实例所有通道的 socket（必须已持锁调用） */
static void wifi_mgr_close_all_mcast(wifi_manager_handle_t h)
{
    struct wifi_mcast_s *ch;

    for (ch = h->mcast_list; ch != NULL; ch = ch->next)
    {
        if (ch->fd >= 0)
        {
            close(ch->fd);
            ch->fd = -1;
        }
    }
}

/** 重建所有通道的 socket（必须已持锁调用） */
static void wifi_mgr_reopen_all_mcast(wifi_manager_handle_t h)
{
    struct wifi_mcast_s *ch;

    for (ch = h->mcast_list; ch != NULL; ch = ch->next)
    {
        if (ch->fd >= 0)
        {
            close(ch->fd);
        }
        ch->fd = wifi_mcast_socket_create(ch->group, ch->port);
    }
}

/* ======================== RX 任务 ======================== */

static void wifi_mcast_rx_task(void *arg)
{
    wifi_manager_handle_t h = arg;
    /* 任务栈为 4096（任务表），收包缓冲改走堆 */
    uint8_t *buf = malloc(WIFI_MCAST_RX_BUF_BYTES);

    if (buf == NULL)
    {
        ESP_LOGE(TAG, "mcast rx buf alloc failed (%u bytes), task exits",
                 (unsigned)WIFI_MCAST_RX_BUF_BYTES);
        xSemaphoreGive(h->rx_done);
        vTaskDelete(NULL);
    }

    for (;;)
    {
        xSemaphoreTake(h->lock, portMAX_DELAY);

        if (h->rx_stop)
        {
            xSemaphoreGive(h->lock);
            break;
        }

        for (struct wifi_mcast_s *ch = h->mcast_list; ch != NULL; ch = ch->next)
        {
            int len;

            if (!ch->rx_enable || ch->fd < 0)
            {
                continue;
            }
            while ((len = recvfrom(ch->fd, buf, WIFI_MCAST_RX_BUF_BYTES, 0,
                                   NULL, NULL)) > 0)
            {
                if (xRingbufferSend(ch->rx_fifo, buf, len,
                                    pdMS_TO_TICKS(0)) != pdPASS)
                {
                    ch->rx_dropped++;
                }
            }
        }

        xSemaphoreGive(h->lock);
        vTaskDelay(pdMS_TO_TICKS(WIFI_MCAST_RX_POLL_MS));
    }

    free(buf);
    xSemaphoreGive(h->rx_done);
    vTaskDelete(NULL);
}

/* ======================== 配置 ======================== */

/** 组装 wifi_config_t 并下发生效 */
static esp_err_t wifi_mgr_apply_config(wifi_manager_handle_t h)
{
    wifi_config_t conf = {0};
    const char *ssid, *password;
    size_t s_len, p_len;
    node_role_cfg_t role_cfg = {0};

    xSemaphoreTake(h->lock, portMAX_DELAY);
    if (h->ssid[0] != '\0')
    {
        /* set_sta() 已指定新凭据：直接使用 */
        ssid     = h->ssid;
        password = h->password;
    }
    else if (node_role_get(&role_cfg) == ESP_OK &&
             role_cfg.wifi_ssid[0] != '\0')
    {
        /* node_role NVS 已配网：读缓存凭据并同步进句柄，便于日志/事件展示 */
        ssid     = role_cfg.wifi_ssid;
        password = role_cfg.wifi_password;
        snprintf(h->ssid,     sizeof(h->ssid),     "%s", ssid);
        snprintf(h->password, sizeof(h->password), "%s", password);
    }
    else
    {
        /* NVS 未配网（node_role 未初始化/为空）：兜底默认凭据 */
        ssid     = WIFI_MANAGER_DEFAULT_SSID;
        password = WIFI_MANAGER_DEFAULT_PASSWORD;
        snprintf(h->ssid,     sizeof(h->ssid),     "%s", ssid);
        snprintf(h->password, sizeof(h->password), "%s", password);
    }
    s_len = strlen(ssid);
    p_len = strlen(password);

    /* 显式截断拷贝：驱动结构体内定长（ssid[32]/password[64]），避免 snprintf 截断告警 */
    if (s_len >= sizeof(conf.sta.ssid))
    {
        s_len = sizeof(conf.sta.ssid) - 1;
    }
    memcpy(conf.sta.ssid, ssid, s_len);
    conf.sta.ssid[s_len] = '\0';
    if (p_len >= sizeof(conf.sta.password))
    {
        p_len = sizeof(conf.sta.password) - 1;
    }
    memcpy(conf.sta.password, password, p_len);
    conf.sta.password[p_len] = '\0';

    conf.sta.scan_method     = WIFI_ALL_CHANNEL_SCAN;
    conf.sta.threshold.authmode = (p_len > 0) ? WIFI_AUTH_WPA2_PSK
                                              : WIFI_AUTH_OPEN;
    xSemaphoreGive(h->lock);

    ESP_LOGI(TAG, "set sta config: ssid=\"%s\" plen=%u", ssid, (unsigned)p_len);
    return esp_wifi_set_config(WIFI_IF_STA, &conf);
}
/* ======================== 生命周期 ======================== */

wifi_manager_handle_t wifi_manager_create(const wifi_manager_cfg_t *cfg)
{
    wifi_manager_handle_t h;
    esp_timer_create_args_t tm = {0};
    esp_err_t err;

    /* NVS：esp_wifi 驱动依赖（与 bt_audio 的 nvs_flash_init 一致）；
     * 业务键由 node_role 统一管理（node_role_get/set），本模块仍不写 NVS */
    {
        esp_err_t nerr = nvs_flash_init();
        if (nerr == ESP_ERR_NVS_NO_FREE_PAGES || nerr == ESP_ERR_NVS_NEW_VERSION_FOUND)
        {
            ESP_ERROR_CHECK(nvs_flash_erase());
            nerr = nvs_flash_init();
        }
        if (nerr != ESP_OK)
        {
            ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(nerr));
            return NULL;
        }
    }

    /* 网络栈（esp_netif / 默认事件循环）只需初始化一次 */
    esp_netif_init();
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "event loop create failed: %s", esp_err_to_name(err));
        return NULL;
    }

    h = calloc(1, sizeof(*h));
    if (h == NULL)
    {
        ESP_LOGE(TAG, "alloc manager failed");
        return NULL;
    }
    h->lock = xSemaphoreCreateMutex();
    if (h->lock == NULL)
    {
        free(h);
        return NULL;
    }
    h->rx_done = xSemaphoreCreateBinary();
    if (h->rx_done == NULL)
    {
        vSemaphoreDelete(h->lock);
        free(h);
        return NULL;
    }

    /* 默认配置（cfg 为空或字段为 0 时回退默认值） */
    h->cfg.connect_timeout_ms    = (cfg && cfg->connect_timeout_ms)
                                   ? cfg->connect_timeout_ms
                                   : WIFI_MGR_CFG_DEFAULT_CONNECT_TIMEOUT_MS;
    h->cfg.reconnect_interval_ms = (cfg && cfg->reconnect_interval_ms)
                                   ? cfg->reconnect_interval_ms
                                   : WIFI_MGR_CFG_DEFAULT_RECONNECT_INTERVAL_MS;
    h->cfg.max_retries           = cfg ? cfg->max_retries
                                       : WIFI_MGR_CFG_DEFAULT_MAX_RETRIES;

    strncpy(h->ip, "0.0.0.0", sizeof(h->ip) - 1);
    h->state = WIFI_STATE_IDLE;

    /* 定时器：单次连接超时 */
    tm.callback    = wifi_mgr_on_connect_timeout;
    tm.arg         = h;
    tm.name        = "wifi_conn_timeout";
    esp_timer_create(&tm, &h->connect_timer);

    /* 定时器：自动重连调度 */
    tm.callback    = wifi_mgr_on_reconnect;
    tm.name        = "wifi_reconnect";
    esp_timer_create(&tm, &h->reconnect_timer);

    /* 事件注册（默认循环；create~destroy 有效） */
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        wifi_mgr_handle_wifi, h,
                                        &h->wifi_evt_inst);
    esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                        wifi_mgr_handle_ip, h,
                                        &h->ip_evt_inst);

    /* 组播收包任务（任务表：wifi_mcast_rx_Task，Core 0 / prio 6 / 栈 4096） */
    if (xTaskCreatePinnedToCore(wifi_mcast_rx_task, "wifi_mcast_rx",
                                WIFI_MCAST_RX_TASK_STACK, h,
                                WIFI_MCAST_RX_TASK_PRIO,
                                &h->rx_task, WIFI_MCAST_RX_TASK_CORE) != pdPASS)
    {
        ESP_LOGE(TAG, "mcast rx task create failed");
    }

    ESP_LOGI(TAG, "wifi_manager created (timeout=%lums, reconn=%lums, retry=%lu)",
             (unsigned long)h->cfg.connect_timeout_ms,
             (unsigned long)h->cfg.reconnect_interval_ms,
             (unsigned long)h->cfg.max_retries);
    return h;
}

esp_err_t wifi_manager_start(wifi_manager_handle_t h)
{
    esp_err_t err;

    if (h == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (h->running)
    {
        return ESP_OK;      /* 幂等 */
    }

    /* 驱动层初始化：只执行一次 */
    if (!h->wifi_inited)
    {
        if (!h->netif_created)
        {
            if (esp_netif_create_default_wifi_sta() == NULL)
            {
                ESP_LOGE(TAG, "create default wifi sta netif failed");
                return ESP_FAIL;
            }
            h->netif_created = true;
        }
        /* 官方写法：WIFI_INIT_CONFIG_DEFAULT() 是初始化器，需先赋给局部变量 */
        wifi_init_config_t wif_cfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&wif_cfg);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
            return err;
        }
        /* RAM 存储：本模块不写 NVS */
        esp_wifi_set_storage(WIFI_STORAGE_RAM);
        h->wifi_inited = true;
    }

    /* 与官方 station 示例一致：先 set_mode 使能 STA 接口，再 set_config */
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_set_mode STA failed: %s", esp_err_to_name(err));
        return err;
    }

    err = wifi_mgr_apply_config(h);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "apply sta config failed: %s", esp_err_to_name(err));
        return err;
    }

    xSemaphoreTake(h->lock, portMAX_DELAY);
    h->running     = true;
    h->state       = WIFI_STATE_CONNECTING;
    h->retry_count = 0;
    xSemaphoreGive(h->lock);

    err = esp_wifi_start();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        xSemaphoreTake(h->lock, portMAX_DELAY);
        h->running = false;
        h->state   = WIFI_STATE_ERROR;
        xSemaphoreGive(h->lock);
        return err;
    }

    ESP_LOGI(TAG, "wifi_manager started");
    return ESP_OK;
}

esp_err_t wifi_manager_stop(wifi_manager_handle_t h)
{
    if (h == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(h->lock, portMAX_DELAY);
    h->running = false;
    h->state   = WIFI_STATE_IDLE;
    esp_timer_stop(h->connect_timer);
    esp_timer_stop(h->reconnect_timer);
    wifi_mgr_close_all_mcast(h);        /* 需持锁 */
    xSemaphoreGive(h->lock);

    if (h->wifi_inited)
    {
        esp_wifi_disconnect();
        esp_wifi_stop();
    }
    ESP_LOGI(TAG, "wifi_manager stopped");
    return ESP_OK;
}

void wifi_manager_destroy(wifi_manager_handle_t h)
{
    if (h == NULL)
    {
        return;
    }

    wifi_manager_stop(h);

    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                          h->wifi_evt_inst);
    esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID,
                                          h->ip_evt_inst);

    esp_timer_delete(h->connect_timer);
    esp_timer_delete(h->reconnect_timer);

    /* 先停收任务：置退出标志后等待其自行退出（正常路径），超时才强制删除 */
    if (h->rx_task != NULL)
    {
        xSemaphoreTake(h->lock, portMAX_DELAY);
        h->rx_stop = true;
        xSemaphoreGive(h->lock);
        if (xSemaphoreTake(h->rx_done, pdMS_TO_TICKS(2000)) != pdPASS)
        {
            ESP_LOGW(TAG, "rx task did not exit in time, force delete");
            vTaskDelete(h->rx_task);
        }
        h->rx_task = NULL;
    }

    /* 清空剩余通道 */
    while (h->mcast_list != NULL)
    {
        struct wifi_mcast_s *ch = h->mcast_list;
        h->mcast_list = ch->next;
        wifi_mcast_sock_destroy(ch);
        free(ch);
    }

    if (h->wifi_inited)
    {
        esp_wifi_deinit();
        h->wifi_inited = false;
    }

    vSemaphoreDelete(h->rx_done);
    vSemaphoreDelete(h->lock);
    free(h);
    ESP_LOGI(TAG, "wifi_manager destroyed");
}

/* ======================== 配网 / 订阅 / 查询 ======================== */

esp_err_t wifi_manager_set_sta(wifi_manager_handle_t h,
                               const char *ssid, const char *password)
{
    esp_err_t err;
    wifi_state_t st;

    if (h == NULL || ssid == NULL || password == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(ssid) >= sizeof(h->ssid) ||
        strlen(password) >= sizeof(h->password))
    {
        return ESP_ERR_INVALID_SIZE;
    }

    xSemaphoreTake(h->lock, portMAX_DELAY);
    snprintf(h->ssid,     sizeof(h->ssid),     "%s", ssid);
    snprintf(h->password, sizeof(h->password), "%s", password);
    st = h->state;
    xSemaphoreGive(h->lock);
    ESP_LOGI(TAG, "set sta credentials: \"%s\"", ssid);

    if (!h->running)
    {
        return ESP_OK;      /* start 时会应用 */
    }

    if (st == WIFI_STATE_CONNECTED || st == WIFI_STATE_CONNECTING)
    {
        /* 正在连接/已连接：断开后由 DISCONNECTED → 自动重连以新配置连接 */
        return esp_wifi_disconnect();
    }

    /* 未在连接中：取消可能挂起的重连定时器，直接下发新配置并立即重连 */
    xSemaphoreTake(h->lock, portMAX_DELAY);
    esp_timer_stop(h->connect_timer);
    esp_timer_stop(h->reconnect_timer);
    xSemaphoreGive(h->lock);

    err = wifi_mgr_apply_config(h);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "apply sta config failed: %s", esp_err_to_name(err));
        return err;
    }
    wifi_mgr_connect(h);
    return ESP_OK;
}

esp_err_t wifi_manager_register_event_cb(wifi_manager_handle_t h,
                                         wifi_event_cb_t cb, void *user_ctx)
{
    if (h == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(h->lock, portMAX_DELAY);
    h->event_cb  = cb;
    h->event_ctx = user_ctx;
    xSemaphoreGive(h->lock);
    return ESP_OK;
}

esp_err_t wifi_manager_get_state(wifi_manager_handle_t h, wifi_state_t *state)
{
    if (h == NULL || state == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(h->lock, portMAX_DELAY);
    *state = h->state;
    xSemaphoreGive(h->lock);
    return ESP_OK;
}

bool wifi_manager_is_connected(wifi_manager_handle_t h)
{
    wifi_state_t st = WIFI_STATE_IDLE;
    return (h != NULL) &&
           (wifi_manager_get_state(h, &st) == ESP_OK) &&
           (st == WIFI_STATE_CONNECTED);
}

esp_err_t wifi_manager_get_ip(wifi_manager_handle_t h, char *ip, size_t *len)
{
    size_t need;

    if (h == NULL || ip == NULL || len == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(h->lock, portMAX_DELAY);
    need = strlen(h->ip);
    if (*len <= need)
    {
        *len = need;
        xSemaphoreGive(h->lock);
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(ip, h->ip, need + 1);
    *len = need;
    xSemaphoreGive(h->lock);
    return ESP_OK;
}

int8_t wifi_manager_get_rssi(wifi_manager_handle_t h)
{
    wifi_ap_record_t ap = {0};
    int8_t rssi = WIFI_RSSI_INVALID;

    if (h != NULL && wifi_manager_is_connected(h))
    {
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
        {
            rssi = ap.rssi;
        }
    }
    return rssi;
}

/* ======================== 组播通道 API ======================== */

esp_err_t wifi_manager_mcast_open(wifi_manager_handle_t h,
                                  const wifi_mcast_cfg_t *cfg,
                                  wifi_mcast_handle_t *out)
{
    struct wifi_mcast_s *ch;
    struct in_addr chk;
    size_t glen;

    if (h == NULL || cfg == NULL || out == NULL || cfg->port == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    glen = strlen(cfg->group);
    if (glen == 0 || glen >= WIFI_MCAST_GROUP_LEN ||
        inet_aton(cfg->group, &chk) != 1)
    {
        ESP_LOGE(TAG, "invalid mcast group: \"%s\"",
                 cfg->group ? cfg->group : "");
        return ESP_ERR_INVALID_ARG;
    }

    ch = calloc(1, sizeof(*ch));
    if (ch == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    memcpy(ch->group, cfg->group, glen);
    ch->group[glen]    = '\0';
    ch->port           = cfg->port;
    ch->rx_enable      = cfg->rx_enable;
    ch->rx_fifo_bytes  = cfg->rx_fifo_bytes ? cfg->rx_fifo_bytes
                                            : WIFI_MCAST_RX_FIFO_DEFAULT_BYTES;
    ch->fd = -1;

    if (cfg->rx_enable)
    {
        /* NOSPLIT：一个 UDP 包整体入环，跨环回绕时不会被拆成两半，
         * 保证 mcast_recv 每次拿到的都是一整包（第九阶段自环测试需要） */
        ch->rx_fifo = xRingbufferCreate(ch->rx_fifo_bytes,
                                        RINGBUF_TYPE_NOSPLIT);
        if (ch->rx_fifo == NULL)
        {
            ESP_LOGE(TAG, "rx fifo create failed (%u bytes)",
                     (unsigned)ch->rx_fifo_bytes);
            free(ch);
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(h->lock, portMAX_DELAY);
    ch->owner     = h;
    ch->next      = h->mcast_list;
    h->mcast_list = ch;
    if (h->state == WIFI_STATE_CONNECTED)
    {
        ch->fd = wifi_mcast_socket_create(ch->group, ch->port);
    }
    xSemaphoreGive(h->lock);

    *out = ch;
    ESP_LOGI(TAG, "mcast opened: %s:%u rx=%d fifo=%u",
             ch->group, ch->port, ch->rx_enable,
             (unsigned)ch->rx_fifo_bytes);
    return ESP_OK;
}

esp_err_t wifi_manager_mcast_send(wifi_mcast_handle_t ch,
                                  const void *data, size_t len,
                                  uint32_t timeout_ms)
{
    struct sockaddr_in dest;
    TickType_t deadline;
    int ret;

    if (ch == NULL || (data == NULL && len != 0))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (ch->fd < 0)
    {
        return ESP_ERR_INVALID_STATE;   /* WiFi 未连接，通道无 socket */
    }

    memset(&dest, 0, sizeof(dest));
    dest.sin_family      = AF_INET;
    dest.sin_port        = htons(ch->port);
    dest.sin_addr.s_addr = inet_addr(ch->group);

    deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    for (;;)
    {
        ret = sendto(ch->fd, data, len, 0,
                     (struct sockaddr *)&dest, sizeof(dest));
        if (ret == (int)len)
        {
            return ESP_OK;
        }
        if (!(ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)))
        {
            ESP_LOGW(TAG, "mcast sendto failed ret=%d errno=%d", ret, errno);
            return ESP_FAIL;
        }
        if (timeout_ms == 0 || xTaskGetTickCount() >= deadline)
        {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

esp_err_t wifi_manager_mcast_recv(wifi_mcast_handle_t ch,
                                  void *buf, size_t cap, size_t *len,
                                  uint32_t timeout_ms)
{
    size_t item_size = 0;
    void  *item;

    if (ch == NULL || buf == NULL || len == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ch->rx_enable || ch->rx_fifo == NULL)
    {
        return ESP_ERR_INVALID_STATE;   /* 通道未开启接收 */
    }

    item = xRingbufferReceive(ch->rx_fifo, &item_size,
                              (timeout_ms == 0)
                                  ? portMAX_DELAY
                                  : pdMS_TO_TICKS(timeout_ms));
    if (item == NULL)
    {
        return ESP_ERR_TIMEOUT;
    }

    if (item_size > cap)
    {
        ESP_LOGW(TAG, "mcast item %u > cap %u, truncated",
                 (unsigned)item_size, (unsigned)cap);
        item_size = cap;
    }
    memcpy(buf, item, item_size);
    *len = item_size;
    vRingbufferReturnItem(ch->rx_fifo, item);
    return ESP_OK;
}

esp_err_t wifi_manager_mcast_close(wifi_mcast_handle_t ch)
{
    if (ch == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (ch->owner != NULL)
    {
        xSemaphoreTake(ch->owner->lock, portMAX_DELAY);
        struct wifi_mcast_s **pp = &ch->owner->mcast_list;
        while (*pp != NULL && *pp != ch)
        {
            pp = &(*pp)->next;
        }
        if (*pp == ch)
        {
            *pp = ch->next;         /* 从链表摘除 */
        }
        xSemaphoreGive(ch->owner->lock);
    }

    wifi_mcast_sock_destroy(ch);
    free(ch);
    return ESP_OK;
}
