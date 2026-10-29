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
#include "ui_font.h"    /* 运行时中文字体（FreeType） */
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <string.h>

static ui_cfg_t     s_cfg;              /* ui_init 保存的句柄配置（可能全 NULL） */
static bool         s_inited = false;
static TaskHandle_t s_taskHandle = NULL;
static QueueHandle_t s_lightEvtQueue = NULL;   /* 灯控事件队列（深度 8） */
static QueueHandle_t s_musicEvtQueue = NULL;   /* 播放事件队列（深度 8） */
static QueueHandle_t s_otaEvtQueue = NULL;     /* OTA 事件队列（深度 4） */

/** 每个页面对应一个独立 screen（切页 = lv_screen_load 整屏重绘，无残影） */
static lv_obj_t *s_pages[UI_PAGE_COUNT] = {0};

/* PHASE2 临时打点：切页到首帧完成的耗时（Phase 6 长稳测试后删除） */
static int64_t s_navT0 = -1;
static int     s_navPage = -1;

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

    /* 运行期字体不能设成 LV_FONT_DEFAULT（那是编译期常量），
     * 这里把 16px 中文字体挂在 screen 根对象上，靠继承覆盖那些"没有显式指定字体"
     * 的控件：lv_textarea 的 placeholder、lv_keyboard 按键、主题生成的部件等。
     * 显式传了字体的 label 不受影响。 */
    {
        lv_font_t *base = ui_font_get(UI_FONT_TEXT_16);
        if (base != NULL)
        {
            lv_obj_set_style_text_font(scr, base, 0);
        }
    }
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
    /* 每建完一页让出 2ms。整段建页累计约 2.5s（每页 ~600ms），本任务在 CPU0
     * prio=10，期间 WiFi 的 ppTask（prio 23）随传随到，IDLE0 反而一直排不上队
     * → 开机日志里出现 `task_wdt: IDLE0 did not reset`（该告警未开 PANIC，
     *   只打印不重启，属噪声，但会掩盖以后的真问题）。
     * 让出 2ms 而不是 1ms：1ms 时 WiFi 的 TX burst 往往还没结束，IDLE0 仍轮不到。 */
    ui_page_screen_build(UI_PAGE_PLAYER);
    vTaskDelay(2);
    ui_page_screen_build(UI_PAGE_LIGHT);
    vTaskDelay(2);
    ui_page_screen_build(UI_PAGE_SETTINGS);
    vTaskDelay(2);
    ui_page_screen_build(UI_PAGE_PLAYLIST);
    vTaskDelay(2);

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
        UI_LOGI( "nav -> page %d", (int)page);   /* 诊断：与画面切换时刻对照 */
        ui_disp_stat_take(NULL);                   /* 清零统计，只统计本次切页 */
        s_navT0   = esp_timer_get_time();        /* PHASE2 打点：切页起点 */
        s_navPage = (int)page;
        lv_screen_load(s_pages[page]);     /* 内部已 invalidate 新屏 */
        lv_obj_invalidate(s_pages[page]);  /* 双保险：显式强制整屏重绘 */
    }
    ui_tabbar_set_active(page);
}

/* ======================== 栈余量诊断 ========================
 * ESP-IDF 的 uxTaskGetStackHighWaterMark() 返回**字节**（不是标准 FreeRTOS 的字），
 * 且是"自任务创建以来的最小剩余"——只会变小，所以在多个点采样就能拿到峰值。
 * headroom = 返回值；used = 栈大小 - 返回值。 */
static void ui_log_stack(const char *when)
{
    const unsigned stack  = (unsigned)UI_TASK_STACK_BYTES;
    const unsigned free_b = (unsigned)uxTaskGetStackHighWaterMark(NULL);

    if (free_b > stack) {
        ESP_LOGW("UI", "stack[%s]: hwm=%u > stack=%u (unexpected unit), skip",
                 when, free_b, stack);
        return;
    }
    ESP_LOGI("UI", "stack[%s]: headroom=%u B, used<=%u B, stack=%u B",
             when, free_b, stack - free_b, stack);
}

/* ======================== 栈深度变化监视（诊断） ========================
 * uxTaskGetStackHighWaterMark 是"历史最小剩余"，只会单调变浅。
 * 因此每次调用只要发现它变深了，就说明"刚跑完的那段代码"刷新了最深记录。
 * 用它做二分：先判断 23.4KB 是不是整个 lv_timer_handler（渲染 + 全部 lv_timer），
 * 是的话再往内逐个回调收窄。找到后就删掉这段。
 * 代价：hwm 要扫一遍未使用的填充字节（32KB 量级），所以只在诊断期间开。 */
static unsigned s_hwmUsed;

