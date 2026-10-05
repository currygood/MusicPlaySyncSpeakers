/**
 * @file ui_statusbar.c
 * @brief 状态栏（常驻 24px）：WiFi / 蓝牙 / 同步 / 麦克风 四图标（UI 界面设计 5.1）
 *
 * 多 screen 架构：每个页面的 screen 各挂一份状态栏，本模块用实例表统一管理；
 * 单个 1s lv_timer 轮询刷新全部实例（未激活 screen 上的刷新无绘制开销）。
 *
 * 数据源约定：
 *   - WiFi：轮询 wifi_manager_get_state()（句柄经 ui_cfg 注入后生效，
 *     不注册 wifi_manager 事件——架构文档 3.4.2 约定 UI 走查询）；
 *   - 蓝牙：由播放源推断——MusicPlay/PlayMode 未实现（第九~十一阶段），先占位灰显；
 *   - 同步：sync_protocol_master_get_status()（第九阶段实现），先占位灰显；
 *   - 麦克风：轮询 call_phone_get_state()（CallPhone 不发事件，只能查询）。
 */

#include "ui_priv.h"
#include "esp_log.h"

typedef struct {
    lv_obj_t *lblWifi;
    lv_obj_t *lblBt;
    lv_obj_t *lblSync;
    lv_obj_t *lblMic;
} sb_inst_t;

static sb_inst_t s_insts[UI_PAGE_COUNT];
static int       s_instCnt = 0;

/** 1s 慢轮询：刷新全部实例上有句柄的图标（lv_timer 运行在 UI_Task 上下文，合法） */
static void statusbar_timer_cb(lv_timer_t *timer)
{
    const ui_cfg_t *cfg = ui_cfg();
    int i;
    (void)timer;

    for (i = 0; i < s_instCnt; i++)
    {
        /* WiFi：IDLE/ERROR 灰，连接中/断开重连 琥珀，已连接 绿 */
        if (cfg->wifi != NULL)
        {
            wifi_state_t st = WIFI_STATE_IDLE;
            if (wifi_manager_get_state(cfg->wifi, &st) == ESP_OK)
            {
                lv_color_t color = UI_COLOR_DISABLED;
                if (st == WIFI_STATE_CONNECTED)
                {
                    color = UI_COLOR_OK;
                }
                else if (st == WIFI_STATE_CONNECTING ||
                         st == WIFI_STATE_DISCONNECTED)
                {
                    color = UI_COLOR_ACCENT;
                }
                lv_obj_set_style_text_color(s_insts[i].lblWifi, color, 0);
            }
        }

        /* 麦克风：IDLE 灰，LISTENING/CONNECTING 琥珀，STREAMING 绿 */
        if (cfg->call_phone != NULL)
        {
            call_phone_state_t st = CALL_PHONE_STATE_IDLE;
            if (call_phone_get_state(cfg->call_phone, &st) == ESP_OK)
            {
                lv_color_t color = UI_COLOR_DISABLED;
                if (st == CALL_PHONE_STATE_STREAMING)
                {
                    color = UI_COLOR_OK;
                }
                else if (st == CALL_PHONE_STATE_LISTENING ||
                         st == CALL_PHONE_STATE_CONNECTING)
                {
                    color = UI_COLOR_ACCENT;
                }
                lv_obj_set_style_text_color(s_insts[i].lblMic, color, 0);
            }
        }

        /* 蓝牙/同步：占位灰显（第九~十一阶段接入 MusicPlay/sync_protocol 后点亮） */
    }
}

void ui_statusbar_create(lv_obj_t *parent)
{
    sb_inst_t *inst;
    lv_obj_t *bar;

    if (s_instCnt >= UI_PAGE_COUNT)
    {
        return;
    }
    inst = &s_insts[s_instCnt];

    bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, LCD_WIDTH, UI_STATUSBAR_H);
    lv_obj_set_style_bg_color(bar, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* 左：WiFi　中：蓝牙 / 同步　右：麦克风（16px 图标） */
    inst->lblWifi = ui_label_create(bar, UI_ICON_WIFI_OFF, &ui_icons_16, UI_COLOR_DISABLED);
    lv_obj_align(inst->lblWifi, LV_ALIGN_LEFT_MID, 8, 0);

    inst->lblBt = ui_label_create(bar, UI_ICON_BT, &ui_icons_16, UI_COLOR_DISABLED);
    lv_obj_align(inst->lblBt, LV_ALIGN_CENTER, -18, 0);

    inst->lblSync = ui_label_create(bar, UI_ICON_SYNC, &ui_icons_16, UI_COLOR_DISABLED);
    lv_obj_align(inst->lblSync, LV_ALIGN_CENTER, 18, 0);

    inst->lblMic = ui_label_create(bar, UI_ICON_MIC, &ui_icons_16, UI_COLOR_DISABLED);
    lv_obj_align(inst->lblMic, LV_ALIGN_RIGHT_MID, -8, 0);

    /* 单定时器服务所有实例：首个实例注册时启动 */
    if (s_instCnt == 0)
    {
        lv_timer_create(statusbar_timer_cb, 1000, NULL);
    }
    s_instCnt++;
}
