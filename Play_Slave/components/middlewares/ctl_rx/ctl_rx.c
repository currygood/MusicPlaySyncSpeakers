/**
 * @file ctl_rx.c
 * @brief 从节点控制通道：接收音量命令 + 周期发送上线公告
 *
 * 收包路径：wifi_manager 的 RX 任务把控制组播的包搬进本通道的 FIFO，
 * 本模块的任务再从 FIFO 取出一整包解析。FIFO 必须有人读，否则 lwIP 的
 * pbuf 会被一直占住（旧架构 PONG 丢失就是这个坑）。
 *
 * 收：只认 4 字节基础帧（音量）。主节点还会周期性发 SYNC_START /
 *     HEARTBEAT / PING / PONG 扩展帧，靠 magic + cmd 精确过滤。
 * 发：每 3 秒向 239.0.0.1:5679 发一条 SYNC_MSG_ANNOUNCE（0x30），
 *     携带 MAC / IP / 音频端口 / 音量 / RSSI / 设备名，主节点据此维护
 *     "已连接从机"表和单播目标。公告走同一个控制通道，不新增 socket。
 */

#include "ctl_rx.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "amplifier.h"

#define TAG "CTL_RX"

/* 与主节点 sync_protocol.h 保持一致，改动需两边同步 */
#define CTL_MAGIC            0xAA55u
#define CTL_CMD_VOLUME       0x05u
#define CTL_MSG_ANNOUNCE     0x30u
#define CTL_ANNOUNCE_VERSION 1u
#define CTL_ANNOUNCE_TOKEN   0x53594E43u   /* "SYNC"，配对令牌 */
#define CTL_AUDIO_PORT       5680u

#define CTL_TASK_STACK   (3072)
#define CTL_TASK_PRIO    (5)
#define CTL_RX_BUF       (64)
#define CTL_POLL_MS      (1000)
#define CTL_ANNOUNCE_MS  (3000)

/** 基础控制帧：Magic(2) + Cmd(1) + Param(1) */
typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t  cmd;
    uint8_t  param;
} ctl_basic_t;

/** 扩展帧头：Magic(2) + Type(1) + Len(1) + Data[] */
typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t  type;
    uint8_t  len;
    uint8_t  data[];
} ctl_ext_t;

/** 从节点公告载荷（与主节点 sync_announce_payload_t 逐字节一致） */
typedef struct __attribute__((packed)) {
    uint8_t  ver;
    uint8_t  mac[6];
    uint8_t  ip[4];
    uint16_t audio_port;
    uint8_t  volume;
    int8_t   rssi;
    uint8_t  flags;
    uint32_t token;
    char     name[16];
} ctl_announce_t;

static wifi_mcast_handle_t s_chan;
static wifi_manager_handle_t s_wifi;
static bool                s_started;
static uint64_t            s_last_announce_us;

/**
 * @brief 发一条上线公告（主节点据此维护"已连接从机"表）
 *
 * 复用控制组播通道 239.0.0.1:5679，不新增 socket。IP 每次现查，DHCP
 * 换地址后主节点能自动跟上；设备名默认取 MAC 末两字节。
 */
