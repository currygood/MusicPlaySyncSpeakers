/**
 * @file light_control.c
 * @brief 灯控模块实现（App 层），对应《主音频节点软件架构分层设计》3.3.3
 *
 * 实现要点：
 *   1. create() 校验配置并创建 SmartHome_Task（优先级 5、栈 4096、Core 0，
 *      见分层设计第 6 节任务表）；任务循环：出队 UI 灯控指令，JSON 编码后经
 *      wifi_manager 灯控组播通道发送；同时轮询接收组播帧，解析 ack / 周期 status
 *      上报并触发 on_event 回调；
 *   2. 灯状态缓存（最多 LC_TRACKED_LIGHTS 个灯，以最近一次上报为准）：
 *      状态 = ack/status 的 ok 字段（true=开、false=关），未收到上行为 UNKNOWN；
 *      超过 state_timeout_ms 未收到某灯上行，发 LIGHT_EVT_ERROR 并把该灯状态落回
 *      UNKNOWN（只在 ON/OFF -> UNKNOWN 状态转移时报一次，之后静默直到新上报）；
 *   3. 不自行持有 socket：组播通道由调用方经 wifi_manager_mcast_open() 打开后传入
 *      （rx_enable 需开启），WiFi 重连 / 换 IP 后由 wifi_manager 内部自动重建。
 *
 * 接口依据（均为本仓库现有头文件/已安装组件）：
 *   - wifi_manager.h（components/msystem/wifi_manager/wifi_manager.h，设计 3.4.2）：
 *     wifi_mcast_handle_t / wifi_manager_mcast_send() / wifi_manager_mcast_recv()；
 *   - cJSON（managed_components/espressif__cjson，设计 7 依赖清单
 *     espressif/cjson ^1.7.19）：cJSON_CreateObject / cJSON_AddStringToObject /
 *     cJSON_PrintUnformatted / cJSON_Parse / cJSON_GetObjectItem / cJSON_IsString /
 *     cJSON_IsBool / cJSON_Delete；
 *   - freertos：xQueueCreate / xQueueSend / xQueueReceive / xTaskCreatePinnedToCore。
 */

#include "light_control.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"

/* ======================== 模块常量 ================================================== */

#define TAG "LightControl"

/** 灯名最大有效长度（设计 3.3.3：data.light[16]，含结束符） */
#define LC_LIGHT_NAME_LEN        16

/** 命令队列深度（on/off 指令 + destroy） */
#define LC_CMD_QUEUE_LEN         8

/** 入队超时（ms） */
#define LC_CMD_QUEUE_TIMEOUT_MS  100

/** 任务名 / 优先级 / 栈 / 核：与分层设计 6 任务表一致（SmartHome_Task, 5, 4096, Core0） */
#define LC_TASK_NAME             "SmartHome_Task"
#define LC_TASK_PRIO             5
#define LC_TASK_STACK            4096
#define LC_TASK_CORE             0

/** 命令队列轮询间隔（ms）：出队空时醒来收组播帧与超时检测 */
#define LC_TASK_POLL_MS          100

/** 组播收包轮询超时（ms）：wifi_manager_mcast_recv 以 0 表示永久阻塞，故轮询需 >0 */
#define LC_RX_POLL_MS            5

/** 接收缓冲（JSON 帧长度上限；ack/status 报文远小于此） */
#define LC_RX_BUF_BYTES          512

/** 状态缓存最多同时追踪的灯数（超出时记录日志并忽略新灯名） */
#define LC_TRACKED_LIGHTS        8

/** 发送超时默认值（cfg->tx_timeout_ms 为 0 时），单位 ms */
#define LC_DEFAULT_TX_TIMEOUT_MS     100

/** 状态上报超时默认值：90s = 3 × 30s 上报周期 */
#define LC_DEFAULT_STATE_TIMEOUT_MS  90000

/* 任务内部命令 */
typedef enum {
    LIGHT_QUEUE_SEND = 0,   /* 发送一条灯控指令 */
    LIGHT_QUEUE_DESTROY,    /* 销毁模块 */
} light_queue_cmd_t;

typedef struct {
    light_queue_cmd_t qcmd;
    light_cmd_t       cmd;         /* qcmd==LIGHT_QUEUE_SEND 时有效 */
    char              light[LC_LIGHT_NAME_LEN];
} light_queue_msg_t;

/* 单灯状态缓存槽位 */
typedef struct {
    char          name[LC_LIGHT_NAME_LEN];
    bool          active;
    light_state_t state;         /* 最近一次上报归一化状态；未收到前为 UNKNOWN */
    uint32_t      last_rx_ms;    /* 最近一次收到该灯上行的运行时刻（esp_timer, ms） */
} light_entry_t;

