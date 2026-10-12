/**
 * @file scr_player.c
 * @brief 播放页（Tab 1，默认主页；UI 界面设计 5.2）
 *
 * 布局：顶区 128px（左封面 128×128 + 右信息列）+ 控制行 48px。
 * 数据接线（第十一阶段，MusicPlay 已实现）：
 *   - 控制：music_play_command(TOGGLE/NEXT/PREV)、music_play_set_mode()、
 *     music_play_set_volume()；按键只做按压动效、绝不本地翻转状态，等
 *     MUSIC_EVT_* 回流刷新（UI 界面设计 5.2 / 6.2 "指令=期望"）；
 *   - 显示：music_play_get_view() 快照（源/状态/模式/音量/曲目）；
 *   - 进度：500ms lv_timer 轮询 music_play_get_position_ms()，总时长取
 *     view->track->duration_ms；时长未知（无曲目/未拿到元数据）时整段隐藏；
 *   - 刷新：MUSIC_EVT_* 经 app_main 接线 → ui_on_music_event → 事件队列 →
 *     UI_Task 调 scr_player_apply_event() 消费（线程约束见设计文档 6.8）；
 *   - 未注入 MusicPlay 句柄（ui_cfg.music == NULL）时退回占位日志。
 */

#include "ui_priv.h"
#include "esp_log.h"

#include <stdio.h>

static const char *TAG = "UI";

/* ======================== 资源路径（烧录在 flash storage 分区，见 ui_priv.h） ======================== */
#define PLAYER_COVER_PATH   UI_IMG_COVER

/* ======================== 控件句柄（第十一阶段真实数据接入） ======================== */
static lv_obj_t *s_lblPlayIcon;   /* 播放/暂停图标（view->state 决定） */
static lv_obj_t *s_lblModeIcon;   /* 顺序/单曲循环图标（view->mode 决定） */
static lv_obj_t *s_lblSrcChip;    /* 源 chip 文字（view->source 决定） */
static lv_obj_t *s_lblTitle;      /* 曲目标题（view->track->display_name） */
static lv_obj_t *s_lblSub;        /* 副标题（音源类型） */
static lv_obj_t *s_barProgress;   /* 进度条（0~1000，500ms 轮询刷新） */
static lv_obj_t *s_lblTime;       /* "00:00 / 00:00" */
static lv_obj_t *s_sliderVol;     /* 音量滑条（view->volume 回流刷新） */
static lv_obj_t *s_lblPrev;       /* ⏮ 图标（pending 态变灰） */
static lv_obj_t *s_lblNext;       /* ⏭ 图标（pending 态变灰） */
static lv_obj_t *s_btnList;       /* 列表入口（BT 源时置灰禁用） */
static lv_obj_t *s_lblListIcon;   /* 列表入口图标（随按钮一起置灰） */

static bool      s_srcBt = false;     /* 当前源（view->source 为准） */
static bool      s_volumeDrag = false;/* 音量拖动中：忽略 VOLUME_CHANGED 回流防抖动 */

/* ======================== 通用控制键（面板底 + 按压高亮 + 24px 图标） ======================== */
static lv_obj_t *ctrl_btn(lv_obj_t *parent, int x, int y, int w, int h,
                          const char *icon, lv_event_cb_t cb, lv_obj_t **lbl_out)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_t *lbl;

    lv_obj_remove_style_all(btn);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, UI_COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_border_color(btn, UI_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_bg_color(btn, UI_COLOR_PANEL_HI, LV_STATE_PRESSED);
    if (cb != NULL)
    {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    }

    lbl = ui_label_create(btn, icon, &ui_icons_24, UI_COLOR_TEXT);
    lv_obj_center(lbl);
    if (lbl_out != NULL)
    {
        *lbl_out = lbl;
    }
    return btn;
}

/* ======================== 时间格式化 ======================== */

/** ms → "mm:ss"（分钟钳到 999，缓冲 24B 足够且不会触发 -Wformat-truncation） */
static void player_fmt_time(char *buf, size_t n, uint32_t ms)
{
    uint32_t sec = ms / 1000u;
    uint32_t min = sec / 60u;

    if (min > 999u)
    {
        min = 999u;
    }
    snprintf(buf, n, "%02u:%02u", (unsigned)min, (unsigned)(sec % 60u));
}

