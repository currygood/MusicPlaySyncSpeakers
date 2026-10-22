/**
 * @file ctl_rx.c
 * @brief 从节点控制通道接收实现（当前只处理音量）
 *
 * 收包路径：wifi_manager 的 RX 任务把控制组播的包搬进本通道的 FIFO，
 * 本模块的任务再从 FIFO 取出一整包解析。FIFO 必须有人读，否则 lwIP 的
 * pbuf 会被一直占住（旧架构 PONG 丢失就是这个坑）。
 *
 * 只认 4 字节基础帧。主节点还会周期性发 SYNC_START / HEARTBEAT /
 * PING / PONG 扩展帧，靠 magic + cmd 精确过滤，端口上收到什么都不影响。
 */

#include "ctl_rx.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "amplifier.h"

#define TAG "CTL_RX"

/* 与主节点 sync_protocol.h 保持一致，改动需两边同步 */
#define CTL_MAGIC        0xAA55u
#define CTL_CMD_VOLUME   0x05u

#define CTL_TASK_STACK   (3072)
#define CTL_TASK_PRIO    (5)
#define CTL_RX_BUF       (64)
#define CTL_POLL_MS      (1000)

/** 基础控制帧：Magic(2) + Cmd(1) + Param(1) */
typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t  cmd;
    uint8_t  param;
} ctl_basic_t;

static wifi_mcast_handle_t s_chan;
static bool                s_started;

static void ctl_rx_task(void *arg)
{
    uint8_t buf[CTL_RX_BUF];
    size_t  len = 0;
    int     last_vol = -1;

    (void)arg;

    while (1)
    {
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

    if (xTaskCreate(ctl_rx_task, "ctl_rx", CTL_TASK_STACK, NULL,
                    CTL_TASK_PRIO, NULL) != pdPASS)
    {
        ESP_LOGE(TAG, "task create failed");
        wifi_manager_mcast_close(s_chan);
        s_chan = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    ESP_LOGI(TAG, "listening on %s:%u (volume)", CTL_RX_GROUP,
             (unsigned)CTL_RX_PORT);
    return ESP_OK;
}
