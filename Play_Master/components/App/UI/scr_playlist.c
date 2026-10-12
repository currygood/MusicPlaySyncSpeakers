/**
 * @file scr_playlist.c
 * @brief 本地音乐列表（二级页，覆盖内容区；UI 界面设计 5.3）
 *
 * 数据接线（第十一阶段，MusicPlay 已实现）：
 *   - 进入页面：播放页列表入口调 music_play_scan_local()（异步），本页先按
 *     已知状态渲染一次；
 *   - 渲染：music_play_get_track_count() / music_play_get_track(i) 逐行建行，
 *     当前曲（view->track_index）高亮 + ▶ 前缀，文件序 = 播放序；
 *   - 点击行：music_play_select_track(i) → 自动返回播放页；
 *   - 重建：MUSIC_EVT_PLAYLIST_CHANGED / TRACK_CHANGED / SOURCE_CHANGED 到达
 *     时由 ui.c 转发 scr_playlist_apply_event() 全量重建；
 *   - 空态：SD 未挂载 → "SD 卡未挂载"；已挂载无文件 → "暂无音乐文件"；
 *   - 未注入 MusicPlay 句柄时提示未接线。
 * 所有 lv_* 调用只在 UI_Task（设计文档 6.8）。
 */

#include "ui_priv.h"
#include "sd_card.h"
#include "esp_log.h"

#include <stdint.h>
#include <stdio.h>

static const char *TAG = "UI";

#define PLAYLIST_HEADER_H  44                    /* 头部高度（返回键 + 标题） */
#define PLAYLIST_ROW_H     30                    /* 单行高（含 2px 行距） */
#define PLAYLIST_PAD       2                     /* 列表容器内边距 */

static lv_obj_t *s_lblCount;   /* 头部计数 "本地音乐 (N)" */
static lv_obj_t *s_body;       /* 滚动列表容器（flex 纵向） */

/* ======================== 行事件 ======================== */

/** 返回键：回到播放页 */
static void back_click_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "playlist: back -> player");
    ui_nav_goto(UI_PAGE_PLAYER);
}

/** 点击某行：选曲并返回播放页（user_data = 曲目下标） */
static void row_click_cb(lv_event_t *e)
{
    const ui_cfg_t *cfg = ui_cfg();
    uint16_t index = (uint16_t)(uintptr_t)lv_event_get_user_data(e);

    if (cfg->music == NULL)
    {
        return;
    }
    ESP_LOGI(TAG, "playlist: select track %u", (unsigned)index);
    music_play_select_track(cfg->music, index);
    ui_nav_goto(UI_PAGE_PLAYER);
}

/* ======================== 列表构建 ======================== */

/** 行宽 / 行内文字左边距（留出当前曲 ▶ 图标位，各行标题等宽左对齐） */
#define PLAYLIST_ROW_W      (LCD_WIDTH - 8 - PLAYLIST_PAD * 2)
#define PLAYLIST_TEXT_X     26

/**
 * 建一行曲目（当前曲高亮 + ▶ 标记）。
 * 注意：图标字形在 ui_icons_16/24 字体里（PUA 码位），中文文案在 ui_font_cn_16，
 * 两者不能混在同一个 label 上——所以 ▶ 单独建一个 label。
 */
static void playlist_row_add(const music_track_t *t, uint16_t index, bool cur)
{
    lv_obj_t *row = lv_button_create(s_body);
    lv_obj_t *lbl;

    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, PLAYLIST_ROW_W);
    lv_obj_set_height(row, PLAYLIST_ROW_H - 2);
    lv_obj_set_style_bg_color(row, cur ? UI_COLOR_PRIMARY : UI_COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_set_style_bg_color(row, UI_COLOR_PANEL_HI, LV_STATE_PRESSED);
    lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)index);

    if (cur)
    {
        lbl = ui_label_create(row, UI_ICON_PLAY, &ui_icons_16, UI_COLOR_TEXT);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 8, 0);
    }

    lbl = ui_label_create(row, t->display_name, &ui_font_cn_16,
                          cur ? UI_COLOR_TEXT : UI_COLOR_TEXT_SUB);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_width(lbl, PLAYLIST_ROW_W - PLAYLIST_TEXT_X - 6);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, PLAYLIST_TEXT_X, 0);
}