struct light_control_s {
    wifi_mcast_handle_t      chan;
    uint32_t                 tx_timeout_ms;
    uint32_t                 state_timeout_ms;
    light_control_event_cb_t on_event;
    void                    *event_ctx;

    QueueHandle_t      cmd_q;
    TaskHandle_t       task;
    SemaphoreHandle_t  lock;             /* 保护 entries 状态表 */
    SemaphoreHandle_t  destroy_done;

    light_entry_t entries[LC_TRACKED_LIGHTS];
};

/* ======================== 内部函数 ================================================ */

static light_entry_t *Light_Find_Entry(light_control_handle_t h, const char *name)
{
    for (int i = 0; i < LC_TRACKED_LIGHTS; i++)
    {
        if (h->entries[i].active && strcmp(h->entries[i].name, name) == 0)
        {
            return &h->entries[i];
        }
    }
    return NULL;
}

static void Light_Fire_Event(light_control_handle_t h, light_event_id_t id,
                             const char *name, bool ok, light_state_t state)
{
    light_event_t evt;

    if (h->on_event == NULL)
    {
        return;
    }
    memset(&evt, 0, sizeof(evt));
    evt.id = id;
    snprintf(evt.data.light, sizeof(evt.data.light), "%s", name);
    evt.data.ok    = ok;
    evt.data.state = state;
    h->on_event(&evt, h->event_ctx);
}

static void Light_Send_Cmd(light_control_handle_t h, const light_queue_msg_t *msg)
{
    cJSON *root = NULL;
    char  *json = NULL;
    esp_err_t ret;
    const char *cmd_str = (msg->cmd == LIGHT_CMD_ON) ? "on" : "off";

    root = cJSON_CreateObject();
    if (root == NULL)
    {
        ESP_LOGE(TAG, "send: cJSON_CreateObject failed");
        return;
    }
    cJSON_AddStringToObject(root, "cmd", cmd_str);
    cJSON_AddStringToObject(root, "light", msg->light);
    json = cJSON_PrintUnformatted(root);
    if (json == NULL)
    {
        ESP_LOGE(TAG, "send: cJSON_PrintUnformatted failed");
        cJSON_Delete(root);
        return;
    }

    ret = wifi_manager_mcast_send(h->chan, json, strlen(json), h->tx_timeout_ms);
    ESP_LOGI(TAG, "send cmd=%s light=\"%s\" (%d bytes) -> %s",
             cmd_str, msg->light, (int)strlen(json),
             (ret == ESP_OK) ? "OK" : esp_err_to_name(ret));

    free(json);
    cJSON_Delete(root);
}

static void Light_Handle_Frame(light_control_handle_t h, const char *json, size_t len)
{
    cJSON *root = NULL;
    cJSON *item_cmd  = NULL;
    cJSON *item_name = NULL;
    cJSON *item_ok   = NULL;
    const char *cmd_str = NULL;
    const char *name    = NULL;
    bool ok = false;
    light_state_t state;
    light_event_id_t event_id;
    light_entry_t *entry;

    ESP_LOGI(TAG, "recv %u bytes: %s", (unsigned)len, json);

    root = cJSON_Parse(json);
    if (root == NULL)
    {
        ESP_LOGW(TAG, "recv: json parse failed");
        return;
    }

    item_cmd = cJSON_GetObjectItem(root, "cmd");
    if (!cJSON_IsString(item_cmd) || item_cmd->valuestring == NULL)
    {
        ESP_LOGW(TAG, "recv: missing/invalid \"cmd\"");
        goto out;
    }
    cmd_str = item_cmd->valuestring;

    if (strcmp(cmd_str, "ack") == 0)
    {
        event_id = LIGHT_EVT_ACK;
    }
    else if (strcmp(cmd_str, "status") == 0)
    {
        event_id = LIGHT_EVT_STATUS;
    }
    else
    {
        ESP_LOGW(TAG, "recv: unexpected cmd \"%s\", ignore (only ack/status)", cmd_str);
        goto out;
    }

    item_name = cJSON_GetObjectItem(root, "light");
    if (!cJSON_IsString(item_name) || item_name->valuestring == NULL ||
        strlen(item_name->valuestring) == 0 ||
        strlen(item_name->valuestring) >= LC_LIGHT_NAME_LEN)
    {
        ESP_LOGW(TAG, "recv: missing/invalid \"light\"");
        goto out;
    }
    name = item_name->valuestring;

    item_ok = cJSON_GetObjectItem(root, "ok");
    if (!cJSON_IsBool(item_ok))
    {
        ESP_LOGW(TAG, "recv: missing/invalid \"ok\" (expect bool)");
        goto out;
    }
    ok    = cJSON_IsTrue(item_ok);
    state = ok ? LIGHT_STATE_ON : LIGHT_STATE_OFF;

    /* 更新缓存（持锁；回调放到锁外） */
    xSemaphoreTake(h->lock, portMAX_DELAY);
    entry = Light_Find_Entry(h, name);
    if (entry == NULL)
    {
        for (int i = 0; i < LC_TRACKED_LIGHTS; i++)
        {
            if (!h->entries[i].active)
            {
                entry = &h->entries[i];
                break;
            }
        }
        if (entry == NULL)
        {
            xSemaphoreGive(h->lock);
            ESP_LOGW(TAG, "recv: tracked table full, ignore \"%s\"", name);
            goto out;
        }
        entry->active = true;
        snprintf(entry->name, sizeof(entry->name), "%s", name);
        entry->state = LIGHT_STATE_UNKNOWN;
    }
    entry->state      = state;
    entry->last_rx_ms = (uint32_t)(esp_timer_get_time() / 1000);
    xSemaphoreGive(h->lock);

    ESP_LOGI(TAG, "%s: light=\"%s\" ok=%d state=%s",
             (event_id == LIGHT_EVT_ACK) ? "ACK" : "STATUS",
             name, ok, (state == LIGHT_STATE_ON) ? "ON" : "OFF");
    Light_Fire_Event(h, event_id, name, ok, state);

out:
    cJSON_Delete(root);
}