static void ctl_send_announce(void)
{
    ctl_announce_t a;
    uint8_t  mac[6] = {0};
    char     ip[16] = {0};
    size_t   ip_len = sizeof(ip);
    struct in_addr addr;
    uint8_t  pkt[sizeof(ctl_ext_t) + sizeof(ctl_announce_t)];

    if (s_wifi == NULL ||
        wifi_manager_get_ip(s_wifi, ip, &ip_len) != ESP_OK ||
        ip[0] == '\0')
    {
        return;
    }
    if (inet_pton(AF_INET, ip, &addr) != 1)
    {
        return;
    }

    memset(&a, 0, sizeof(a));
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK)
    {
        memset(mac, 0, sizeof(mac));
    }
    a.ver        = CTL_ANNOUNCE_VERSION;
    memcpy(a.mac, mac, sizeof(mac));
    memcpy(a.ip, &addr.s_addr, sizeof(a.ip));
    a.audio_port = CTL_AUDIO_PORT;
    a.volume     = Amplifier_Get_Volume();
    a.rssi       = wifi_manager_get_rssi(s_wifi);
    a.token      = CTL_ANNOUNCE_TOKEN;
    snprintf(a.name, sizeof(a.name), "从机-%02X%02X", mac[4], mac[5]);

    pkt[0] = (uint8_t)(CTL_MAGIC & 0xFF);
    pkt[1] = (uint8_t)(CTL_MAGIC >> 8);
    pkt[2] = CTL_MSG_ANNOUNCE;
    pkt[3] = (uint8_t)sizeof(a);
    memcpy(pkt + sizeof(ctl_ext_t), &a, sizeof(a));

    if (wifi_manager_mcast_send(s_chan, pkt, sizeof(pkt), 100) != ESP_OK)
    {
        ESP_LOGD(TAG, "announce send failed");
    }
}

static void ctl_rx_task(void *arg)
{
    uint8_t buf[CTL_RX_BUF];
    size_t  len = 0;
    int     last_vol = -1;

    (void)arg;

    while (1)
    {
        /* 周期公告：主节点只靠它判断从机在线/离线。recv 每 1s 返回一次，
         * 所以实际公告间隔是 3~4s，仍在主节点 10s 超时窗口内。 */
        int64_t now_us = esp_timer_get_time();
        if ((uint64_t)(now_us - (int64_t)s_last_announce_us) >=
            (uint64_t)CTL_ANNOUNCE_MS * 1000u)
        {
            s_last_announce_us = (uint64_t)now_us;
            ctl_send_announce();
        }

        if (wifi_manager_mcast_recv(s_chan, buf, sizeof(buf), &len,
                                    CTL_POLL_MS) != ESP_OK)
        {
            continue;   /* 超时/未连接：继续等 */
        }
        if (len < sizeof(ctl_basic_t))
        {
            continue;
        }

        ctl_basic_t f;
        memcpy(&f, buf, sizeof(f));
        if (f.magic != CTL_MAGIC || f.cmd != CTL_CMD_VOLUME)
        {
            continue;   /* START/HEARTBEAT/PING/PONG 等扩展帧 */
        }

        uint8_t vol = (f.param > 100) ? 100 : f.param;
        if ((int)vol == last_vol)
        {
            continue;   /* 主节点重复下发同一个值时不再打日志/重复设置 */
        }
        last_vol = vol;
        Amplifier_Set_Volume(vol);
        ESP_LOGI(TAG, "volume -> %u (from master)", (unsigned)vol);
    }
}

esp_err_t ctl_rx_start(wifi_manager_handle_t wifi)
{
    wifi_mcast_cfg_t cfg = {
        .group         = CTL_RX_GROUP,
        .port          = CTL_RX_PORT,
        .rx_enable     = true,
        .rx_fifo_bytes = 1024,
    };
    esp_err_t err;

    if (wifi == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_started)
    {
        return ESP_OK;
    }

    err = wifi_manager_mcast_open(wifi, &cfg, &s_chan);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "mcast_open %s:%u failed: %s",
                 CTL_RX_GROUP, (unsigned)CTL_RX_PORT, esp_err_to_name(err));
        return err;
    }

    s_wifi = wifi;
    s_last_announce_us = 0;   /* 任务第一次循环立即公告 */

    if (xTaskCreate(ctl_rx_task, "ctl_rx", CTL_TASK_STACK, NULL,
                    CTL_TASK_PRIO, NULL) != pdPASS)
    {
        ESP_LOGE(TAG, "task create failed");
        wifi_manager_mcast_close(s_chan);
        s_chan = NULL;
        s_wifi = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    ESP_LOGI(TAG, "listening on %s:%u (volume + announce)", CTL_RX_GROUP,
             (unsigned)CTL_RX_PORT);
    return ESP_OK;
}
