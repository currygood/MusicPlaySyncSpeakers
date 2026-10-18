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
#include "node_role.h"
#include "wifi_manager.h"
#include "OTA_Update.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 动态标签（句柄注入后由 1s 轮询刷新） */
static lv_obj_t *s_lblNetState;
static lv_obj_t *s_lblNetIp;
static lv_obj_t *s_lblMicState;
static lv_obj_t *s_lblFwVersion;  /* 固件版本标签 */
static lv_obj_t *s_btnOta;        /* "检查更新"按钮（OTA 期间禁用） */

/* WiFi 配网对话框状态 */
static lv_obj_t   *s_wifiDlg       = NULL;   /* 配网主对话框 */
static lv_obj_t   *s_wifiList      = NULL;   /* AP 列表容器 */
static lv_obj_t   *s_wifiPwdDlg    = NULL;   /* 密码输入对话框 */
static lv_obj_t   *s_wifiPwdTa     = NULL;   /* 密码输入框 */
static lv_obj_t   *s_wifiStatusLbl = NULL;   /* 状态提示标签 */
static char        s_selectedSsid[33] = {0}; /* 当前选中的 SSID */
static uint8_t     s_selectedAuth  = 0;      /* 当前选中的认证方式 */
static wifi_scan_result_t *s_scanResult = NULL;  /* 堆分配扫描结果，对话框生命周期内有效 */

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
        UI_LOGI( "设置页 切蓝牙源 -> music_play_set_source(BT): %s", esp_err_to_name(ret));
        return;
    }
    UI_LOGI( "[占位] 切蓝牙源 -> TODO: music_play_set_source(MUSIC_SOURCE_BT)（未注入句柄）");
}

static void src_local_click_cb(lv_event_t *e)
{
    const ui_cfg_t *cfg = ui_cfg();
    (void)e;

    if (cfg->music != NULL)
    {
        esp_err_t ret = music_play_set_source(cfg->music, MUSIC_SOURCE_LOCAL);
        UI_LOGI( "设置页 切本地源 -> music_play_set_source(LOCAL): %s", esp_err_to_name(ret));
        return;
    }
    UI_LOGI( "[占位] 切本地源 -> TODO: music_play_set_source(MUSIC_SOURCE_LOCAL)（未注入句柄）");
}

/* ======================== WiFi 配网对话框（前向声明） ======================== */

static void wifi_config_dlg_create(wifi_manager_handle_t wifi);
static void wifi_pwd_dlg_create(const char *ssid);
static void wifi_connect_do(const char *password);

/* 回调函数（C语言不支持lambda，必须使用普通函数） */
static void wifi_dlg_close_cb(lv_event_t *e);
static void wifi_pwd_close_cb(lv_event_t *e);
static void wifi_connect_btn_cb(lv_event_t *e);
static void wifi_pwd_kb_ready_cb(lv_event_t *e);
static void wifi_dlg_close_timer_cb(lv_timer_t *timer);

/* ======================== WiFi 配网实现 ======================== */

static void wifi_cfg_click_cb(lv_event_t *e)
{
    const ui_cfg_t *cfg = ui_cfg();
    (void)e;

    if (cfg->wifi == NULL)
    {
        UI_LOGW( "WiFi 配网：wifi 句柄未注入");
        return;
    }

    if (s_wifiDlg != NULL)
    {
        lv_obj_del(s_wifiDlg);
        s_wifiDlg = NULL;
    }

    wifi_config_dlg_create(cfg->wifi);
}

/** 认证方式转显示文本 */
static const char *auth_mode_name(uint8_t auth)
{
    switch (auth)
    {
    case WIFI_AUTH_OPEN:           return "";
    case WIFI_AUTH_WEP:            return "WEP";
    case WIFI_AUTH_WPA_PSK:        return "WPA";
    case WIFI_AUTH_WPA2_PSK:       return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:   return "WPA/WPA2";
    case WIFI_AUTH_WPA3_PSK:       return "WPA3";
    default:                       return "?";
    }
}

