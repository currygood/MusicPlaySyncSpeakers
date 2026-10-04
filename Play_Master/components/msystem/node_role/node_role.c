/**
 * @file node_role.c
 * @brief 节点角色与运行参数持久化模块实现（NVS 唯一管理入口）
 *
 * 实现要点（依据 docs/02-设计/nvs存储.md）：
 *   - 默认 nvs 分区、命名空间 `node_role`、Key `cfg`，整体式单 blob
 *     （node_role_cfg_t，首字段 version 结构版本号）；
 *   - node_role_init(defaults)：读取整体配置入内存缓存；首次开机 /
 *     version 不匹配 / 读取失败时用 defaults 写回并提交；
 *   - node_role_get()/set()：读写内存缓存；set() 强制 version=当前版本，
 *     单次 nvs_commit() 整体原子提交；
 *   - 互斥锁保证线程安全；本模块是全工程 NVS 持久化唯一入口，
 *     其余业务模块只经接口读写，不直接调用 NVS API。
 *
 * 接口依据（esp-idf 6.0.1）：
 *   - nvs_flash.h：nvs_flash_init / nvs_open / nvs_get_blob /
 *     nvs_set_blob / nvs_commit / nvs_close。
 */

#include "node_role.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "nvs_flash.h"

#define TAG "node_role"

/* ======================== 默认参数 ======================== */

/** blob 结构版本号（《nvs存储.md》1.2 节，字段变更时递增） */
#define NODE_ROLE_CFG_VERSION          (1)

/** NVS 命名空间 / Key（《nvs存储.md》1.1 节） */
#define NODE_ROLE_NVS_NAMESPACE        "node_role"
#define NODE_ROLE_NVS_KEY              "cfg"

/* 内置默认值：定义见 node_role.h（开发期统一定义，配网 UI 接入后可覆盖） */

/* ======================== 模块状态 ======================== */

static SemaphoreHandle_t s_lock;          /* 缓存读写互斥锁 */
static node_role_cfg_t   s_cache;         /* 内存缓存（唯一权威副本） */

/* ======================== 内部函数 ======================== */

/** 以内置默认值填充配置结构（version 由调用方/init 统一处理） */
static void node_role_fill_defaults(node_role_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->role             = NODE_ROLE_MASTER;
    snprintf(cfg->wifi_ssid,            sizeof(cfg->wifi_ssid),
             "%s", NODE_ROLE_DEFAULT_SSID);
    snprintf(cfg->wifi_password,            sizeof(cfg->wifi_password),
             "%s", NODE_ROLE_DEFAULT_PASSWORD);
    snprintf(cfg->ota_server_url,           sizeof(cfg->ota_server_url),
             "%s", NODE_ROLE_DEFAULT_OTA_URL);
    snprintf(cfg->multicast_group,          sizeof(cfg->multicast_group),
             "%s", NODE_ROLE_DEFAULT_GROUP);
    cfg->audio_sample_rate = NODE_ROLE_DEFAULT_SAMPLE_RATE;
    cfg->sync_delay_ms     = NODE_ROLE_DEFAULT_SYNC_DELAY_MS;
    cfg->volume            = NODE_ROLE_DEFAULT_VOLUME;
    cfg->play_mode         = NODE_ROLE_DEFAULT_PLAY_MODE;
}