static void ui_deep_watch(const char *tag)
{
    unsigned stack = (unsigned)UI_TASK_STACK_BYTES;
    unsigned freeB = (unsigned)uxTaskGetStackHighWaterMark(NULL);
    unsigned used;

    if (freeB > stack) {
        return;
    }
    used = stack - freeB;
    if (used > s_hwmUsed + 256U) {          /* 只报明显变深，避免刷屏 */
        ESP_LOGI("UI", "[DEEP] %-24s used=%u B (+%u)", tag, used, used - s_hwmUsed);
        s_hwmUsed = used;
    }
}

/* 一屏真正刷完的时刻（LVGL 9 的 LV_EVENT_REFR_READY）。
 * 用它统计"切页→首帧完成"，比 lv_refr_now() 好：不阻塞、不饿死同核其它任务。 */
static void ui_on_render_ready(lv_event_t *e)
{
    ui_disp_stat_t st;
    int64_t total;
    uint32_t drawAvg = 0;
    (void)e;

    if (s_navT0 < 0) {
        return;
    }
    total = esp_timer_get_time() - s_navT0;
    ui_disp_stat_take(&st);

    /* 一屏 = st.flush_calls 个渲染窗口（PARTIAL 模式按缓冲高度切段）。
     *   flush_us   : SPI 物理传输，不可压缩（15MHz 下全屏理论下限 ~82ms）
     *   render_us  : 各窗口之间空档累计 = LVGL 软件绘制（可优化）
     *   render_max : 单个窗口最慢值。若 max 远大于平均 → 某几段有重对象/文字；
     *                若 max ≈ 平均 → 每段固定开销（对象树重遍历）占主导。
     * 注意 depth_peak 的口径：flush 回调是在"这一块已画完"之后才被调用，
     * 那时绘制栈早已展开，所以它只反映 flush 自身深度（~1.7KB），不是渲染峰值。 */
    if (st.render_count > 0) {
        drawAvg = st.render_us / st.render_count;
    }
    ESP_LOGI("UI", "[REFR] page %d total %lld us | flush %u us / %u win | "
             "draw %u us / %u win (avg %u, max %u) | flush depth %u B",
             s_navPage, (long long)total, (unsigned)st.flush_us, (unsigned)st.flush_calls,
             (unsigned)st.render_us, (unsigned)st.render_count,
             (unsigned)drawAvg, (unsigned)st.render_max_us, (unsigned)st.depth_peak);
    ui_log_stack("after render");
    s_navT0   = -1;
    s_navPage = -1;
}

/* ======================== 降级：英文错误页 ========================
 * 字体链路任一环失败（storage 挂载失败 / 文件缺失 / 尺寸或魔数不符 / PSRAM 不足 /
 * FreeType 创建失败）时，**不构建 4 个中文页面**——固件里已经没有任何中文字库，
 * 硬撑下去只会满屏 □，比明确报错更难查。
 * 这里用 LVGL 内置 Montserrat 14（纯 ASCII）把失败原因摊在屏上，现场就能看出是哪一环。
 * 注意：不引入任何中文字库来"兜底"，这是刻意设计。 */
static void ui_show_fatal_error(const char *err_name, const char *detail)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_t *lbl;

    if (scr == NULL) {
        ESP_LOGE("UI", "no active screen; cannot render error page");
        return;
    }

    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lbl = lv_label_create(scr);
    lv_label_set_text(lbl, "UI FONT ERROR");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, UI_COLOR_ERR, 0);
    lv_obj_set_pos(lbl, 8, 10);

    lbl = lv_label_create(scr);
    lv_label_set_text(lbl, err_name != NULL ? err_name : "-");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, UI_COLOR_TEXT, 0);
    lv_obj_set_pos(lbl, 8, 34);

    lbl = lv_label_create(scr);
    lv_label_set_text(lbl, detail != NULL ? detail : "-");
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, LCD_WIDTH - 16);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, UI_COLOR_TEXT_SUB, 0);
    lv_obj_set_pos(lbl, 8, 56);

    lbl = lv_label_create(scr);
    lv_label_set_text(lbl, "Font: /storage/font/ui_font.ttf\n"
                           "Fix the storage partition (reflash full image)\n"
                           "or the font file, then reboot.");
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, LCD_WIDTH - 16);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, UI_COLOR_DISABLED, 0);
    lv_obj_set_pos(lbl, 8, 120);

    ESP_LOGE("UI", "fatal: %s | %s (English error page shown, UI pages NOT built)",
             err_name != NULL ? err_name : "-", detail != NULL ? detail : "-");
}

/* ======================== UI_Task 与事件队列 ======================== */

/**
 * UI 任务主体：由 app_main 最早创建的那个任务进入（原因见 ui.h/ui_run）。
 * 主题与页面构建也放在这里，避免 LVGL 小对象在任务栈分配前先吃光内部 RAM。
 */