/** RSSI 转信号强度图标（简化版） */
static const char *rssi_icon(int8_t rssi)
{
    if (rssi >= -50)  return "\xee\xa2\xa8";  /* 强 */
    if (rssi >= -65)  return "\xee\xa2\xa7";  /* 中 */
    return "\xee\xa2\xa6";                     /* 弱 */
}

/** AP 列表项点击回调：选中一个 AP */
static void wifi_ap_click_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    uint16_t idx = (uint16_t)(uintptr_t)lv_obj_get_user_data(btn);
    wifi_scan_result_t *result = (wifi_scan_result_t *)lv_event_get_user_data(e);

    if (result == NULL || idx >= result->count)
    {
        return;
    }

    snprintf(s_selectedSsid, sizeof(s_selectedSsid), "%s", result->ap_list[idx].ssid);
    s_selectedAuth = result->ap_list[idx].authmode;  /* ESP-IDF 6.0.1 字段名 */

    UI_LOGI( "WiFi 配网：选中 AP \"%s\" (auth=%u)",
             s_selectedSsid, (unsigned)s_selectedAuth);

    /* 如果是开放网络，直接连接；否则弹出密码输入框 */
    if (s_selectedAuth == WIFI_AUTH_OPEN)
    {
        wifi_connect_do("");
    }
    else
    {
        wifi_pwd_dlg_create(s_selectedSsid);
    }
}

/** 对话框关闭按钮回调 */
static void wifi_dlg_close_cb(lv_event_t *e)
{
    (void)e;
    if (s_wifiDlg != NULL)
    {
        lv_obj_del(s_wifiDlg);
        s_wifiDlg = NULL;
    }

    /* 释放堆上的扫描结果副本 */
    if (s_scanResult != NULL)
    {
        wifi_manager_scan_free(s_scanResult);
        free(s_scanResult);
        s_scanResult = NULL;
    }
}