static void Light_Check_Timeouts(light_control_handle_t h)
{
    light_event_t evt[LC_TRACKED_LIGHTS];
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    int n = 0;

    /* 持锁改状态并收集超时事件，回调放到锁外（避免回调内查询 get_state 死锁） */
    xSemaphoreTake(h->lock, portMAX_DELAY);
    for (int i = 0; i < LC_TRACKED_LIGHTS; i++)
    {
        light_entry_t *entry = &h->entries[i];

        if (!entry->active || entry->state == LIGHT_STATE_UNKNOWN)
        {
            continue;
        }
        if ((now_ms - entry->last_rx_ms) >= h->state_timeout_ms)
        {
            ESP_LOGW(TAG, "light \"%s\" no report for %lu ms -> UNKNOWN",
                     entry->name,
                     (unsigned long)(now_ms - entry->last_rx_ms));
            entry->state = LIGHT_STATE_UNKNOWN;  /* 落回；下次上报前不再重复触发 */

            memset(&evt[n], 0, sizeof(evt[n]));
            evt[n].id = LIGHT_EVT_ERROR;
            snprintf(evt[n].data.light, sizeof(evt[n].data.light), "%s", entry->name);
            evt[n].data.ok    = false;
            evt[n].data.state = LIGHT_STATE_UNKNOWN;
            n++;
        }
    }
    xSemaphoreGive(h->lock);

    for (int i = 0; i < n; i++)
    {
        Light_Fire_Event(h, evt[i].id, evt[i].data.light,
                         evt[i].data.ok, evt[i].data.state);
    }
}

static void Light_Control_Task(void *arg)
{
    light_control_handle_t h = arg;
    light_queue_msg_t msg;
    char rxbuf[LC_RX_BUF_BYTES];
    size_t rx_len = 0;

    for (;;)
    {
        /* 1. 灯控指令出队（空转超时后继续处理收包/超时） */
        if (xQueueReceive(h->cmd_q, &msg, pdMS_TO_TICKS(LC_TASK_POLL_MS)) == pdTRUE)
        {
            if (msg.qcmd == LIGHT_QUEUE_DESTROY)
            {
                break;
            }
            Light_Send_Cmd(h, &msg);
        }

        /* 2. 轮询收灯控组播帧（ack/status；本机下发的 on/off 回环帧也会收到并打印） */
        while (wifi_manager_mcast_recv(h->chan, rxbuf, sizeof(rxbuf) - 1,
                                       &rx_len, LC_RX_POLL_MS) == ESP_OK)
        {
            rxbuf[rx_len] = '\0';
            Light_Handle_Frame(h, rxbuf, rx_len);
        }

        /* 3. 状态上报超时检测 */
        Light_Check_Timeouts(h);
    }

    xSemaphoreGive(h->destroy_done);
    vTaskDelete(NULL);
}

/* ======================== 对外 API ================================================ */

