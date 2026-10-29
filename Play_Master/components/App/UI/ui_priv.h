/**
 * @file ui_priv.h
 * @brief UI 模块内部共享定义（布局/主题/导航/移植层），仅 UI 目录内使用
 */

#ifndef __UI_PRIV_H__
#define __UI_PRIV_H__

/* ======================== 是否打开 UI 模块调试打印 ======================== */
#define UI_DEBUG_LOGS    (false)

#include "esp_log.h"
#include "lvgl.h"
#include "ui.h"
#include "ui_icons.h"   /* 控件图标字形宏（配合 ui_icons_16/24 字体使用） */
#include "LCD_Touch.h"   /* LCD_WIDTH / LCD_HEIGHT 布局基准 */

#if UI_DEBUG_LOGS
#define UI_LOGE(...) ESP_LOGE("UI", __VA_ARGS__)
#define UI_LOGW(...) ESP_LOGW("UI", __VA_ARGS__)
#define UI_LOGI(...) ESP_LOGI("UI", __VA_ARGS__)
#define UI_LOGD(...) ESP_LOGD("UI", __VA_ARGS__)
#else
#define UI_LOGE(...) do { } while (0)
#define UI_LOGW(...) do { } while (0)
#define UI_LOGI(...) do { } while (0)
#define UI_LOGD(...) do { } while (0)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ======================== 主题色（深色主题：用户选定） ========================
 * 注：IPS 全背光下深色主题暗室里会有整体泛灰的物理观感（面板黑场 + 曝光），
 * 背景用纯黑 0x000000 与面板黑场对齐，泛白感最低。浅色版值保留在注释中可随时切回。 */
#define UI_COLOR_BG        lv_color_hex(0x000000)   /* 页面背景：纯黑（浅色版 0xF2F4F7） */
#define UI_COLOR_PANEL     lv_color_hex(0x14181D)   /* 卡片/面板（浅色版 0xFFFFFF） */
#define UI_COLOR_PANEL_HI  lv_color_hex(0x1E242B)   /* 面板高亮/按压（浅色版 0xE4E9F0） */
#define UI_COLOR_BORDER    lv_color_hex(0x2A323B)   /* 分隔/描边（浅色版 0xD5DBE3） */
#define UI_COLOR_PRIMARY   lv_color_hex(0x3D9BFF)   /* 主色：选中/进度（浅色版 0x2B7DE9） */
#define UI_COLOR_ACCENT    lv_color_hex(0xFFB03A)   /* 强调：灯开/音量（浅色版 0xE8890C） */
#define UI_COLOR_TEXT      lv_color_hex(0xE8EAED)   /* 文字主色（浅色版 0x1A1D21） */
#define UI_COLOR_TEXT_SUB  lv_color_hex(0x9AA0A6)   /* 文字次色（浅色版 0x5A6472） */
#define UI_COLOR_DISABLED  lv_color_hex(0x5F6368)   /* 禁用/占位（浅色版 0xA8B0BA） */
#define UI_COLOR_OK        lv_color_hex(0x51CF66)   /* 成功（浅色版 0x2F9E44） */
#define UI_COLOR_ERR       lv_color_hex(0xFF5C5C)   /* 错误（浅色版 0xE03131） */

/* ======================== 字体 ========================
 * 中文走运行时 FreeType 字体（ui_font.h 的 ui_font_text()），不再有编译进固件的
 * 中文字库；图标字体仍是 lv_font_conv 产物（PUA 码位，不是中文，保留）。 */
extern const lv_font_t ui_icons_16;     /* 控件图标 16px（状态栏） */
extern const lv_font_t ui_icons_24;     /* 控件图标 24px（Tab 栏/大按钮） */

/* ======================== 布局常量（UI 界面设计 1.3 三段式） ======================== */
#define UI_STATUSBAR_H   24
#define UI_TABBAR_H      40
#define UI_CONTENT_H     (LCD_HEIGHT - UI_STATUSBAR_H - UI_TABBAR_H)  /* 176 */

/* ======================== UI 图片资源（烧录进 flash storage 分区） ======================== */
/* main/CMakeLists.txt: spiffs_create_partition_image(storage ../storage FLASH_IN_PROJECT)
 * 将仓库 Play_Master/storage/ui_img/ 下的 PNG 随固件烧录，UI 不再读 SD 卡。 */
#define UI_IMG_MOUNT_POINT  "/storage"                      /* SPIFFS 挂载点 */
#define UI_IMG_LIGHT_ON     "F:/ui_img/Light_On.png"
#define UI_IMG_LIGHT_OFF    "F:/ui_img/Light_Off.png"

