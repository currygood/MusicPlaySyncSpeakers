/**
 * @file scr_light.c
 * @brief 灯控页（Tab 2；UI 界面设计 5.4）
 *
 * 两张卡片：卧室灯(bedroom) / 客厅灯(livingroom)，灯名遵循《灯控协议规范》。
 * - 句柄注入（ui_cfg.light）：switch → light_control_send() 真实下发，
 *   状态经 ui_on_light_event()（ack/30s status/超时）回流刷新；
 * - 未注入（第八阶段默认）：本地演示态 + ESP_LOGI 占位，注明真实接口。
 * 图标：随固件烧录在 flash storage 分区 ui_img 目录（Light_On.png / Light_Off.png，
 * 路径宏见 ui_priv.h），缺失时回退字形灯泡（颜色区分开/关）。
 */

#include "ui_priv.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "UI";

typedef struct {
    const char *name_cn;     /* 卡片名 */
    const char *name_en;     /* 灯控协议灯名 */
    const char *png_on;      /* SD 卡开灯图 */
    const char *png_off;     /* SD 卡关灯图 */
    bool        png_ok;      /* 两张图都存在 */
    bool        on;          /* 当前本地态 */
    lv_obj_t   *img;         /* PNG 图标（png_ok 时有效） */
    lv_obj_t   *icon_lbl;    /* 字形灯泡（回退） */
    lv_obj_t   *name_lbl;
    lv_obj_t   *state_lbl;
    lv_obj_t   *sw;
} light_card_t;

static light_card_t s_cards[2] = {
    { "卧室灯", "bedroom",    UI_IMG_LIGHT_ON, UI_IMG_LIGHT_OFF,
      false, false, NULL, NULL, NULL, NULL, NULL },
    { "客厅灯", "livingroom", UI_IMG_LIGHT_ON, UI_IMG_LIGHT_OFF,
      false, false, NULL, NULL, NULL, NULL, NULL },
};

/* ======================== 卡片状态渲染 ======================== */

static void light_card_apply(light_card_t *card, light_state_t st)
{
    const char *txt;
    lv_color_t color;
    bool isOn = (st == LIGHT_STATE_ON);

    switch (st)
    {
    case LIGHT_STATE_ON:  txt = "已开启"; color = UI_COLOR_ACCENT;    break;
    case LIGHT_STATE_OFF: txt = "已关闭"; color = UI_COLOR_TEXT_SUB;  break;
    default:              txt = "未知";   color = UI_COLOR_DISABLED;  break;
    }
    card->on = isOn;

    lv_label_set_text(card->state_lbl, txt);
    lv_obj_set_style_text_color(card->state_lbl, color, 0);

    /* 图标：PNG 换图；字形换色 */
    if (card->png_ok && card->img != NULL)
    {
        lv_image_set_src(card->img, isOn ? card->png_on : card->png_off);
    }
    if (card->icon_lbl != NULL)
    {
        lv_obj_set_style_text_color(card->icon_lbl,
                                    isOn ? UI_COLOR_ACCENT : UI_COLOR_DISABLED, 0);
    }

    /* 开关跟随状态（UNKNOWN 保持原位不动） */
    if (st == LIGHT_STATE_ON)
    {
        lv_obj_add_state(card->sw, LV_STATE_CHECKED);
    }
    else if (st == LIGHT_STATE_OFF)
    {
        lv_obj_clear_state(card->sw, LV_STATE_CHECKED);
    }
}

/* ======================== 事件 ======================== */

static void sw_click_cb(lv_event_t *e)
{
    light_card_t *card = (light_card_t *)lv_event_get_user_data(e);
    const ui_cfg_t *cfg = ui_cfg();
    bool wantOn = lv_obj_has_state(card->sw, LV_STATE_CHECKED);

    if (cfg->light != NULL)
    {
        /* 真实下发：状态等 ack/status 回流（scr_light_apply_event），不本地提交 */
        esp_err_t ret = light_control_send(cfg->light,
                                           wantOn ? LIGHT_CMD_ON : LIGHT_CMD_OFF,
                                           card->name_en);
        lv_label_set_text(card->state_lbl, "等待回执…");
        lv_obj_set_style_text_color(card->state_lbl, UI_COLOR_ACCENT, 0);
        ESP_LOGI(TAG, "light %s(%s) -> light_control_send(%s): %s",
                 card->name_cn, card->name_en,
                 wantOn ? "LIGHT_CMD_ON" : "LIGHT_CMD_OFF",
                 esp_err_to_name(ret));
    }
    else
    {
        /* 第八阶段占位：本地演示态 + 日志（LightControl 第七阶段已实现，
         * app_main 传入句柄后本分支自动不再进入） */
        ESP_LOGI(TAG, "[占位] 灯控 %s %s -> TODO: light_control_send(LIGHT_CMD_%s, \"%s\")",
                 card->name_cn, wantOn ? "ON" : "OFF",
                 wantOn ? "ON" : "OFF", card->name_en);
        light_card_apply(card, wantOn ? LIGHT_STATE_ON : LIGHT_STATE_OFF);
    }
}

