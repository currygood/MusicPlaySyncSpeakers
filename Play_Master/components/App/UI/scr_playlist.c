/**
 * @file scr_playlist.c
 * @brief 本地音乐列表（二级页，覆盖内容区；UI 界面设计 5.3）
 *
 * 第八阶段：MusicPlay 未实现（第十阶段接入），列表为空态占位；返回键可用。
 * 接入后：进入页面时 music_play_scan_local() → get_track_count/get_track 渲染，
 * 点击项 music_play_select_track(i) 并返回播放页。
 */

#include "ui_priv.h"
#include "sd_card.h"
#include "esp_log.h"

static const char *TAG = "UI";

/** 返回键：回到播放页 */
static void back_click_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "playlist: back -> player");
    ui_nav_goto(UI_PAGE_PLAYER);
}

void scr_playlist_build(lv_obj_t *page)
{
    lv_obj_t *back;
    lv_obj_t *title;
    lv_obj_t *icon;
    lv_obj_t *line1;
    lv_obj_t *line2;
    lv_obj_t *note;

    /* 头部：返回键 + 标题 */
    back = lv_button_create(page);
    lv_obj_remove_style_all(back);
    lv_obj_set_pos(back, 4, 4);
    lv_obj_set_size(back, 44, 36);
    lv_obj_set_style_bg_color(back, UI_COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(back, 8, 0);
    lv_obj_set_style_border_color(back, UI_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_add_event_cb(back, back_click_cb, LV_EVENT_CLICKED, NULL);

    title = ui_label_create(back, UI_ICON_BACK, &ui_icons_24, UI_COLOR_TEXT);
    lv_obj_center(title);

    title = ui_label_create(page, "本地音乐", &ui_font_cn_16, UI_COLOR_TEXT);
    lv_obj_set_pos(title, 56, 12);

    /* 空态：居中图标 + 两行说明（SD 未挂载时提示挂载问题） */
    icon = ui_label_create(page, UI_ICON_MUSIC, &ui_icons_24, UI_COLOR_DISABLED);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -46);

    if (SD_Card_Is_Mounted())
    {
        line1 = ui_label_create(page, "暂无音乐文件", &ui_font_cn_16, UI_COLOR_TEXT_SUB);
        line2 = ui_label_create(page, "请将 MP3/WAV 放入 /music 目录",
                                &ui_font_cn_16, UI_COLOR_DISABLED);
    }
    else
    {
        line1 = ui_label_create(page, "SD 卡未挂载", &ui_font_cn_16, UI_COLOR_TEXT_SUB);
        line2 = ui_label_create(page, "请检查 SD 卡后重启", &ui_font_cn_16, UI_COLOR_DISABLED);
    }
    lv_obj_align(line1, LV_ALIGN_CENTER, 0, -12);
    lv_obj_align(line2, LV_ALIGN_CENTER, 0, 10);

    note = ui_label_create(page, "MusicPlay 接入后自动扫描（第十阶段）",
                           &ui_font_cn_16, UI_COLOR_DISABLED);
    lv_obj_align(note, LV_ALIGN_CENTER, 0, 44);
}
