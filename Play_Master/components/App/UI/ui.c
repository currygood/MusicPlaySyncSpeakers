/**
 * @file ui.c
 * @brief UI 模块生命周期：硬件自举 → LVGL 移植 → 页面构建 → UI_Task
 *
 * 线程模型（UI 界面设计 2.1 / 6.8）：
 *   - 所有 lv_* 调用只发生在 UI_Task（自持任务方案，无需外部加锁）；
 *   - 外部事件回调（如 LightControl 的 SmartHome_Task 上下文调用
 *     ui_on_light_event）只做入队，由 UI_Task 循环消费后刷新界面。
 */

#include "ui_priv.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>

static const char *TAG = "UI";

static ui_cfg_t     s_cfg;              /* ui_init 保存的句柄配置（可能全 NULL） */
static bool         s_inited = false;
static TaskHandle_t s_taskHandle = NULL;
static QueueHandle_t s_lightEvtQueue = NULL;   /* 灯控事件队列（深度 8） */

/** 每个页面对应一个独立 screen（切页 = lv_screen_load 整屏重绘，无残影） */
static lv_obj_t *s_pages[UI_PAGE_COUNT] = {0};

const ui_cfg_t *ui_cfg(void)
{
    return &s_cfg;
}

/* ======================== 页面构建 ======================== */

/**
 * 构建一个页面的独立 screen：状态栏 + 内容容器 + Tab 栏（每屏一份）。
 * 内容容器保持全透明，页面内容自行绘制；切页由 lv_screen_load 整屏重绘，
 * 不存在跨页残影（修复方案 2，2026-10 确认采用）。
 */