/** 初始化 NVS 分区（esp_wifi 与本模块共同依赖，与 bt_audio 一致） */
static esp_err_t node_role_nvs_init(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

/* ======================== API ======================== */

esp_err_t node_role_init(const node_role_cfg_t *defaults)
{
    esp_err_t err;
    nvs_handle_t nv;
    node_role_cfg_t cached;
    size_t len;
    bool should_write = false;

    if (s_lock == NULL)
    {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL)
        {
            ESP_LOGE(TAG, "mutex create failed");
            return ESP_ERR_NO_MEM;
        }
    }

    /* 1. 确定本次默认配置（调用方指定或内置默认） */
    if (defaults != NULL)
    {
        memcpy(&cached, defaults, sizeof(cached));
    }
    else
    {
        node_role_fill_defaults(&cached);
    }
    cached.version = NODE_ROLE_CFG_VERSION;   /* 强制当前版本 */

    /* 2. NVS 分区就绪 */
    err = node_role_nvs_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 3. 读取整体 blob；缺失/异常时准备按 defaults 重建 */
    err = nvs_open(NODE_ROLE_NVS_NAMESPACE, NVS_READWRITE, &nv);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "nvs_open(%s) failed: %s",
                 NODE_ROLE_NVS_NAMESPACE, esp_err_to_name(err));
        return err;
    }

    len = 0;
    err = nvs_get_blob(nv, NODE_ROLE_NVS_KEY, NULL, &len);
    if (err == ESP_OK)
    {
        if (len == sizeof(cached))
        {
            err = nvs_get_blob(nv, NODE_ROLE_NVS_KEY, &cached, &len);
            if (err != ESP_OK)
            {
                ESP_LOGW(TAG, "cfg read failed (%s), rebuild defaults",
                         esp_err_to_name(err));
                node_role_fill_defaults(&cached);
                cached.version = NODE_ROLE_CFG_VERSION;
                should_write = true;
            }
            else if (cached.version != NODE_ROLE_CFG_VERSION)
            {
                ESP_LOGW(TAG, "cfg version %u != %u, rebuild with defaults",
                         (unsigned)cached.version, (unsigned)NODE_ROLE_CFG_VERSION);
                node_role_fill_defaults(&cached);
                cached.version = NODE_ROLE_CFG_VERSION;
                should_write = true;
            }
        }
        else
        {
            ESP_LOGW(TAG, "cfg len %u != %u, rebuild with defaults",
                     (unsigned)len, (unsigned)sizeof(cached));
            node_role_fill_defaults(&cached);
            cached.version = NODE_ROLE_CFG_VERSION;
            should_write = true;
        }
    }
    else if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGI(TAG, "first boot, write defaults to NVS");
        should_write = true;
    }
    else
    {
        ESP_LOGW(TAG, "cfg probe failed: %s, rebuild with defaults",
                 esp_err_to_name(err));
        should_write = true;
    }

    if (should_write)
    {
        err = nvs_set_blob(nv, NODE_ROLE_NVS_KEY, &cached, sizeof(cached));
        if (err == ESP_OK)
        {
            err = nvs_commit(nv);
        }
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "write defaults failed: %s", esp_err_to_name(err));
            nvs_close(nv);
            return err;
        }
        ESP_LOGI(TAG, "cfg initialized (role=%u ssid=\"%s\")",
                 (unsigned)cached.role, cached.wifi_ssid);
    }

    nvs_close(nv);

    /* 4. 发布到全局缓存 */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(&s_cache, &cached, sizeof(s_cache));
    xSemaphoreGive(s_lock);

    return ESP_OK;
}

esp_err_t node_role_get(node_role_cfg_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(out, &s_cache, sizeof(*out));
    xSemaphoreGive(s_lock);

    return ESP_OK;
}

esp_err_t node_role_set(const node_role_cfg_t *cfg)
{
    esp_err_t err;
    nvs_handle_t nv;

    if (cfg == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(&s_cache, cfg, sizeof(s_cache));
    s_cache.version = NODE_ROLE_CFG_VERSION;   /* 强制当前版本 */

    err = nvs_open(NODE_ROLE_NVS_NAMESPACE, NVS_READWRITE, &nv);
    if (err == ESP_OK)
    {
        err = nvs_set_blob(nv, NODE_ROLE_NVS_KEY, &s_cache, sizeof(s_cache));
        if (err == ESP_OK)
        {
            err = nvs_commit(nv);
        }
        nvs_close(nv);
    }
    xSemaphoreGive(s_lock);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "cfg saved (role=%u ssid=\"%s\")",
                 (unsigned)s_cache.role, s_cache.wifi_ssid);
    }
    else
    {
        ESP_LOGE(TAG, "cfg save failed: %s", esp_err_to_name(err));
    }

    return err;
}

node_role_t node_role_get_role(void)
{
    node_role_t role = NODE_ROLE_MASTER;

    if (s_lock != NULL)
    {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        role = (node_role_t)s_cache.role;
        xSemaphoreGive(s_lock);
    }
    return role;
}

bool node_role_is_master(void)
{
    return (node_role_get_role() == NODE_ROLE_MASTER);
}