/** 创建 WiFi 扫描列表对话框 */
static void wifi_config_dlg_create(wifi_manager_handle_t wifi)
{
    lv_obj_t *dlg, *lbl, *list, *btn;
    wifi_scan_result_t scan_result = {0};
    esp_err_t err;
    uint16_t i;

    /* 0) 防御性释放旧的堆副本 */
    if (s_scanResult != NULL)
    {
        wifi_manager_scan_free(s_scanResult);
        free(s_scanResult);
        s_scanResult = NULL;
    }

    /* 1) 创建对话框背景 */
    dlg = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(dlg);
    lv_obj_set_size(dlg, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_style_bg_color(dlg, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(dlg, LV_OPA_60, 0);
    lv_obj_center(dlg);
    s_wifiDlg = dlg;

    /* 2) 标题栏 */
    lbl = ui_label_create(dlg, "WiFi 配网", &ui_font_cn_16, UI_COLOR_TEXT);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 10);

    /* 3) 状态提示（扫描中/出错/无结果） */
    s_wifiStatusLbl = ui_label_create(dlg, "正在扫描...", &ui_font_cn_16, UI_COLOR_ACCENT);
    lv_obj_align(s_wifiStatusLbl, LV_ALIGN_TOP_MID, 0, 40);

    /* 4) AP 列表容器（可滚动） */
    list = lv_obj_create(dlg);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, LCD_WIDTH - 40, LCD_HEIGHT - 120);
    lv_obj_set_pos(list, 20, 70);
    lv_obj_set_style_bg_color(list, UI_COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(list, 8, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_scrollable(list, true);           /* 替代废弃的 add_flag */
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 2, 0);
    lv_obj_set_style_pad_column(list, 0, 0);
    s_wifiList = list;

    /* 5) 关闭按钮 */
    btn = lv_button_create(dlg);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 80, 32);
    lv_obj_set_pos(btn, (LCD_WIDTH - 80) / 2, LCD_HEIGHT - 50);
    lv_obj_set_style_bg_color(btn, UI_COLOR_DISABLED, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lbl = ui_label_create(btn, "取消", &ui_font_cn_16, UI_COLOR_TEXT);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(btn, wifi_dlg_close_cb, LV_EVENT_CLICKED, NULL);

    /* 6) 启动扫描（阻塞 3~5 秒，在当前 UI 任务上下文执行） */
    err = wifi_manager_scan(wifi, &scan_result);
    if (err != ESP_OK)
    {
        lv_label_set_text_fmt(s_wifiStatusLbl, "扫描失败: %s", esp_err_to_name(err));
        return;
    }
    if (scan_result.count == 0)
    {
        lv_label_set_text(s_wifiStatusLbl, "未找到 WiFi 网络");
        wifi_manager_scan_free(&scan_result);
        return;
    }

    /* 将扫描结果浅拷贝到堆上，避免栈变量悬空 */
    s_scanResult = malloc(sizeof(wifi_scan_result_t));
    if (s_scanResult == NULL)
    {
        lv_label_set_text(s_wifiStatusLbl, "内存不足");
        wifi_manager_scan_free(&scan_result);
        return;
    }
    memcpy(s_scanResult, &scan_result, sizeof(wifi_scan_result_t));

    /* 7) 填充 AP 列表 */
    lv_label_set_text(s_wifiStatusLbl, "");
    for (i = 0; i < scan_result.count; i++)
    {
        wifi_ap_record_t *ap = &scan_result.ap_list[i];
        lv_obj_t *row, *icon_lbl, *ssid_lbl, *auth_lbl;
        char buf[64];

        /* 每行：信号图标 + SSID + 认证方式 */
        row = lv_button_create(list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), 36);
        lv_obj_set_style_bg_color(row, UI_COLOR_PANEL_HI, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 4, 0);
        lv_obj_set_style_pad_row(row, 0, 0);
        lv_obj_set_style_pad_column(row, 0, 0);
        lv_obj_set_scrollable(row, false);      /* 替代废弃的 clear_flag */
        lv_obj_set_user_data(row, (void *)(uintptr_t)i);
        lv_obj_add_event_cb(row, wifi_ap_click_cb, LV_EVENT_CLICKED, s_scanResult);

        /* 信号强度图标 */
        icon_lbl = ui_label_create(row, rssi_icon(ap->rssi), &ui_icons_16, UI_COLOR_TEXT);
        lv_obj_align(icon_lbl, LV_ALIGN_LEFT_MID, 4, 0);

        /* SSID 名称 */
        {
            const char *ssid_text;
            if (ap->ssid[0])
            {
                ssid_text = (const char *)ap->ssid;
            }
            else
            {
                ssid_text = "(隐藏)";
            }
            ssid_lbl = ui_label_create(row, ssid_text,
                                       &ui_font_cn_16, UI_COLOR_TEXT);
        }
        lv_obj_align(ssid_lbl, LV_ALIGN_LEFT_MID, 28, 0);

        /* 认证方式 */
        snprintf(buf, sizeof(buf), "%s", auth_mode_name(ap->authmode));  /* ESP-IDF 6.0.1 字段名 */
        auth_lbl = ui_label_create(row, buf, &ui_font_cn_16, UI_COLOR_DISABLED);  /* 使用已有字体 */
        lv_obj_align(auth_lbl, LV_ALIGN_RIGHT_MID, -8, 0);
    }
}

/** 密码对话框关闭按钮回调 */
static void wifi_pwd_close_cb(lv_event_t *e)
{
    (void)e;
    if (s_wifiPwdDlg != NULL)
    {
        lv_obj_del(s_wifiPwdDlg);
        s_wifiPwdDlg = NULL;
        s_wifiPwdTa = NULL;
    }
}

/** 键盘 OK 按钮回调：获取密码并发起连接 */
static void wifi_pwd_kb_ready_cb(lv_event_t *e)
{
    (void)e;
    if (s_wifiPwdTa != NULL)
    {
        const char *pwd = lv_textarea_get_text(s_wifiPwdTa);
        wifi_connect_do(pwd ? pwd : "");
    }
}