static void ui_page_screen_build(ui_page_t page)
{
    lv_obj_t *scr = lv_obj_create(NULL);   /* 父对象 NULL = 新建 screen */
    lv_obj_t *content;

    lv_obj_set_style_bg_color(scr, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    s_pages[page] = scr;

    ui_statusbar_create(scr);

    content = lv_obj_create(scr);
    lv_obj_remove_style_all(content);
    lv_obj_set_pos(content, 0, UI_STATUSBAR_H);
    lv_obj_set_size(content, LCD_WIDTH, UI_CONTENT_H);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    switch (page)
    {
    case UI_PAGE_PLAYER:   scr_player_build(content);   break;
    case UI_PAGE_LIGHT:    scr_light_build(content);    break;
    case UI_PAGE_SETTINGS: scr_settings_build(content); break;
    case UI_PAGE_PLAYLIST: scr_playlist_build(content); break;
    default: break;
    }

    ui_tabbar_create(scr, page);
}

static void ui_pages_create(void)
{
    ui_page_screen_build(UI_PAGE_PLAYER);
    ui_page_screen_build(UI_PAGE_LIGHT);
    ui_page_screen_build(UI_PAGE_SETTINGS);
    ui_page_screen_build(UI_PAGE_PLAYLIST);

    /* 首屏加载播放页（lv_screen_load 触发整屏重绘，再加显式失效双保险） */
    lv_screen_load(s_pages[UI_PAGE_PLAYER]);
    lv_obj_invalidate(s_pages[UI_PAGE_PLAYER]);
    ui_tabbar_set_active(UI_PAGE_PLAYER);
}

void ui_nav_goto(ui_page_t page)
{
    if ((int)page < 0 || page >= UI_PAGE_COUNT || s_pages[page] == NULL)
    {
        return;
    }
    if (lv_screen_active() != s_pages[page])
    {
        ESP_LOGI(TAG, "nav -> page %d", (int)page);   /* 诊断：与画面切换时刻对照 */
        lv_screen_load(s_pages[page]);     /* 内部已 invalidate 新屏 */
        lv_obj_invalidate(s_pages[page]);  /* 双保险：显式强制整屏重绘 */
    }
    ui_tabbar_set_active(page);
}

/* ======================== UI_Task 与事件队列 ======================== */

/** UI_Task：Core 1 / 优先级 12 / 栈 13312（架构文档第 6 章任务表） */
static void ui_task(void *arg)
{
    light_event_t evt;
    (void)arg;

    ESP_LOGI(TAG, "UI_Task started (core=%d, prio=%d)",
             (int)xPortGetCoreID(), (int)uxTaskPriorityGet(NULL));

    for (;;)
    {
        /* 先排空灯控事件队列（SmartHome_Task 上下文入队），再跑 LVGL */
        while (s_lightEvtQueue != NULL &&
               xQueueReceive(s_lightEvtQueue, &evt, 0) == pdTRUE)
        {
            scr_light_apply_event(&evt);
        }

        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void ui_on_light_event(const light_event_t *evt, void *user_ctx)
{
    (void)user_ctx;
    if (evt == NULL || s_lightEvtQueue == NULL)
    {
        return;
    }
    /* SmartHome_Task 上下文：只入队，lv_* 留给 UI_Task（设计文档 6.8 约束） */
    (void)xQueueSend(s_lightEvtQueue, evt, 0);
}

/* ======================== 开机显示自检（诊断残影用，定位后置 0 关闭） ======================== */

#define UI_BOOT_SELFTEST  0   /* 自检已确认：面板/flush/字节序正常，关闭省 8s 开机时间 */

#if UI_BOOT_SELFTEST
/**
 * @brief 开机显示自检：两阶段分离故障层，每阶段 2s 并打串口日志。
 *
 *   A 阶段（绕过 LVGL，直填 LCD）：黑 → 白
 *     —— 测 LCD 驱动 + SPI + 窗口地址；若白色阶段看到黑色残影 = LCD/硬件层问题。
 *   B 阶段（走 LVGL 渲染 + flush 全管线）：黑 → 白 → 红 → 蓝
 *     —— 若白阶段见黑残影 = LVGL 背景/重绘问题（残影根因所在）；
 *     —— 红/蓝 互换成 蓝/红 = RGB565 字节序问题（lv_draw_rgb565_swap 多/少）。
 *   B 结束进 UI：播放页若透出蓝色残影 = 页面背景样式未生效（缺陷②实锤）。
 */
static void ui_boot_selftest(void)
{
    static const struct { uint32_t color; const char *name; } seq[] = {
        { 0x000000, "黑色" },
        { 0xFFFFFF, "白色" },
        { 0xFF0000, "纯红(显示成蓝=字节序反)" },
        { 0x0000FF, "纯蓝(显示成红=字节序反)" },
    };
    lv_obj_t *scr = lv_screen_active();   /* 此时尚未创建页面，用默认屏 */
    size_t i;

    ESP_LOGI(TAG, "selftest A1: LCD 直填黑色 2s（应纯黑无内容）");
    LCD_Fill_Color(RGB565(0, 0, 0));
    vTaskDelay(pdMS_TO_TICKS(2000));

    ESP_LOGI(TAG, "selftest A2: LCD 直填白色 2s（若见黑残影=LCD/硬件层）");
    LCD_Fill_Color(RGB565(255, 255, 255));
    vTaskDelay(pdMS_TO_TICKS(2000));

    for (i = 0; i < sizeof(seq) / sizeof(seq[0]); i++)
    {
        ESP_LOGI(TAG, "selftest B%u: LVGL 填 %s 2s", (unsigned)(i + 1), seq[i].name);
        lv_obj_set_style_bg_color(scr, lv_color_hex(seq[i].color), 0);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
        lv_obj_invalidate(scr);
        lv_refr_now(NULL);   /* 强制立即重绘 + flush */
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
#endif /* UI_BOOT_SELFTEST */

/* ======================== 生命周期 ======================== */

esp_err_t ui_init(const ui_cfg_t *cfg)
{
    esp_err_t ret;

    if (s_inited)
    {
        ESP_LOGW(TAG, "ui already inited");
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_cfg, 0, sizeof(s_cfg));
    if (cfg != NULL)
    {
        s_cfg = *cfg;
    }

    /* 1) 硬件自举：SD(未挂载时) → I2C 总线 → LCD/触摸。SD 失败不阻塞
     *    （页面降级为占位显示，见 UI 界面设计 8.4 容错矩阵）。 */
    ret = ui_hw_bringup();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "hw bringup failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 2) LVGL 移植：lv_init + tick + lv_fs('F') + 显示/触摸适配 */
    ret = ui_lvgl_port_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "lvgl port init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 2.5) 开机显示自检：分离 面板/SPI 层 与 LVGL 渲染层（诊断残影，定位后置 0） */
#if UI_BOOT_SELFTEST
    ui_boot_selftest();
#endif

    /* 3) 主题 + 页面构建（此时尚未创建 UI_Task，无并发触碰 LVGL） */
    ui_theme_init();
    ui_pages_create();

    /* 4) 灯控事件队列 + UI_Task（Core 1 / 12 / 13312） */
    s_lightEvtQueue = xQueueCreate(8, sizeof(light_event_t));
    if (s_lightEvtQueue == NULL)
    {
        ESP_LOGE(TAG, "light event queue create failed");
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreatePinnedToCore(ui_task, "UI_Task", 13312, NULL, 12,
                                &s_taskHandle, 1) != pdPASS)
    {
        ESP_LOGE(TAG, "UI_Task create failed");
        return ESP_ERR_NO_MEM;
    }

    s_inited = true;
    /* 固件版本标记：串口出现 multi-screen 才说明跑的是多 screen 新固件 */
    ESP_LOGI(TAG, "ui init ok (multi-screen v2): wifi=%s, light=%s, call_phone=%s",
             (s_cfg.wifi       != NULL) ? "wired" : "placeholder",
             (s_cfg.light      != NULL) ? "wired" : "placeholder",
             (s_cfg.call_phone != NULL) ? "wired" : "placeholder");
    ESP_LOGI(TAG, "free heap: internal=%lu B, psram=%lu B",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return ESP_OK;
}

void ui_deinit(void)
{
    if (!s_inited)
    {
        return;
    }
    if (s_taskHandle != NULL)
    {
        vTaskDelete(s_taskHandle);
        s_taskHandle = NULL;
    }
    if (s_lightEvtQueue != NULL)
    {
        vQueueDelete(s_lightEvtQueue);
        s_lightEvtQueue = NULL;
    }
    s_inited = false;
    ESP_LOGI(TAG, "ui deinit");
}
