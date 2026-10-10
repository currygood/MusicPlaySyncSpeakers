/**
 * @file scr_player.c
 * @brief 播放页（Tab 1，默认主页；UI 界面设计 5.2）
 *
 * 第八阶段布局：顶区 128px（左封面 128×128 + 右信息列）+ 控制行 48px。
 * 播放控制按键仍为占位：按下后本地图标反馈 + ESP_LOGI 注明将来调用的真实
 * 接口（music_play_command / set_mode / set_volume，第十阶段接入）；
 * 源切换已接真实接口（music_play_set_source / get_source，事件回流刷新）。
 * 真实数据接入点（已留好）：
 *   - 标题/副标题 ← music_play_get_view()->track（9.2 建议落地后 BT 源同源）
 *   - 进度       ← 500ms 轮询 music_play_get_position_ms()（本地源；BT 源隐藏）
 *   - 按键       ← music_play_command(TOGGLE/NEXT/PREV)
 *   - 音量       ← music_play_set_volume()，事件回流刷新（不本地乐观提交）
 */

#include "ui_priv.h"
#include "esp_log.h"

static const char *TAG = "UI";

/* ======================== 资源路径（烧录在 flash storage 分区，见 ui_priv.h） ======================== */
#define PLAYER_COVER_PATH   UI_IMG_COVER

/* ======================== 控件句柄（后续阶段接真实数据用） ======================== */
static lv_obj_t *s_lblPlayIcon;   /* 播放/暂停图标（演示期本地切换） */
static lv_obj_t *s_lblModeIcon;   /* 顺序/单曲循环图标 */
static lv_obj_t *s_lblSrcChip;    /* 源 chip 文字（演示期本地切换） */
static lv_obj_t *s_barProgress;   /* 进度条（0~1000，接入后 500ms 轮询刷新） */
static lv_obj_t *s_lblTime;       /* "00:00 / 00:00" */
static bool      s_playing = false;
static bool      s_singleLoop = false;
static bool      s_srcBt = true;  /* 当前源：true=蓝牙（有句柄时以事件/查询为准） */

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

/* ======================== 按键事件（占位日志 + 本地图标反馈） ======================== */

static void play_click_cb(lv_event_t *e)
{
    (void)e;
    s_playing = !s_playing;
    lv_label_set_text(s_lblPlayIcon, s_playing ? UI_ICON_PAUSE : UI_ICON_PLAY);
    /* 真实接入（第十阶段）：不发命令也绝不本地翻转状态，仅等 MUSIC_EVT 回流 */
    ESP_LOGI(TAG, "[占位] 播放/暂停 -> TODO: music_play_command(MUSIC_CMD_TOGGLE)（第十阶段接入）");
}

static void prev_click_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "[占位] 上一首 -> TODO: music_play_command(MUSIC_CMD_PREV)（第十阶段接入）");
}

static void next_click_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "[占位] 下一首 -> TODO: music_play_command(MUSIC_CMD_NEXT)（第十阶段接入）");
}

static void mode_click_cb(lv_event_t *e)
{
    (void)e;
    s_singleLoop = !s_singleLoop;
    lv_label_set_text(s_lblModeIcon, s_singleLoop ? UI_ICON_REPEAT_ONE : UI_ICON_REPEAT);
    ESP_LOGI(TAG, "[占位] 播放模式%s -> TODO: music_play_set_mode(%s)（第十阶段接入）",
             s_singleLoop ? "单曲循环" : "顺序播放",
             s_singleLoop ? "MUSIC_PLAY_MODE_SINGLE_LOOP" : "MUSIC_PLAY_MODE_SEQUENTIAL");
}