/** 密码输入对话框（带屏幕键盘） */
static void wifi_pwd_dlg_create(const char *ssid)
{
    lv_obj_t *dlg, *lbl, *ta, *kb;

    if (s_wifiPwdDlg != NULL)
    {
        lv_obj_del(s_wifiPwdDlg);
        s_wifiPwdDlg = NULL;
    }

    /* 对话框背景 */
    dlg = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(dlg);
    lv_obj_set_size(dlg, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_style_bg_color(dlg, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(dlg, LV_OPA_60, 0);
    lv_obj_center(dlg);
    s_wifiPwdDlg = dlg;

    /* 标题 */
    lbl = ui_label_create(dlg, "输入密码", &ui_font_cn_16, UI_COLOR_TEXT);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 5);

    /* SSID 提示 */
    {
        char ssid_buf[64];
        snprintf(ssid_buf, sizeof(ssid_buf), "网络: %s", ssid);
        lbl = ui_label_create(dlg, ssid_buf, &ui_font_cn_16, UI_COLOR_TEXT_SUB);
    }
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 25);

    /* 密码输入框 */
    ta = lv_textarea_create(dlg);
    lv_obj_set_size(ta, LCD_WIDTH - 40, 32);
    lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, 48);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_password_mode(ta, true);
    lv_textarea_set_placeholder_text(ta, "请输入 WiFi 密码");
    s_wifiPwdTa = ta;

    /* 屏幕键盘（填充剩余空间） */
    kb = lv_keyboard_create(dlg);
    lv_obj_set_size(kb, LCD_WIDTH, LCD_HEIGHT - 85);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, ta);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_add_event_cb(kb, wifi_pwd_kb_ready_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, wifi_pwd_close_cb, LV_EVENT_CANCEL, NULL);
}

/** 定时器回调：延迟关闭配网对话框 */
static void wifi_dlg_close_timer_cb(lv_timer_t *timer)
{
    lv_timer_del(timer);
    if (s_wifiDlg != NULL)
    {
        lv_obj_del(s_wifiDlg);
        s_wifiDlg = NULL;
    }
}

/** 执行连接：保存凭据到 NVS 并发起连接 */
static void wifi_connect_do(const char *password)
{
    const ui_cfg_t *cfg = ui_cfg();
    node_role_cfg_t role_cfg = {0};
    esp_err_t err;

    if (cfg->wifi == NULL || s_selectedSsid[0] == '\0')
    {
        UI_LOGE( "WiFi 连接：参数无效");
        return;
    }

    UI_LOGI( "WiFi 连接：SSID=\"%s\"", s_selectedSsid);

    /* 1) 拷贝密码到局部缓冲区（必须在删对话框前，因为
     *    lv_textarea_get_text() 返回的内部指针在 textarea 释放后悬空） */
    char pwd_buf[65];
    snprintf(pwd_buf, sizeof(pwd_buf), "%.*s",
             (int)sizeof(pwd_buf) - 1, password ? password : "");

    /* 2) 关闭密码对话框（如果打开） */
    if (s_wifiPwdDlg != NULL)
    {
        lv_obj_del(s_wifiPwdDlg);
        s_wifiPwdDlg = NULL;
        s_wifiPwdTa = NULL;
    }

    /* 3) 显示连接中状态 */
    if (s_wifiDlg != NULL && s_wifiStatusLbl != NULL)
    {
        lv_label_set_text(s_wifiStatusLbl, "正在连接...");
    }

    /* 4) 读取当前配置、更新 WiFi 凭据、写回 NVS */
    err = node_role_get(&role_cfg);
    if (err != ESP_OK)
    {
        UI_LOGE( "node_role_get failed: %s", esp_err_to_name(err));
        if (s_wifiStatusLbl != NULL)
        {
            lv_label_set_text(s_wifiStatusLbl, "配置读取失败");
        }
        return;
    }

    snprintf(role_cfg.wifi_ssid, sizeof(role_cfg.wifi_ssid), "%.*s",
             (int)sizeof(role_cfg.wifi_ssid) - 1, s_selectedSsid);
    snprintf(role_cfg.wifi_password, sizeof(role_cfg.wifi_password), "%.*s",
             (int)sizeof(role_cfg.wifi_password) - 1, pwd_buf);

    err = node_role_set(&role_cfg);
    if (err != ESP_OK)
    {
        UI_LOGE( "node_role_set failed: %s", esp_err_to_name(err));
        if (s_wifiStatusLbl != NULL)
        {
            lv_label_set_text(s_wifiStatusLbl, "保存配置失败");
        }
        return;
    }

    /* 5) 下发新凭据并立即连接 */
    err = wifi_manager_set_sta(cfg->wifi, s_selectedSsid, pwd_buf);
    if (err != ESP_OK)
    {
        UI_LOGE( "wifi_manager_set_sta failed: %s", esp_err_to_name(err));
        if (s_wifiStatusLbl != NULL)
        {
            lv_label_set_text(s_wifiStatusLbl, "发起连接失败");
        }
        return;
    }

    /* 6) 延迟关闭对话框（让用户看到"正在连接"） */
    lv_timer_create(wifi_dlg_close_timer_cb, 2000, NULL);
}