/* ======================== 页面导航 ======================== */
typedef enum {
    UI_PAGE_PLAYER = 0,   /* 播放页（默认主页） */
    UI_PAGE_LIGHT,        /* 灯控页 */
    UI_PAGE_SETTINGS,     /* 设置页 */
    UI_PAGE_PLAYLIST,     /* 本地音乐列表（二级页，播放页进入） */
    UI_PAGE_COUNT,
} ui_page_t;

/** 切页（多 screen 架构：lv_screen_load 整屏重绘，无残影；
 *  playlist 期间"播放"Tab 保持高亮） */
void ui_nav_goto(ui_page_t page);

/* ======================== 全局配置 ======================== */
/** 返回 ui_init() 保存的句柄配置（字段可能为 NULL = 占位模式） */
const ui_cfg_t *ui_cfg(void);

/* ======================== 主题辅助 ======================== */
void       ui_theme_init(void);  /* 主题初始化（字体已编译进固件，此处设全局默认样式） */
void       ui_style_card(lv_obj_t *obj);  /* 卡片面板统一样式 */
lv_obj_t  *ui_label_create(lv_obj_t *parent, const char *text,
                           const lv_font_t *font, lv_color_t color);

/* ======================== 显示/触摸/文件系统移植（ui_disp.c） ======================== */
esp_err_t ui_hw_bringup(void);      /* SPIFFS storage 挂载 + I2C 总线 + LCD/触摸初始化 */
esp_err_t ui_lvgl_port_init(void);  /* lv_init + tick + lv_fs('F') + 显示/触摸适配 */

/**
 * @brief 检查 flash 内 UI 图片是否存在（如 "F:/ui_img/Music_Album.png"）
 * @return true 存在可加载；false 不存在/SPIFFS 未挂载（调用方使用占位显示）
 */
bool ui_img_file_ok(const char *lv_path);

/**
 * @brief 建立"渲染期栈深"基准（必须在 ui_run() 最开头、建页面之前调用一次）
 */
void ui_disp_sp_probe_init(void);

/**
 * @brief 一次整屏刷新的耗时拆解（诊断用）
 *
 * 为什么需要 render_* 这组数：flush 回调只在「这一段已经画完」之后被调用，
 * 所以单个 flush 的耗时只包含 SPI 传输，不含绘制。相邻两次 flush 之间的空档
 * 恰好就是「下一段窗口的绘制耗时」，把它单独统计才能判断 550ms 到底花在
 * SPI 上还是花在 LVGL 软件绘制上，以及是不是每段耗时均匀
 * （均匀 = 每段固定开销大，抖动 = 某类对象/文字特别贵）。
 */
typedef struct {
    uint32_t flush_us;       /* 本次刷新累计 SPI 耗时 */
    uint32_t flush_calls;    /* flush 次数 = 渲染窗口数 */
    uint32_t render_us;      /* 各渲染窗口的绘制耗时累计（flush 之间的空档） */
    uint32_t render_count;   /* 参与统计的窗口数（首段无前一次 flush，不计） */
    uint32_t render_max_us;  /* 单窗口最长绘制耗时 */
    uint32_t depth_peak;     /* 渲染期峰值栈用量（含 ui_run 入口已用量） */
} ui_disp_stat_t;

/**
 * @brief 取走「自上次调用以来」的刷新统计（取走后清零）
 * @param st 输出；可为 NULL（仅清零）
 */
void ui_disp_stat_take(ui_disp_stat_t *st);

/* ======================== 页面构造（各 scr_*.c / statusbar / tabbar） ======================== */
void ui_statusbar_create(lv_obj_t *parent);
void ui_tabbar_create(lv_obj_t *parent, ui_page_t cur);   /* cur = 本屏对应页（Tab 初始高亮） */
void ui_tabbar_set_active(ui_page_t page);

void scr_player_build(lv_obj_t *page);
void scr_playlist_build(lv_obj_t *page);
void scr_light_build(lv_obj_t *page);
void scr_settings_build(lv_obj_t *page);

/** UI_Task 转发灯控事件到灯控页（事件队列消费侧） */
void scr_light_apply_event(const light_event_t *evt);

/** UI_Task 转发播放事件到播放页（事件队列消费侧） */
void scr_player_apply_event(const music_event_t *evt);

/** UI_Task 转发播放事件到本地列表（列表/当前曲变化时重建） */
void scr_playlist_apply_event(const music_event_t *evt);

/** UI_Task 转发 OTA 事件到设置页（进度/状态更新） */
void scr_settings_apply_ota_event(const ota_update_event_info_t *info);

/** 按 MusicPlay 当前状态重建本地列表（进入页面 / 事件回流时调用，UI_Task 上下文） */
void scr_playlist_refresh(void);

#ifdef __cplusplus
}
#endif

#endif /* __UI_PRIV_H__ */