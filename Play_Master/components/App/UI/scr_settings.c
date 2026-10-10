/**
 * @file scr_settings.c
 * @brief 设置页（Tab 3，可滚动；UI 界面设计 5.5）
 *
 * 分区：播放源 / 网络 / 同步播放 / 语音助手 / 固件 / 关于。
 * 第八阶段数据来源：
 *   - 真实：SD 卡状态（SD_Card_Is_Mounted()）；
 *   - 句柄注入后真实：WiFi 状态/IP（wifi_manager 查询，1s 轮询）、
 *     语音状态（call_phone_get_state()，1s 轮询）；
 *   - 源切换：music_play_set_source()（句柄注入后真实，NULL 时占位日志）；
 *   - 占位日志：配网（需扫描接口，设计文档 9.1）、同步（sync_protocol，
 *     九阶段）、OTA（十二阶段）。
 */

#include "ui_priv.h"
#include "sd_card.h"
#include "esp_log.h"

static const char *TAG = "UI";

/* 动态标签（句柄注入后由 1s 轮询刷新） */
static lv_obj_t *s_lblNetState;
static lv_obj_t *s_lblNetIp;
static lv_obj_t *s_lblMicState;

/* ======================== 文案映射 ======================== */

static const char *wifi_state_name(wifi_state_t st)
{
    switch (st)
    {
    case WIFI_STATE_IDLE:         return "未启动";
    case WIFI_STATE_CONNECTING:   return "连接中";
    case WIFI_STATE_CONNECTED:    return "已连接";
    case WIFI_STATE_DISCONNECTED: return "已断开(重连中)";
    case WIFI_STATE_ERROR:        return "错误(检查密码)";
    default:                      return "未知";
    }
}

static const char *call_state_name(call_phone_state_t st)
{
    switch (st)
    {
    case CALL_PHONE_STATE_IDLE:       return "待机";
    case CALL_PHONE_STATE_LISTENING:  return "监听唤醒词";
    case CALL_PHONE_STATE_CONNECTING: return "建立语音链路";
    case CALL_PHONE_STATE_STREAMING:  return "语音通话中";
    default:                          return "未知";
    }
}

/* ======================== 占位按键 ======================== */

static void src_bt_click_cb(lv_event_t *e)
{
    const ui_cfg_t *cfg = ui_cfg();
    (void)e;

    if (cfg->music != NULL)
    {
        esp_err_t ret = music_play_set_source(cfg->music, MUSIC_SOURCE_BT);
        ESP_LOGI(TAG, "设置页 切蓝牙源 -> music_play_set_source(BT): %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "[占位] 切蓝牙源 -> TODO: music_play_set_source(MUSIC_SOURCE_BT)（未注入句柄）");
}

static void src_local_click_cb(lv_event_t *e)
{
    const ui_cfg_t *cfg = ui_cfg();
    (void)e;

    if (cfg->music != NULL)
    {
        esp_err_t ret = music_play_set_source(cfg->music, MUSIC_SOURCE_LOCAL);
        ESP_LOGI(TAG, "设置页 切本地源 -> music_play_set_source(LOCAL): %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "[占位] 切本地源 -> TODO: music_play_set_source(MUSIC_SOURCE_LOCAL)（未注入句柄）");
}

static void wifi_cfg_click_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "[占位] WiFi 配网 -> TODO: wifi_manager_scan_start()/scan_get_results() + "
                  "node_role_set() + wifi_manager_set_sta()（扫描接口见 UI 界面设计 9.1）");
}

static void ota_click_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "[占位] 检查更新 -> TODO: OTA_Update_CheckAndUpgrade() + ui_on_ota_event()（第十二阶段接入）");
}

/* ======================== 慢轮询（1s，UI_Task 上下文） ======================== */

static void settings_timer_cb(lv_timer_t *timer)
{
    const ui_cfg_t *cfg = ui_cfg();
    char buf[64];
    (void)timer;

    if (cfg->wifi != NULL)
    {
        wifi_state_t st = WIFI_STATE_IDLE;
        if (wifi_manager_get_state(cfg->wifi, &st) == ESP_OK)
        {
            lv_obj_set_style_text_color(s_lblNetState,
                (st == WIFI_STATE_CONNECTED) ? UI_COLOR_OK : UI_COLOR_TEXT_SUB, 0);
            lv_label_set_text_fmt(s_lblNetState, "状态: %s", wifi_state_name(st));
        }
        {
            char ip[16] = {0};
            size_t len = sizeof(ip);
            if (wifi_manager_get_ip(cfg->wifi, ip, &len) == ESP_OK)
            {
                snprintf(buf, sizeof(buf), "IP: %s   RSSI: %d dBm",
                         ip, (int)wifi_manager_get_rssi(cfg->wifi));
                lv_label_set_text(s_lblNetIp, buf);
            }
        }
    }

    if (cfg->call_phone != NULL)
    {
        call_phone_state_t st = CALL_PHONE_STATE_IDLE;
        if (call_phone_get_state(cfg->call_phone, &st) == ESP_OK)
        {
            lv_label_set_text_fmt(s_lblMicState, "状态: %s", call_state_name(st));
        }
    }
}