static void chip_click_cb(lv_event_t *e)
{
    const ui_cfg_t *cfg = ui_cfg();
    music_source_t want;
    esp_err_t ret;
    (void)e;

    if (cfg->music == NULL)
    {
        /* 未注入 MusicPlay 句柄：本地演示切换（占位） */
        s_srcBt = !s_srcBt;
        lv_label_set_text(s_lblSrcChip, s_srcBt ? "蓝牙" : "本地");
        ESP_LOGI(TAG, "[占位] 切换播放源为%s -> TODO: music_play_set_source(MUSIC_SOURCE_%s)",
                 s_srcBt ? "蓝牙" : "本地", s_srcBt ? "BT" : "LOCAL");
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
    (void)e;
    ui_nav_goto(UI_PAGE_PLAYLIST);
}

static void volume_cb(lv_event_t *e)
{
    static uint32_t logCnt = 0;
    lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
    int val = (int)lv_slider_get_value(slider);

    /* 拖动过程日志限频（每 10 次打一条），真实接入后这条日志换成 set_volume 调用 */
    if ((++logCnt % 10) == 1)
    {
        ESP_LOGI(TAG, "[占位] 音量 %d -> TODO: music_play_set_volume(%d)（第十阶段接入）", val, val);
    }
}

/* ======================== 事件回流（UI_Task 上下文消费） ======================== */

/** 处理 MusicPlay 事件：源变化刷新源 chip；其余事件在 UI 接线阶段补齐 */
void scr_player_apply_event(const music_event_t *evt)
{
    if (evt == NULL)
    {
        return;
    }

    switch (evt->id)
    {
    case MUSIC_EVT_SOURCE_CHANGED:
        s_srcBt = (evt->view.source == MUSIC_SOURCE_BT);
        if (s_lblSrcChip != NULL)
        {
            lv_label_set_text(s_lblSrcChip, s_srcBt ? "蓝牙" : "本地");
        }
        ESP_LOGI(TAG, "music evt: source -> %s", s_srcBt ? "BT" : "LOCAL");
        break;

    case MUSIC_EVT_ERROR:
        ESP_LOGW(TAG, "music evt: ERROR (src=%d state=%d)",
                 (int)evt->view.source, (int)evt->view.state);
        break;

    default:
        /* 播放状态/曲目/音量/模式等事件在 UI 接线阶段补齐 */
        break;
    }
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
    lv_obj_t *title;
    lv_obj_t *sub;
    lv_obj_t *volIcon;

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
    s_lblSrcChip = ui_label_create(chip, "蓝牙", &ui_font_cn_16, UI_COLOR_PRIMARY);
    lv_obj_center(s_lblSrcChip);
    /* 初始源：MusicPlay 已先行创建，读一次缓存源（内部加锁，UI_Task 启动前安全） */
    if (cfg->music != NULL)
    {
        s_srcBt = (music_play_get_source(cfg->music) == MUSIC_SOURCE_BT);
        lv_label_set_text(s_lblSrcChip, s_srcBt ? "蓝牙" : "本地");
    }

    listBtn = lv_button_create(page);
    lv_obj_remove_style_all(listBtn);
    lv_obj_set_pos(listBtn, LCD_WIDTH - 4 - 32, 4);
    lv_obj_set_size(listBtn, 32, 26);
    lv_obj_set_style_bg_color(listBtn, UI_COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(listBtn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(listBtn, 8, 0);
    lv_obj_set_style_bg_color(listBtn, UI_COLOR_PANEL_HI, LV_STATE_PRESSED);
    lv_obj_add_event_cb(listBtn, list_click_cb, LV_EVENT_CLICKED, NULL);
    title = ui_label_create(listBtn, UI_ICON_LIST, &ui_icons_16, UI_COLOR_TEXT);
    lv_obj_center(title);

    /* ---- 顶区右：标题 / 副标题（接入后随 MUSIC_EVT_TRACK 更新） ---- */
    title = ui_label_create(page, "未接入播放模块", &ui_font_cn_16, UI_COLOR_TEXT);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_pos(title, colX, 38);
    lv_obj_set_width(title, colW);

    sub = ui_label_create(page, "等待 MusicPlay 接入", &ui_font_cn_16, UI_COLOR_TEXT_SUB);
    lv_obj_set_pos(sub, colX, 60);
    lv_obj_set_width(sub, colW);

    /* ---- 顶区右：进度条 + 时间（本地源 500ms 轮询；BT 源整段隐藏） ---- */
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
    volIcon = ui_label_create(page, UI_ICON_VOLUME, &ui_icons_16, UI_COLOR_ACCENT);
    lv_obj_set_pos(volIcon, colX, 112);

    lv_obj_t *slider = lv_slider_create(page);
    lv_obj_set_pos(slider, colX + 22, 112);
    lv_obj_set_size(slider, colW - 22, 12);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, 50, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, UI_COLOR_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, UI_COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, UI_COLOR_ACCENT, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, volume_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* ---- 控制行：模式 + ⏮ ▶⏸ ⏭（键高压缩到 36px 并下移，
     *      与音量条拉开 12px 间距，避免误触） ---- */
    ctrl_btn(page, 46, 136, 40, 36, UI_ICON_REPEAT, mode_click_cb, &s_lblModeIcon);
    ctrl_btn(page, 98, 136, 48, 36, UI_ICON_PREV, prev_click_cb, NULL);
    ctrl_btn(page, 158, 136, 56, 36, UI_ICON_PLAY, play_click_cb, &s_lblPlayIcon);
    ctrl_btn(page, 226, 136, 48, 36, UI_ICON_NEXT, next_click_cb, NULL);
}