/* ======================== OTA 固件升级 ======================== */

/**
 * @brief OTA 事件回调（注册到 OTA_Update 模块，由 ota_sync_task 上下文调用）
 *
 * 职责：
 *   - UPDATE_AVAILABLE：同步停止 music/call/light 任务，释放内存和 CPU
 *   - 所有事件：转发到 UI_Task 事件队列，由 scr_settings_apply_ota_event 刷新界面
 *
 * 注意：本回调在 ota_sync_task（优先级 5）上下文中执行，非 UI 线程。
 *       停止任务的操作必须在 ota_check_and_upgrade() 继续下载之前完成，
 *       因此此处直接调用 destroy 而非入队。
 */
static void ota_ui_evt_cb(const ota_update_event_info_t *info, void *user_ctx)
{
    (void)user_ctx;

    if (info == NULL)
    {
        return;
    }

    if (info->event == OTA_UPDATE_EVT_UPDATE_AVAILABLE)
    {
        const ui_cfg_t *cfg = ui_cfg();
        UI_LOGI( "OTA update available, stopping app tasks...");

        if (cfg->music != NULL)
        {
            music_play_destroy(cfg->music);
            UI_LOGI( "music_play destroyed");
        }
        if (cfg->call_phone != NULL)
        {
            call_phone_destroy(cfg->call_phone);
            UI_LOGI( "call_phone destroyed");
        }
        if (cfg->light != NULL)
        {
            light_control_destroy(cfg->light);
            UI_LOGI( "light_control destroyed");
        }
    }

    ui_on_ota_event(info);
}

/**
 * @brief OTA 后台任务：调用阻塞式 OTA_Update_CheckAndUpgrade()
 *
 * 本任务在独立栈上运行，阻塞等待 OTA 检测/下载/校验完成。
 * 有新固件时模块内部会重启设备；无更新或失败时本任务退出。
 *
 * 栈 4096：实际只需保存少量局部变量 + 等待信号量（ota_sync_task 内部自建
 * 12288 字节栈执行 HTTP/下载的重活）。
 */
static void ota_bg_task(void *arg)
{
    (void)arg;
    esp_err_t err;

    UI_LOGI( "OTA background task started");

    OTA_Update_RegisterEventCallback(ota_ui_evt_cb, NULL);

    err = OTA_Update_CheckAndUpgrade();
    if (err == ESP_OK)
    {
        UI_LOGI( "OTA check: firmware up to date");
    }
    else
    {
        UI_LOGW( "OTA check/upgrade failed: %s", esp_err_to_name(err));
    }

    UI_LOGI( "OTA background task exiting");
    vTaskDelete(NULL);
}