/* ======================== 构建辅助 ======================== */

/** 分区标题 + 卡片面板，返回面板（调用方往里放行控件） */
static lv_obj_t *section_add(lv_obj_t *page, int y, const char *title, int panel_h)
{
    lv_obj_t *lbl = ui_label_create(page, title, &ui_font_cn_16, UI_COLOR_TEXT_SUB);
    lv_obj_t *panel = lv_obj_create(page);

    lv_obj_set_pos(lbl, 8, y);
    ui_style_card(panel);               /* 先剥离主题并设卡片样式（内含 remove_style_all） */
    lv_obj_set_pos(panel, 8, y + 20);   /* pos/size 属样式属性，须在其后设置 */
    lv_obj_set_size(panel, LCD_WIDTH - 16, panel_h);
    lv_obj_set_style_radius(panel, 6, 0);
    return panel;
}

/** 文字按钮（占位按键统一外观） */
static lv_obj_t *text_btn(lv_obj_t *parent, int x, int y, int w, int h,
                          const char *text, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_t *lbl;

    lv_obj_remove_style_all(btn);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, UI_COLOR_PANEL_HI, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_bg_color(btn, UI_COLOR_PRIMARY, LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lbl = ui_label_create(btn, text, &ui_font_cn_16, UI_COLOR_TEXT);
    lv_obj_center(lbl);
    return btn;
}

void scr_settings_build(lv_obj_t *page)
{
    const ui_cfg_t *cfg = ui_cfg();
    lv_obj_t *panel;
    lv_obj_t *lbl;
    int y = 4;

    /* 页面允许竖向滚动（内容高约 500 > 176），隐藏滚动条保持整洁 */
    lv_obj_add_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);

    /* ---- 1. 播放源 ---- */
    panel = section_add(page, y, "播放源", 44);
    text_btn(panel, 8, 6, 70, 32, "蓝牙", src_bt_click_cb);
    text_btn(panel, 86, 6, 70, 32, "本地", src_local_click_cb);
    lbl = ui_label_create(panel, "蓝牙连接期间自动优先蓝牙模式",
                          &ui_font_cn_16, UI_COLOR_DISABLED);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 166, 0);
    y += 20 + 44 + 8;

    /* ---- 2. 网络 ---- */
    panel = section_add(page, y, "网络", 70);
    s_lblNetState = ui_label_create(panel,
        (cfg->wifi != NULL) ? "状态: --" : "状态: 未接线（传入 wifi 句柄后生效）",
        &ui_font_cn_16, UI_COLOR_TEXT_SUB);
    lv_obj_set_pos(s_lblNetState, 8, 8);
    s_lblNetIp = ui_label_create(panel, "IP: --", &ui_font_cn_16, UI_COLOR_TEXT_SUB);
    lv_obj_set_pos(s_lblNetIp, 8, 40);
    text_btn(panel, 204, 20, 84, 30, "WiFi 配网", wifi_cfg_click_cb);
    y += 20 + 70 + 8;

    /* ---- 3. 同步播放 ---- */
    panel = section_add(page, y, "同步播放", 40);
    lbl = ui_label_create(panel, "从节点: 未启用（第九阶段 sync_protocol 接入）",
                          &ui_font_cn_16, UI_COLOR_TEXT_SUB);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 8, 0);
    y += 20 + 40 + 8;

    /* ---- 4. 语音助手 ---- */
    panel = section_add(page, y, "语音助手", 58);
    s_lblMicState = ui_label_create(panel,
        (cfg->call_phone != NULL) ? "状态: --" : "状态: 未接线（传入 call_phone 句柄后生效）",
        &ui_font_cn_16, UI_COLOR_TEXT_SUB);
    lv_obj_set_pos(s_lblMicState, 8, 8);
    lbl = ui_label_create(panel, "唤醒词: 你好小智", &ui_font_cn_16, UI_COLOR_DISABLED);
    lv_obj_set_pos(lbl, 8, 32);
    y += 20 + 58 + 8;

    /* ---- 5. 固件 ---- */
    panel = section_add(page, y, "固件", 44);
    lbl = ui_label_create(panel, "版本: 待 OTA 接入（第十二阶段）",
                          &ui_font_cn_16, UI_COLOR_TEXT_SUB);
    lv_obj_set_pos(lbl, 8, 12);
    text_btn(panel, 204, 7, 84, 30, "检查更新", ota_click_cb);
    y += 20 + 44 + 8;

    /* ---- 6. 关于 ---- */
    panel = section_add(page, y, "关于", 56);
    lbl = ui_label_create(panel,
        SD_Card_Is_Mounted() ? "SD 卡: 已挂载" : "SD 卡: 未挂载",
        &ui_font_cn_16, UI_COLOR_TEXT_SUB);
    lv_obj_set_pos(lbl, 8, 6);
    lbl = ui_label_create(panel, "LVGL 9.6 · 第八阶段界面联调",
                          &ui_font_cn_16, UI_COLOR_DISABLED);
    lv_obj_set_pos(lbl, 8, 30);

    lv_timer_create(settings_timer_cb, 1000, NULL);
}