/** 空态/提示行：占满容器宽度的居中两行文案 */
static void playlist_empty_add(const char *line1, const char *line2)
{
    lv_obj_t *box = lv_obj_create(s_body);
    lv_obj_t *lbl;

    lv_obj_remove_style_all(box);
    lv_obj_set_width(box, LCD_WIDTH - 8 - PLAYLIST_PAD * 2);
    lv_obj_set_height(box, 110);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lbl = ui_label_create(box, line1, &ui_font_cn_16, UI_COLOR_TEXT_SUB);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -14);

    if (line2 != NULL)
    {
        lbl = ui_label_create(box, line2, &ui_font_cn_16, UI_COLOR_DISABLED);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 10);
    }
}

/**
 * 按 MusicPlay 当前状态重建列表。
 * 调用上下文：UI_Task（进入页面 / 事件回流）。
 */
void scr_playlist_refresh(void)
{
    const ui_cfg_t *cfg = ui_cfg();
    const music_play_view_t *v = NULL;
    uint16_t count = 0;
    uint16_t cur = 0;
    uint16_t i;
    char title[32];

    if (s_body == NULL)
    {
        return;
    }

    lv_obj_clean(s_body);   /* 清空旧行，全量重建 */

    if (cfg->music != NULL)
    {
        v     = music_play_get_view(cfg->music);
        count = music_play_get_track_count(cfg->music);
        if (v != NULL)
        {
            cur = v->track_index;
        }
    }

    if (s_lblCount != NULL)
    {
        snprintf(title, sizeof(title), "本地音乐 (%u)", (unsigned)count);
        lv_label_set_text(s_lblCount, title);
    }

    if (cfg->music == NULL)
    {
        playlist_empty_add("未接入播放模块", "传入 MusicPlay 句柄后自动扫描");
        return;
    }
    if (count == 0)
    {
        if (SD_Card_Is_Mounted())
        {
            playlist_empty_add("暂无音乐文件", "请将 MP3/WAV 放入 /music 目录");
        }
        else
        {
            playlist_empty_add("SD 卡未挂载", "请检查 SD 卡后重启");
        }
        return;
    }

    for (i = 0; i < count; i++)
    {
        const music_track_t *t = music_play_get_track(cfg->music, i);
        if (t == NULL)
        {
            continue;
        }
        playlist_row_add(t, i, (i == cur));
    }
}

/* ======================== 事件回流（UI_Task 上下文消费） ======================== */

/** 处理 MusicPlay 事件：列表/当前曲变化时重建（其余事件与列表无关） */
void scr_playlist_apply_event(const music_event_t *evt)
{
    if (evt == NULL)
    {
        return;
    }

    switch (evt->id)
    {
    case MUSIC_EVT_PLAYLIST_CHANGED:
    case MUSIC_EVT_TRACK_CHANGED:
    case MUSIC_EVT_SOURCE_CHANGED:
        scr_playlist_refresh();
        break;

    default:
        break;
    }
}

/* ======================== 页面构建 ======================== */

void scr_playlist_build(lv_obj_t *page)
{
    lv_obj_t *back;
    lv_obj_t *lbl;

    /* 头部：返回键 + 计数标题 */
    back = lv_button_create(page);
    lv_obj_remove_style_all(back);
    lv_obj_set_pos(back, 4, 4);
    lv_obj_set_size(back, 44, 36);
    lv_obj_set_style_bg_color(back, UI_COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(back, 8, 0);
    lv_obj_set_style_border_color(back, UI_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_set_style_bg_color(back, UI_COLOR_PANEL_HI, LV_STATE_PRESSED);
    lv_obj_add_event_cb(back, back_click_cb, LV_EVENT_CLICKED, NULL);
    lbl = ui_label_create(back, UI_ICON_BACK, &ui_icons_24, UI_COLOR_TEXT);
    lv_obj_center(lbl);

    s_lblCount = ui_label_create(page, "本地音乐 (0)", &ui_font_cn_16, UI_COLOR_TEXT);
    lv_obj_set_pos(s_lblCount, 56, 12);

    /* 列表容器：可滚动 flex 纵列 */
    s_body = lv_obj_create(page);
    lv_obj_remove_style_all(s_body);
    lv_obj_set_pos(s_body, 4, PLAYLIST_HEADER_H);
    lv_obj_set_size(s_body, LCD_WIDTH - 8, UI_CONTENT_H - PLAYLIST_HEADER_H);
    lv_obj_set_style_bg_color(s_body, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_body, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_body, PLAYLIST_PAD, 0);
    lv_obj_set_style_pad_row(s_body, 2, 0);
    lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(s_body, LV_SCROLLBAR_MODE_OFF);

    scr_playlist_refresh();
}