/* ======================== 状态刷新（view 快照 → 控件） ======================== */

/**
 * 用 MusicPlay 视图快照刷新整页静态显示。
 * 调用上下文：UI_Task（页面构建时，或 MUSIC_EVT_* 消费时）。
 */
static void player_apply_view(const music_play_view_t *v)
{
    lv_color_t ctrlColor;
    lv_color_t chipColor;

    if (v == NULL)
    {
        return;
    }

    /* 源 chip */
    s_srcBt = (v->source == MUSIC_SOURCE_BT);
    chipColor = (v->source == MUSIC_SOURCE_BT) ? UI_COLOR_PRIMARY : UI_COLOR_ACCENT;
    if (s_lblSrcChip != NULL)
    {
        lv_label_set_text(s_lblSrcChip, s_srcBt ? "蓝牙" : "本地");
        lv_obj_set_style_text_color(s_lblSrcChip, chipColor, 0);
    }

    /* 标题 / 副标题 */
    if (s_lblTitle != NULL)
    {
        lv_label_set_text(s_lblTitle,
                          (v->track != NULL) ? v->track->display_name : "无曲目");
    }
    if (s_lblSub != NULL)
    {
        lv_label_set_text(s_lblSub, s_srcBt ? "蓝牙音频" : "本地音乐");
    }

    /* ▶/⏸ 与 🔁 图标（只由回流快照驱动，绝不本地翻转） */
    if (s_lblPlayIcon != NULL)
    {
        lv_label_set_text(s_lblPlayIcon,
                          (v->state == MUSIC_STATE_PLAYING) ? UI_ICON_PAUSE : UI_ICON_PLAY);
    }
    if (s_lblModeIcon != NULL)
    {
        lv_label_set_text(s_lblModeIcon,
                          (v->mode == MUSIC_PLAY_MODE_SINGLE_LOOP) ? UI_ICON_REPEAT_ONE
                                                                   : UI_ICON_REPEAT);
    }

    /* 主控键"等待回流"态：pending_cmd 时图标转灰（设计文档 5.2） */
    ctrlColor = v->pending_cmd ? UI_COLOR_DISABLED : UI_COLOR_TEXT;
    if (s_lblPrev != NULL)
    {
        lv_obj_set_style_text_color(s_lblPrev, ctrlColor, 0);
    }
    if (s_lblNext != NULL)
    {
        lv_obj_set_style_text_color(s_lblNext, ctrlColor, 0);
    }
    if (s_lblPlayIcon != NULL)
    {
        lv_obj_set_style_text_color(s_lblPlayIcon, ctrlColor, 0);
    }

    /* 音量滑条：拖动中不覆盖用户手指位置（防回流抖动） */
    if (s_sliderVol != NULL && !s_volumeDrag)
    {
        lv_slider_set_value(s_sliderVol, (int32_t)v->volume, LV_ANIM_OFF);
    }

    /* 列表入口：列表是本地源功能，BT 源时置灰（点击回调内同样拦截） */
    if (s_btnList != NULL)
    {
        lv_obj_set_style_opa(s_btnList, s_srcBt ? LV_OPA_40 : LV_OPA_COVER, 0);
    }
    if (s_lblListIcon != NULL)
    {
        lv_obj_set_style_text_color(s_lblListIcon,
                                    s_srcBt ? UI_COLOR_DISABLED : UI_COLOR_TEXT, 0);
    }
}