void ui_run(void)
{
    light_event_t evt;
    music_event_t mevt;

    s_taskHandle = xTaskGetCurrentTaskHandle();

    /* 栈深基准：必须在建字体/建页面之前采，此时栈基本是空的。
     * 后面 flush 回调里读 SP 与这个基准相减，就是渲染期的真实峰值用量
     * （uxTaskGetStackHighWaterMark 会因大块"只写一部分"的局部缓冲而虚报）。 */
    ui_disp_sp_probe_init();

    UI_LOGI( "UI task running (core=%d, prio=%d, hwm_words=%u)",
             (int)xPortGetCoreID(), (int)uxTaskPriorityGet(NULL),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));

    /* 中文字体：必须在 lv_init() 之后、页面构建之前。
     * 内部会把 /storage/font/ui_font.ttf 整块预载进 PSRAM（避免 SPIFFS 随机读把
     * UI_Task 卡到看门狗），再创建 12/16/20 三档字号的 FreeType 字体。
     * 失败则切英文错误页并停在这里：固件里没有中文字库可用，继续建页面只会满屏 □。 */
    {
        int64_t t0 = esp_timer_get_time();
        esp_err_t ferr = ui_font_init();
        ESP_LOGI("UIFONT", "ui_font_init -> %s (%lld ms)%s%s",
                 esp_err_to_name(ferr), (long long)((esp_timer_get_time() - t0) / 1000),
                 (ferr == ESP_OK) ? "" : ", reason: ",
                 (ferr == ESP_OK) ? "" : ui_font_last_error());

        if (ferr != ESP_OK)
        {
            ui_show_fatal_error(esp_err_to_name(ferr), ui_font_last_error());
            /* LVGL 还要继续跑，否则错误页根本刷新不出去；
             * 事件队列不再排空——此时业务回调引用的控件都不存在。 */
            for (;;)
            {
                lv_timer_handler();
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
    }

    ui_theme_init();
    ui_pages_create();

    /* 注册"整屏刷完"回调，用于统计切页耗时 + 采样渲染期栈峰值（不阻塞） */
    {
        lv_display_t *disp = lv_display_get_default();
        if (disp != NULL) {
            lv_display_add_event_cb(disp, ui_on_render_ready, LV_EVENT_REFR_READY, NULL);
        } else {
            ESP_LOGW("UI", "no default display, render timing disabled");
        }
    }

    /* 栈水位必须在 4 个页面全部构建完之后再采一次：
     * 上面那次是"空任务"水位，测不到布局/LVGL 对象创建的真实峰值。 */
    ui_log_stack("after pages built");
    ui_deep_watch("base after pages");

    for (;;)
    {
        /* 先排空灯控事件队列（SmartHome_Task 上下文入队），再跑 LVGL */
        while (s_lightEvtQueue != NULL &&
               xQueueReceive(s_lightEvtQueue, &evt, 0) == pdTRUE)
        {
            scr_light_apply_event(&evt);
        }

        /* 再排空播放事件队列（MusicPlay_Task 上下文入队），刷新播放页源 chip 等 */
        while (s_musicEvtQueue != NULL &&
               xQueueReceive(s_musicEvtQueue, &mevt, 0) == pdTRUE)
        {
            scr_player_apply_event(&mevt);
            scr_playlist_apply_event(&mevt);
        }

        /* 排空 OTA 事件队列（ota_sync_task 上下文入队），刷新设置页固件状态 */
        {
            ota_update_event_info_t oinfo;
            while (s_otaEvtQueue != NULL &&
                   xQueueReceive(s_otaEvtQueue, &oinfo, 0) == pdTRUE)
            {
                scr_settings_apply_ota_event(&oinfo);
            }
        }

        /* 注意：这里绝对不能调 lv_refr_now() 强制同步刷完。
         * 一屏 320x240 走 10MHz SPI 光传输就 ~123ms，加上首次字形光栅化，
         * 一次性渲染完实测 744ms —— 期间 UI_Task（core0/prio10）完全不让 CPU，
         * 把同核的 wifi_mcast_rx（prio6）饿死，实测直接把它的栈顶穿
         * （stack overflow in task wifi_mcast_rx）。
         * 切页耗时改由 display 的 LV_EVENT_REFR_READY 回调统计，零阻塞。 */
        lv_timer_handler();
        ui_deep_watch("lv_timer_handler");
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

void ui_on_music_event(const music_event_t *evt, void *user_ctx)
{
    (void)user_ctx;
    if (evt == NULL || s_musicEvtQueue == NULL)
    {
        return;
    }
    /* MusicPlay_Task 上下文：只入队，lv_* 留给 UI_Task（设计文档 6.8 约束） */
    (void)xQueueSend(s_musicEvtQueue, evt, 0);
}

void ui_on_ota_event(const ota_update_event_info_t *info)
{
    if (info == NULL || s_otaEvtQueue == NULL)
    {
        return;
    }
    /* ota_sync_task 上下文：只入队，lv_* 留给 UI_Task */
    (void)xQueueSend(s_otaEvtQueue, info, 0);
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

    UI_LOGI( "selftest A1: LCD 直填黑色 2s（应纯黑无内容）");
    LCD_Fill_Color(RGB565(0, 0, 0));
    vTaskDelay(pdMS_TO_TICKS(2000));

    UI_LOGI( "selftest A2: LCD 直填白色 2s（若见黑残影=LCD/硬件层）");
    LCD_Fill_Color(RGB565(255, 255, 255));
    vTaskDelay(pdMS_TO_TICKS(2000));

    for (i = 0; i < sizeof(seq) / sizeof(seq[0]); i++)
    {
        UI_LOGI( "selftest B%u: LVGL 填 %s 2s", (unsigned)(i + 1), seq[i].name);
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
    int64_t   t_stage;

    if (s_inited)
    {
        UI_LOGW( "ui already inited");
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_cfg, 0, sizeof(s_cfg));
    if (cfg != NULL)
    {
        s_cfg = *cfg;
    }

    /* 1) 硬件自举：SD(未挂载时) → I2C 总线 → LCD/触摸。SD 失败不阻塞
     *    （页面降级为占位显示，见 UI 界面设计 8.4 容错矩阵）。 */
    t_stage = esp_timer_get_time();
    ret = ui_hw_bringup();
    if (ret != ESP_OK)
    {
        UI_LOGE( "hw bringup failed: %s", esp_err_to_name(ret));
        return ret;
    }
    UI_LOGI( "stage: hw bringup ok in %u ms",
             (unsigned)((esp_timer_get_time() - t_stage) / 1000));

    /* 2) LVGL 移植：lv_init + tick + lv_fs('F') + 显示/触摸适配 */
    t_stage = esp_timer_get_time();
    ret = ui_lvgl_port_init();
    if (ret != ESP_OK)
    {
        UI_LOGE( "lvgl port init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    UI_LOGI( "stage: lvgl port ok in %u ms",
             (unsigned)((esp_timer_get_time() - t_stage) / 1000));

    /* 2.5) 开机显示自检：分离 面板/SPI 层 与 LVGL 渲染层（诊断残影，定位后置 0） */
#if UI_BOOT_SELFTEST
    ui_boot_selftest();
#endif

    /* 3) 主题 + 页面构建在 ui_run() 里做：LVGL 的 widget/style 都是几百字节的
     *    小块 malloc。sdkconfig 里 SPIRAM_MALLOC_ALWAYSINTERNAL=0，即所有普通
     *    malloc 优先落 PSRAM（仅 PSRAM 耗尽或显式 MALLOC_CAP_DMA/INTERNAL 才走
     *    内部），故 LVGL 对象/图片缓存均走 PSRAM；内部 DRAM 留给 flush 缓冲、
     *    任务栈与 DMA。先让 UI 任务栈（app_main 最先创建）占住内部堆。 */

    /* 4) 灯控/播放事件队列（PSRAM：内部 DRAM 留给任务栈与 DMA） */
    s_lightEvtQueue = xQueueCreateWithCaps(8, sizeof(light_event_t), MALLOC_CAP_SPIRAM);
    s_musicEvtQueue = xQueueCreateWithCaps(8, sizeof(music_event_t), MALLOC_CAP_SPIRAM);
    s_otaEvtQueue   = xQueueCreateWithCaps(4, sizeof(ota_update_event_info_t), MALLOC_CAP_SPIRAM);
    if (s_lightEvtQueue == NULL || s_musicEvtQueue == NULL)
    {
        UI_LOGE( "event queue create failed");
        return ESP_ERR_NO_MEM;
    }

    s_inited = true;
    /* 固件版本标记：串口出现 multi-screen 才说明跑的是多 screen 新固件 */
    UI_LOGI( "ui init ok (multi-screen v2): wifi=%s, light=%s, music=%s",
             (s_cfg.wifi       != NULL) ? "wired" : "placeholder",
             (s_cfg.light      != NULL) ? "wired" : "placeholder",
             (s_cfg.music      != NULL) ? "wired" : "placeholder");
    UI_LOGI( "free heap: internal=%lu B, psram=%lu B",
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
        vQueueDeleteWithCaps(s_lightEvtQueue);
        s_lightEvtQueue = NULL;
    }
    if (s_musicEvtQueue != NULL)
    {
        vQueueDeleteWithCaps(s_musicEvtQueue);
        s_musicEvtQueue = NULL;
    }
    if (s_otaEvtQueue != NULL)
    {
        vQueueDeleteWithCaps(s_otaEvtQueue);
        s_otaEvtQueue = NULL;
    }
    s_inited = false;
    UI_LOGI( "ui deinit");
}