/**
 * @file ui_tabbar.c
 * @brief 底部 Tab 栏（常驻 40px，纯图标）：播放 / 灯控 / 设置（UI 界面设计 4.2）
 *
 * 多 screen 架构：每个页面的 screen 各挂一份 Tab 栏，本模块用实例表统一管理；
 * ui_tabbar_set_active() 同步全部实例的选中高亮，二级页（本地音乐列表）
 * 打开期间"播放"Tab 保持高亮。切页由 ui_nav_goto() -> lv_screen_load() 完成。
 */

#include "ui_priv.h"
#include "esp_log.h"
#include <stdint.h>

#define TAB_COUNT  3
#define TAB_W      (LCD_WIDTH / TAB_COUNT)

static const char *s_icons[TAB_COUNT] = {
    UI_ICON_MUSIC, UI_ICON_BULB, UI_ICON_SETTINGS,
};
static const ui_page_t s_pageMap[TAB_COUNT] = {
    UI_PAGE_PLAYER, UI_PAGE_LIGHT, UI_PAGE_SETTINGS,
};

typedef struct {
    lv_obj_t *btn[TAB_COUNT];
    lv_obj_t *lbl[TAB_COUNT];
} tab_inst_t;

static tab_inst_t s_insts[UI_PAGE_COUNT];
static int        s_instCnt = 0;

static void tab_click_cb(lv_event_t *e)
{
    int tab = (int)(intptr_t)lv_event_get_user_data(e);
    ui_nav_goto(s_pageMap[tab]);
}

void ui_tabbar_set_active(ui_page_t page)
{
    int i, j;

    for (i = 0; i < s_instCnt; i++)
    {
        for (j = 0; j < TAB_COUNT; j++)
        {
            bool active = (s_pageMap[j] == page);
            if (page == UI_PAGE_PLAYLIST && s_pageMap[j] == UI_PAGE_PLAYER)
            {
                active = true;   /* 二级页归属播放页 */
            }
            lv_obj_set_style_text_color(s_insts[i].lbl[j],
                                        active ? UI_COLOR_PRIMARY : UI_COLOR_DISABLED, 0);
            lv_obj_set_style_bg_color(s_insts[i].btn[j],
                                      active ? UI_COLOR_PANEL : UI_COLOR_BG, 0);
        }
    }
}

void ui_tabbar_create(lv_obj_t *parent, ui_page_t cur)
{
    tab_inst_t *inst;
    lv_obj_t *bar;
    int j;

    if (s_instCnt >= UI_PAGE_COUNT)
    {
        return;
    }
    inst = &s_insts[s_instCnt];

    bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_pos(bar, 0, UI_STATUSBAR_H + UI_CONTENT_H);
    lv_obj_set_size(bar, LCD_WIDTH, UI_TABBAR_H);
    lv_obj_set_style_bg_color(bar, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(bar, UI_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    for (j = 0; j < TAB_COUNT; j++)
    {
        inst->btn[j] = lv_button_create(bar);
        lv_obj_remove_style_all(inst->btn[j]);
        lv_obj_set_pos(inst->btn[j], j * TAB_W, 0);
        lv_obj_set_size(inst->btn[j], TAB_W, UI_TABBAR_H);
        lv_obj_add_event_cb(inst->btn[j], tab_click_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)j);

        inst->lbl[j] = lv_label_create(inst->btn[j]);
        lv_label_set_text(inst->lbl[j], s_icons[j]);
        lv_obj_set_style_text_font(inst->lbl[j], &ui_icons_24, 0);
        lv_obj_set_style_text_color(inst->lbl[j], UI_COLOR_DISABLED, 0);
        lv_obj_center(inst->lbl[j]);
    }
    s_instCnt++;

    ui_tabbar_set_active(cur);
}