light_control_handle_t light_control_create(const light_control_cfg_t *cfg)
{
    light_control_handle_t h;

    if (cfg == NULL || cfg->chan == NULL)
    {
        ESP_LOGE(TAG, "create: cfg or chan is NULL");
        return NULL;
    }

    h = calloc(1, sizeof(*h));
    if (h == NULL)
    {
        ESP_LOGE(TAG, "create: no mem");
        return NULL;
    }

    h->chan             = cfg->chan;
    h->tx_timeout_ms    = cfg->tx_timeout_ms    ? cfg->tx_timeout_ms    : LC_DEFAULT_TX_TIMEOUT_MS;
    h->state_timeout_ms = cfg->state_timeout_ms ? cfg->state_timeout_ms : LC_DEFAULT_STATE_TIMEOUT_MS;
    h->on_event          = cfg->on_event;
    h->event_ctx         = cfg->event_ctx;

    h->cmd_q        = xQueueCreate(LC_CMD_QUEUE_LEN, sizeof(light_queue_msg_t));
    h->lock         = xSemaphoreCreateMutex();
    h->destroy_done = xSemaphoreCreateBinary();
    if (h->cmd_q == NULL || h->lock == NULL || h->destroy_done == NULL)
    {
        ESP_LOGE(TAG, "create: queue/sem create failed");
        goto fail;
    }

    if (xTaskCreatePinnedToCore(Light_Control_Task, LC_TASK_NAME, LC_TASK_STACK, h,
                                LC_TASK_PRIO, &h->task, LC_TASK_CORE) != pdPASS)
    {
        ESP_LOGE(TAG, "create: task create failed");
        goto fail;
    }

    ESP_LOGI(TAG, "created: chan=%p tx_timeout=%lu ms state_timeout=%lu ms",
             (void *)h->chan, (unsigned long)h->tx_timeout_ms,
             (unsigned long)h->state_timeout_ms);
    return h;

fail:
    if (h->cmd_q != NULL)
    {
        vQueueDelete(h->cmd_q);
    }
    if (h->lock != NULL)
    {
        vSemaphoreDelete(h->lock);
    }
    if (h->destroy_done != NULL)
    {
        vSemaphoreDelete(h->destroy_done);
    }
    free(h);
    return NULL;
}

void light_control_destroy(light_control_handle_t h)
{
    light_queue_msg_t msg;

    if (h == NULL)
    {
        return;
    }

    msg.qcmd = LIGHT_QUEUE_DESTROY;
    if (xQueueSend(h->cmd_q, &msg, pdMS_TO_TICKS(LC_CMD_QUEUE_TIMEOUT_MS)) != pdTRUE)
    {
        ESP_LOGE(TAG, "destroy: queue send timeout");
        return;
    }
    if (xSemaphoreTake(h->destroy_done, pdMS_TO_TICKS(3000)) != pdTRUE)
    {
        ESP_LOGE(TAG, "destroy: task exit timeout");
        return;
    }

    vQueueDelete(h->cmd_q);
    vSemaphoreDelete(h->lock);
    vSemaphoreDelete(h->destroy_done);
    free(h);
    ESP_LOGI(TAG, "destroy done");
}

esp_err_t light_control_send(light_control_handle_t h,
                             light_cmd_t cmd, const char *light)
{
    light_queue_msg_t msg;

    if (h == NULL || light == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (cmd != LIGHT_CMD_ON && cmd != LIGHT_CMD_OFF)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(light) == 0 || strlen(light) >= LC_LIGHT_NAME_LEN)
    {
        ESP_LOGW(TAG, "send: invalid light name \"%s\"", light);
        return ESP_ERR_INVALID_ARG;
    }

    msg.qcmd = LIGHT_QUEUE_SEND;
    msg.cmd  = cmd;
    snprintf(msg.light, sizeof(msg.light), "%s", light);
    return (xQueueSend(h->cmd_q, &msg, pdMS_TO_TICKS(LC_CMD_QUEUE_TIMEOUT_MS)) == pdTRUE)
           ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t light_control_get_state(light_control_handle_t h,
                                  const char *light,
                                  light_state_t *out)
{
    light_entry_t *entry;

    if (h == NULL || light == NULL || out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(light) == 0 || strlen(light) >= LC_LIGHT_NAME_LEN)
    {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(h->lock, portMAX_DELAY);
    entry = Light_Find_Entry(h, light);
    *out  = (entry != NULL) ? entry->state : LIGHT_STATE_UNKNOWN;
    xSemaphoreGive(h->lock);
    return ESP_OK;
}