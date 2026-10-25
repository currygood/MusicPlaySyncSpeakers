/**
 * @file ui_statusbar.c
 * @brief 状态栏（常驻 24px）：WiFi / 蓝牙 / 同步 三图标（UI 界面设计 5.1）
 *
 * 多 screen 架构：每个页面的 screen 各挂一份状态栏，本模块用实例表统一管理；
 * 单个 1s lv_timer 轮询刷新全部实例（未激活 screen 上的刷新无绘制开销）。
 *
 * 数据源约定：
 *   - WiFi：轮询 wifi_manager_get_state()（句柄经 ui_cfg 注入后生效，
 *     不注册 wifi_manager 事件——架构文档 3.4.2 约定 UI 走查询）；
 *   - 蓝牙：由播放源推断——music_play_get_source()==MUSIC_SOURCE_BT 点亮，未注入句柄时灰显；
 *   - 同步：sync_protocol_master_get_status()（第九阶段实现），先占位灰显。
 *
 * 原第 4 个"麦克风"图标随语音唤醒功能下线一并删除（2026-10-25，
 * 见 legacy_disabled/README.md）：已无 call_phone 句柄可查。
 */

#include "ui_priv.h"
#include "esp_log.h"

typedef struct {
    lv_obj_t *lblWifi;
    lv_obj_t *lblBt;
    lv_obj_t *lblSync;
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
        /* WiFi：IDLE/ERROR 灰+OFF图标，连接中/断开重连 琥珀+OFF图标，已连接 绿+WIFI图标 */
        if (cfg->wifi != NULL)
        {
            wifi_state_t st = WIFI_STATE_IDLE;
            if (wifi_manager_get_state(cfg->wifi, &st) == ESP_OK)
            {
                lv_color_t color = UI_COLOR_DISABLED;
                const char *icon = UI_ICON_WIFI_OFF;
                if (st == WIFI_STATE_CONNECTED)
                {
                    color = UI_COLOR_OK;
                    icon = UI_ICON_WIFI;
                }
                else if (st == WIFI_STATE_CONNECTING ||
                         st == WIFI_STATE_DISCONNECTED)
                {
                    color = UI_COLOR_ACCENT;
                }
                lv_obj_set_style_text_color(s_insts[i].lblWifi, color, 0);
                lv_label_set_text(s_insts[i].lblWifi, icon);
            }
        }

        /* 蓝牙：MusicPlay 当前源为 BT（A2DP 已连接）时点亮 */
        if (cfg->music != NULL)
        {
            lv_color_t color = (music_play_get_source(cfg->music) == MUSIC_SOURCE_BT)
                                   ? UI_COLOR_OK : UI_COLOR_DISABLED;
            lv_obj_set_style_text_color(s_insts[i].lblBt, color, 0);
        }

        /* 同步：占位灰显（sync_protocol 状态查询接口落地后点亮） */
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

    /* 左：WiFi　中：蓝牙　右：同步（16px 图标） */
    inst->lblWifi = ui_label_create(bar, UI_ICON_WIFI_OFF, &ui_icons_16, UI_COLOR_DISABLED);
    lv_obj_align(inst->lblWifi, LV_ALIGN_LEFT_MID, 8, 0);

    inst->lblBt = ui_label_create(bar, UI_ICON_BT, &ui_icons_16, UI_COLOR_DISABLED);
    lv_obj_align(inst->lblBt, LV_ALIGN_CENTER, 0, 0);

    inst->lblSync = ui_label_create(bar, UI_ICON_SYNC, &ui_icons_16, UI_COLOR_DISABLED);
    lv_obj_align(inst->lblSync, LV_ALIGN_RIGHT_MID, -8, 0);

    /* 单定时器服务所有实例：首个实例注册时启动 */
    if (s_instCnt == 0)
    {
        lv_timer_create(statusbar_timer_cb, 1000, NULL);
    }
    s_instCnt++;
}