static void ota_click_cb(lv_event_t *e)
{
    (void)e;
    const ui_cfg_t *cfg = ui_cfg();

    if (cfg->wifi == NULL || !wifi_manager_is_connected(cfg->wifi))
    {
        UI_LOGW( "OTA check skipped: WiFi not connected");
        if (s_lblFwVersion != NULL)
        {
            lv_label_set_text(s_lblFwVersion, "版本: 请先连接WiFi");
        }
        return;
    }

    if (s_btnOta != NULL)
    {
        lv_obj_add_state(s_btnOta, LV_STATE_DISABLED);
    }

    if (s_lblFwVersion != NULL)
    {
        char buf[48];
        const char *localVer = OTA_Update_GetLocalVersion();
        if (localVer != NULL && localVer[0] != '\0')
        {
            snprintf(buf, sizeof(buf), "版本: %s (检查中...)", localVer);
        }
        else
        {
            snprintf(buf, sizeof(buf), "版本: 检查中...");
        }
        lv_label_set_text(s_lblFwVersion, buf);
    }

    BaseType_t ret = xTaskCreatePinnedToCore(ota_bg_task, "ota_ui", 4096,
                                 NULL, 5, NULL, 0);
    if (ret != pdPASS)
    {
        UI_LOGE( "Failed to create OTA background task");
        if (s_lblFwVersion != NULL)
        {
            lv_label_set_text(s_lblFwVersion, "版本: 启动OTA失败(内存不足)");
        }
        if (s_btnOta != NULL)
        {
            lv_obj_clear_state(s_btnOta, LV_STATE_DISABLED);
        }
        return;
    }

    UI_LOGI( "OTA check started from UI");
}

void scr_settings_apply_ota_event(const ota_update_event_info_t *info)
{
    if (info == NULL || s_lblFwVersion == NULL)
    {
        return;
    }

    char buf[64];
    const char *localVer = OTA_Update_GetLocalVersion();
    const char *remoteVer = info->version;

    switch (info->event)
    {
    case OTA_UPDATE_EVT_CHECK_STARTED:
        snprintf(buf, sizeof(buf), "版本: %s (检查中...)", localVer);
        break;

    case OTA_UPDATE_EVT_UP_TO_DATE:
        snprintf(buf, sizeof(buf), "版本: %s (已是最新)", localVer);
        if (s_btnOta != NULL)
        {
            lv_obj_clear_state(s_btnOta, LV_STATE_DISABLED);
        }
        break;

    case OTA_UPDATE_EVT_UPDATE_AVAILABLE:
        snprintf(buf, sizeof(buf), "版本: %s -> %s (正在准备)",
                 localVer, remoteVer);
        break;

    case OTA_UPDATE_EVT_DOWNLOAD_STARTED:
        snprintf(buf, sizeof(buf), "版本: %s -> %s (下载 0%%)",
                 localVer, remoteVer);
        break;

    case OTA_UPDATE_EVT_DOWNLOAD_PROGRESS:
        snprintf(buf, sizeof(buf), "版本: %s -> %s (下载 %d%%)",
                 localVer, remoteVer, info->progress_percent);
        break;

    case OTA_UPDATE_EVT_DOWNLOAD_FINISHED:
        snprintf(buf, sizeof(buf), "版本: %s -> %s (校验中...)",
                 localVer, remoteVer);
        break;

    case OTA_UPDATE_EVT_READY_TO_REBOOT:
        snprintf(buf, sizeof(buf), "版本: %s -> %s (即将重启)",
                 localVer, remoteVer);
        break;

    case OTA_UPDATE_EVT_ABORTED:
        if (info->http_status > 0)
        {
            snprintf(buf, sizeof(buf), "版本: %s (失败 HTTP %d)",
                     localVer, info->http_status);
        }
        else
        {
            snprintf(buf, sizeof(buf), "版本: %s (检查失败)", localVer);
        }
        if (s_btnOta != NULL)
        {
            lv_obj_clear_state(s_btnOta, LV_STATE_DISABLED);
        }
        break;

    default:
        return;
    }

    lv_label_set_text(s_lblFwVersion, buf);
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
    lv_obj_set_scrollable(page, true);
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
    s_lblFwVersion = ui_label_create(panel, "版本: 待 OTA 接入（第十二阶段）",
                          &ui_font_cn_16, UI_COLOR_TEXT_SUB);
    lv_obj_set_pos(s_lblFwVersion, 8, 12);
    s_btnOta = text_btn(panel, 204, 7, 84, 30, "检查更新", ota_click_cb);
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