/** 进度条 + 时间：时长未知（无曲目/无元数据）时整段隐藏 */
static void player_refresh_progress(void)
{
    const ui_cfg_t *cfg = ui_cfg();
    const music_play_view_t *v;
    uint32_t pos = 0;
    uint32_t dur = 0;
    char tPos[24];
    char tDur[24];
    char line[64];

    if (cfg->music == NULL || s_barProgress == NULL || s_lblTime == NULL)
    {
        return;
    }

    v = music_play_get_view(cfg->music);
    if (v == NULL || v->track == NULL || v->track->duration_ms == 0)
    {
        lv_obj_add_flag(s_barProgress, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lblTime, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    dur = v->track->duration_ms;
    (void)music_play_get_position_ms(cfg->music, &pos);
    if (pos > dur)
    {
        pos = dur;
    }

    lv_obj_clear_flag(s_barProgress, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_lblTime, LV_OBJ_FLAG_HIDDEN);
    lv_bar_set_value(s_barProgress, (int32_t)((uint64_t)pos * 1000u / dur), LV_ANIM_OFF);

    player_fmt_time(tPos, sizeof(tPos), pos);
    player_fmt_time(tDur, sizeof(tDur), dur);
    snprintf(line, sizeof(line), "%s / %s", tPos, tDur);
    lv_label_set_text(s_lblTime, line);
}

static void player_progress_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    player_refresh_progress();
}

/* ======================== 按键事件（只发"期望"，等事件回流） ======================== */

static void play_click_cb(lv_event_t *e)
{
    const ui_cfg_t *cfg = ui_cfg();
    esp_err_t ret;
    (void)e;

    if (cfg->music == NULL)
    {
        ESP_LOGI(TAG, "[占位] 播放/暂停 -> TODO: music_play_command(MUSIC_CMD_TOGGLE)（未注入句柄）");
        return;
    }
    ret = music_play_command(cfg->music, MUSIC_CMD_TOGGLE);
    ESP_LOGI(TAG, "播放/暂停 -> music_play_command(TOGGLE): %s", esp_err_to_name(ret));
}

static void prev_click_cb(lv_event_t *e)
{
    const ui_cfg_t *cfg = ui_cfg();
    esp_err_t ret;
    (void)e;

    if (cfg->music == NULL)
    {
        ESP_LOGI(TAG, "[占位] 上一首 -> TODO: music_play_command(MUSIC_CMD_PREV)（未注入句柄）");
        return;
    }
    ret = music_play_command(cfg->music, MUSIC_CMD_PREV);
    ESP_LOGI(TAG, "上一首 -> music_play_command(PREV): %s", esp_err_to_name(ret));
}

static void next_click_cb(lv_event_t *e)
{
    const ui_cfg_t *cfg = ui_cfg();
    esp_err_t ret;
    (void)e;

    if (cfg->music == NULL)
    {
        ESP_LOGI(TAG, "[占位] 下一首 -> TODO: music_play_command(MUSIC_CMD_NEXT)（未注入句柄）");
        return;
    }
    ret = music_play_command(cfg->music, MUSIC_CMD_NEXT);
    ESP_LOGI(TAG, "下一首 -> music_play_command(NEXT): %s", esp_err_to_name(ret));
}

static void mode_click_cb(lv_event_t *e)
{
    const ui_cfg_t *cfg = ui_cfg();
    const music_play_view_t *v;
    music_play_mode_t next;
    esp_err_t ret;
    (void)e;

    if (cfg->music == NULL)
    {
        ESP_LOGI(TAG, "[占位] 播放模式 -> TODO: music_play_set_mode()（未注入句柄）");
        return;
    }

    /* 以 view->mode 为准取反（图标等 MUSIC_EVT_PLAY_MODE_CHANGED 回流） */
    v    = music_play_get_view(cfg->music);
    next = (v != NULL && v->mode == MUSIC_PLAY_MODE_SINGLE_LOOP)
               ? MUSIC_PLAY_MODE_SEQUENTIAL : MUSIC_PLAY_MODE_SINGLE_LOOP;
    ret  = music_play_set_mode(cfg->music, next);
    ESP_LOGI(TAG, "播放模式 -> music_play_set_mode(%s): %s",
             (next == MUSIC_PLAY_MODE_SINGLE_LOOP) ? "SINGLE_LOOP" : "SEQUENTIAL",
             esp_err_to_name(ret));
}

static void chip_click_cb(lv_event_t *e)
{
    const ui_cfg_t *cfg = ui_cfg();
    music_source_t want;
    esp_err_t ret;
    (void)e;

    if (cfg->music == NULL)
    {
        /* 未注入 MusicPlay 句柄：占位日志（无状态可切） */
        ESP_LOGI(TAG, "[占位] 切换播放源 -> TODO: music_play_set_source()（未注入句柄）");
        return;
    }

    /* 真实接入：只下发"期望源"，界面等 MUSIC_EVT_SOURCE_CHANGED 回流刷新，不本地翻转
     * （蓝牙已连接时会被自动轮询规则拉回，见 UI 界面设计 6.3） */
    want = (music_play_get_source(cfg->music) == MUSIC_SOURCE_BT)
               ? MUSIC_SOURCE_LOCAL : MUSIC_SOURCE_BT;
    ret  = music_play_set_source(cfg->music, want);
    ESP_LOGI(TAG, "chip -> music_play_set_source(%s): %s",
             (want == MUSIC_SOURCE_BT) ? "BT" : "LOCAL", esp_err_to_name(ret));
}

static void list_click_cb(lv_event_t *e)
{
    const ui_cfg_t *cfg = ui_cfg();
    (void)e;

    /* 列表是本地源功能：蓝牙源时禁用（UI 界面设计 5.2） */
    if (s_srcBt)
    {
        ESP_LOGI(TAG, "列表入口：蓝牙源下不可用");
        return;
    }
    if (cfg->music != NULL)
    {
        music_play_scan_local(cfg->music);   /* 异步：扫完回 MUSIC_EVT_PLAYLIST_CHANGED */
    }
    ui_nav_goto(UI_PAGE_PLAYLIST);
    scr_playlist_refresh();                  /* 先按当前已知列表渲染一次 */
}

static void volume_cb(lv_event_t *e)
{
    const ui_cfg_t *cfg = ui_cfg();
    lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
    lv_event_code_t code = lv_event_get_code(e);
    static uint32_t logCnt = 0;
    int val;
    esp_err_t ret;

    /* 注册的是 LV_EVENT_ALL，这里必须白名单：绘制/覆盖检查等事件一律忽略，
     * 否则会被误当成音量变化反复下发 */
    if (code == LV_EVENT_PRESSED)
    {
        s_volumeDrag = true;      /* 按下：屏蔽 VOLUME_CHANGED 回流，防手指位置被覆盖 */
        return;
    }
    if (code == LV_EVENT_PRESS_LOST)
    {
        s_volumeDrag = false;     /* 拖动被打断：恢复回流 */
        return;
    }
    if (code != LV_EVENT_VALUE_CHANGED && code != LV_EVENT_RELEASED)
    {
        return;
    }

    val = (int)lv_slider_get_value(slider);
    if (cfg->music == NULL)
    {
        if ((++logCnt % 10) == 1)
        {
            ESP_LOGI(TAG, "[占位] 音量 %d -> TODO: music_play_set_volume(%d)（未注入句柄）", val, val);
        }
        if (code == LV_EVENT_RELEASED)
        {
            s_volumeDrag = false;
        }
        return;
    }

    ret = music_play_set_volume(cfg->music, (uint8_t)val);
    if (ret != ESP_OK || (++logCnt % 10) == 1)
    {
        ESP_LOGI(TAG, "音量 %d -> music_play_set_volume: %s", val, esp_err_to_name(ret));
    }
    if (code == LV_EVENT_RELEASED)
    {
        s_volumeDrag = false;     /* 拖动结束：恢复回流刷新 */
    }
}

/* ======================== 事件回流（UI_Task 上下文消费） ======================== */

/** 处理 MusicPlay 事件：整体快照刷新播放页 + 进度条 */
void scr_player_apply_event(const music_event_t *evt)
{
    if (evt == NULL)
    {
        return;
    }

    switch (evt->id)
    {
    case MUSIC_EVT_SOURCE_CHANGED:
        player_apply_view(&evt->view);
        ESP_LOGI(TAG, "music evt: source -> %s", (evt->view.source == MUSIC_SOURCE_BT) ? "BT" : "LOCAL");
        break;

    case MUSIC_EVT_PLAY_STATE_CHANGED:
        player_apply_view(&evt->view);
        ESP_LOGI(TAG, "music evt: state -> %d", (int)evt->view.state);
        break;

    case MUSIC_EVT_TRACK_CHANGED:
    case MUSIC_EVT_TRACK_INFO:
    case MUSIC_EVT_PLAY_MODE_CHANGED:
        player_apply_view(&evt->view);
        break;

    case MUSIC_EVT_VOLUME_CHANGED:
        if (s_sliderVol != NULL && !s_volumeDrag)
        {
            lv_slider_set_value(s_sliderVol, (int32_t)evt->view.volume, LV_ANIM_OFF);
        }
        ESP_LOGI(TAG, "music evt: volume -> %u", (unsigned)evt->view.volume);
        break;

    case MUSIC_EVT_EOF:
        ESP_LOGI(TAG, "music evt: EOF");
        break;

    case MUSIC_EVT_PLAYLIST_CHANGED:
        break;   /* 列表页由 scr_playlist_apply_event 重建 */

    case MUSIC_EVT_ERROR:
        player_apply_view(&evt->view);
        ESP_LOGW(TAG, "music evt: ERROR (src=%d state=%d)",
                 (int)evt->view.source, (int)evt->view.state);
        break;

    default:
        break;
    }

    player_refresh_progress();
}

/* ======================== 页面构建 ======================== */

/** 封面：SD 资源存在用 PNG，否则深色面板 + 音符图标占位（8.4 容错） */
static void cover_build(lv_obj_t *page)
{
    lv_obj_t *cover = lv_image_create(page);

    lv_obj_remove_style_all(cover);
    lv_obj_set_pos(cover, 0, 0);
    lv_obj_set_size(cover, UI_COVER_SIZE, UI_COVER_SIZE);
    lv_obj_set_style_bg_color(cover, UI_COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(cover, 8, 0);
    lv_obj_set_style_clip_corner(cover, true, 0);

    if (ui_img_file_ok(PLAYER_COVER_PATH))
    {
        lv_image_set_src(cover, PLAYER_COVER_PATH);
        /* 用户图片尺寸不一定是 128：CONTAIN 等比缩放适配控件区域 */
        lv_image_set_inner_align(cover, LV_IMAGE_ALIGN_CONTAIN);
        ESP_LOGI(TAG, "cover loaded: %s", PLAYER_COVER_PATH);
    }
    else
    {
        lv_obj_t *icon = ui_label_create(cover, UI_ICON_MUSIC, &ui_icons_24,
                                         UI_COLOR_DISABLED);
        lv_obj_align(icon, LV_ALIGN_CENTER, 0, 0);
        ESP_LOGW(TAG, "cover missing: %s（SPIFFS 未挂载/镜像未烧录/"
                      "未开启 CONFIG_LV_USE_LODEPNG），使用占位图", PLAYER_COVER_PATH);
    }
}

void scr_player_build(lv_obj_t *page)
{
    const ui_cfg_t *cfg = ui_cfg();
    const int colX = UI_COVER_SIZE + 4;         /* 右信息列起点 x=132 */
    const int colW = LCD_WIDTH - colX - 4;      /* 184 */
    lv_obj_t *chip;
    lv_obj_t *listBtn;
    lv_obj_t *lbl;

    /* ---- 顶区左：封面 128×128 ---- */
    cover_build(page);

    /* ---- 顶区右：源 chip + 列表入口 ---- */
    chip = lv_button_create(page);
    lv_obj_remove_style_all(chip);
    lv_obj_set_pos(chip, colX, 4);
    lv_obj_set_size(chip, 64, 26);
    lv_obj_set_style_bg_color(chip, UI_COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(chip, 13, 0);
    lv_obj_set_style_border_color(chip, UI_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(chip, 1, 0);
    lv_obj_set_style_bg_color(chip, UI_COLOR_PANEL_HI, LV_STATE_PRESSED);
    lv_obj_add_event_cb(chip, chip_click_cb, LV_EVENT_CLICKED, NULL);
    s_lblSrcChip = ui_label_create(chip, "本地", &ui_font_cn_16, UI_COLOR_ACCENT);
    lv_obj_center(s_lblSrcChip);

    s_btnList = lv_button_create(page);
    lv_obj_remove_style_all(s_btnList);
    lv_obj_set_pos(s_btnList, LCD_WIDTH - 4 - 32, 4);
    lv_obj_set_size(s_btnList, 32, 26);
    lv_obj_set_style_bg_color(s_btnList, UI_COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(s_btnList, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_btnList, 8, 0);
    lv_obj_set_style_bg_color(s_btnList, UI_COLOR_PANEL_HI, LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_btnList, list_click_cb, LV_EVENT_CLICKED, NULL);
    s_lblListIcon = ui_label_create(s_btnList, UI_ICON_LIST, &ui_icons_16, UI_COLOR_TEXT);
    lv_obj_center(s_lblListIcon);

    /* ---- 顶区右：标题 / 副标题（随 MUSIC_EVT_TRACK_* 更新） ---- */
    s_lblTitle = ui_label_create(page, "无曲目", &ui_font_cn_16, UI_COLOR_TEXT);
    lv_label_set_long_mode(s_lblTitle, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_pos(s_lblTitle, colX, 38);
    lv_obj_set_width(s_lblTitle, colW);

    s_lblSub = ui_label_create(page, "本地音乐", &ui_font_cn_16, UI_COLOR_TEXT_SUB);
    lv_obj_set_pos(s_lblSub, colX, 60);
    lv_obj_set_width(s_lblSub, colW);

    /* ---- 顶区右：进度条 + 时间（500ms 轮询；时长未知时整段隐藏） ---- */
    s_barProgress = lv_bar_create(page);
    lv_obj_set_pos(s_barProgress, colX, 82);
    lv_obj_set_size(s_barProgress, colW, 8);
    lv_bar_set_range(s_barProgress, 0, 1000);
    lv_bar_set_value(s_barProgress, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_barProgress, UI_COLOR_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_barProgress, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_barProgress, UI_COLOR_PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_barProgress, LV_OPA_COVER, LV_PART_INDICATOR);

    s_lblTime = ui_label_create(page, "00:00 / 00:00", &ui_font_cn_16, UI_COLOR_TEXT_SUB);
    lv_obj_set_pos(s_lblTime, colX, 94);

    /* ---- 顶区右：音量 ---- */
    lbl = ui_label_create(page, UI_ICON_VOLUME, &ui_icons_16, UI_COLOR_ACCENT);
    lv_obj_set_pos(lbl, colX, 112);

    s_sliderVol = lv_slider_create(page);
    lv_obj_set_pos(s_sliderVol, colX + 22, 112);
    lv_obj_set_size(s_sliderVol, colW - 22, 12);
    lv_slider_set_range(s_sliderVol, 0, 100);
    lv_slider_set_value(s_sliderVol, (int32_t)music_play_get_volume(cfg->music), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_sliderVol, UI_COLOR_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_sliderVol, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_sliderVol, UI_COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_sliderVol, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_sliderVol, UI_COLOR_ACCENT, LV_PART_KNOB);
    lv_obj_add_event_cb(s_sliderVol, volume_cb, LV_EVENT_ALL, NULL);

    /* ---- 控制行：模式 + ⏮ ▶⏸ ⏭（键高压缩到 36px 并下移，
     *      与音量条拉开 12px 间距，避免误触） ---- */
    ctrl_btn(page, 46, 136, 40, 36, UI_ICON_REPEAT, mode_click_cb, &s_lblModeIcon);
    ctrl_btn(page, 98, 136, 48, 36, UI_ICON_PREV, prev_click_cb, &s_lblPrev);
    ctrl_btn(page, 158, 136, 56, 36, UI_ICON_PLAY, play_click_cb, &s_lblPlayIcon);
    ctrl_btn(page, 226, 136, 48, 36, UI_ICON_NEXT, next_click_cb, &s_lblNext);

    /* ---- 初始状态：用 MusicPlay 快照拉一次（含音量/源/模式/图标） ---- */
    if (cfg->music != NULL)
    {
        player_apply_view(music_play_get_view(cfg->music));
    }
    player_refresh_progress();
    lv_timer_create(player_progress_timer_cb, 500, NULL);
}