void scr_light_apply_event(const light_event_t *evt)
{
    int i;

    for (i = 0; i < 2; i++)
    {
        if (strcmp(s_cards[i].name_en, evt->data.light) == 0)
        {
            const char *id = (evt->id == LIGHT_EVT_ACK)    ? "ACK"    :
                             (evt->id == LIGHT_EVT_STATUS) ? "STATUS" : "ERROR";
            ESP_LOGI(TAG, "light evt %s: %s ok=%d", id, evt->data.light, evt->data.ok);
            light_card_apply(&s_cards[i], evt->data.state);
            if (evt->id == LIGHT_EVT_ERROR)
            {
                ESP_LOGW(TAG, "light %s: 回执异常/状态上报超时 -> 状态回落未知",
                         evt->data.light);
            }
            return;
        }
    }
    ESP_LOGW(TAG, "light evt: unknown light \"%s\"", evt->data.light);
}

/* ======================== 页面构建 ======================== */

static void card_build(light_card_t *card, lv_obj_t *page, int x)
{
    lv_obj_t *panel = lv_obj_create(page);
    ui_style_card(panel);               /* 先剥离主题并设卡片样式（内含 remove_style_all） */
    lv_obj_set_pos(panel, x, 4);        /* pos/size 属样式属性，须在其后设置 */
    lv_obj_set_size(panel, 148, 130);

    /* 图标：优先 flash 内 PNG（48×48 区），缺失回退 24px 字形灯泡 */
    if (ui_img_file_ok(card->png_on) && ui_img_file_ok(card->png_off))
    {
        card->png_ok = true;
        card->img = lv_image_create(panel);
        lv_obj_set_pos(card->img, 50, 8);
        lv_obj_set_size(card->img, 48, 48);
        lv_image_set_inner_align(card->img, LV_IMAGE_ALIGN_CONTAIN);
    }
    else
    {
        card->icon_lbl = ui_label_create(panel, UI_ICON_BULB, &ui_icons_24,
                                         UI_COLOR_DISABLED);
        lv_obj_align(card->icon_lbl, LV_ALIGN_TOP_MID, 0, 20);
    }

    card->name_lbl = ui_label_create(panel, card->name_cn, &ui_font_cn_16, UI_COLOR_TEXT);
    lv_obj_align(card->name_lbl, LV_ALIGN_TOP_MID, 0, 58);

    card->state_lbl = ui_label_create(panel, "未知", &ui_font_cn_16, UI_COLOR_DISABLED);
    lv_obj_align(card->state_lbl, LV_ALIGN_TOP_MID, 0, 78);

    card->sw = lv_switch_create(panel);
    lv_obj_align(card->sw, LV_ALIGN_TOP_MID, 0, 98);
    lv_obj_set_style_bg_color(card->sw, UI_COLOR_PANEL_HI, LV_PART_MAIN);
    lv_obj_set_style_bg_color(card->sw, UI_COLOR_PRIMARY, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(card->sw, sw_click_cb, LV_EVENT_VALUE_CHANGED, card);
}

void scr_light_build(lv_obj_t *page)
{
    const ui_cfg_t *cfg = ui_cfg();
    lv_obj_t *note;
    int i;

    card_build(&s_cards[0], page, 8);
    card_build(&s_cards[1], page, 164);

    note = ui_label_create(page,
                           (cfg->light != NULL) ? "状态以灯控回执为准" : "灯控未接线 · 本地演示模式",
                           &ui_font_cn_16, UI_COLOR_DISABLED);
    lv_obj_align(note, LV_ALIGN_BOTTOM_MID, 0, -4);

    /* 初始状态：有句柄用模块缓存恢复，否则未知 */
    for (i = 0; i < 2; i++)
    {
        light_state_t st = LIGHT_STATE_UNKNOWN;
        if (cfg->light != NULL &&
            light_control_get_state(cfg->light, s_cards[i].name_en, &st) == ESP_OK)
        {
            light_card_apply(&s_cards[i], st);
        }
        else
        {
            light_card_apply(&s_cards[i], LIGHT_STATE_UNKNOWN);
        }
    }
}
