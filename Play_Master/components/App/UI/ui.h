/**
 * @file ui.h
 * @brief UI 模块接口（App 层），对应《主音频节点软件架构分层设计》3.3 UI 行
 *        与《UI 界面设计》（docs/02-设计/UI界面设计.md）
 *
 * 模块职责（第八阶段：界面联调）：
 *   - 自持 UI_Task（Core 1 / 优先级 12 / 栈 13312，架构文档第 6 章任务表）内跑
 *     LVGL 9.6，所有 lv_* 调用只发生在 UI_Task；
 *   - 显示/触摸经 middlewares/LCD_Touch 既有接口适配（ui_disp.c）；
 *   - 页面：状态栏 + 播放 / 灯控 / 设置 三个 Tab + 本地音乐列表二级页；
 *   - 已实现模块（LightControl / CallPhone / wifi_manager）句柄注入后走真实
 *     接口；未注入或未实现的模块（MusicPlay / PlayMode / sync_protocol / OTA）
 *     触摸后仅 ESP_LOGI 占位提示，后续阶段补齐（真实调用点已留在代码里）。
 *
 * 依赖注入：ui_cfg_t 各句柄可 NULL，NULL 时对应功能为占位显示。
 */

#ifndef __UI_H__
#define __UI_H__

#include "esp_err.h"
#include "light_control.h"   /* light_control_handle_t / light_event_t */
#include "call_phone.h"      /* call_phone_handle_t */
#include "wifi_manager.h"    /* wifi_manager_handle_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ======================== 类型定义 ================================================== */

/** 创建参数（全部可空：NULL 的句柄对应功能显示占位 + 日志） */
typedef struct {
    wifi_manager_handle_t  wifi;       /* 网络状态显示：NULL 时状态栏/设置页占位 */
    light_control_handle_t light;      /* 灯控页真实开关：NULL 时本地演示 + 日志 */
    call_phone_handle_t    call_phone; /* 语音状态显示：NULL 时占位 */
    /* 后续阶段扩展（第九~十二）：music_play / play_mode / sync_protocol / OTA，
     * UI 内部已按"NULL = 占位日志"设计，扩展时在此加字段即可 */
} ui_cfg_t;

/* ======================== API ============================================================ */

/**
 * @brief UI 初始化：SPIFFS storage 挂载 → I2C 总线 → LCD/触摸 → LVGL → 页面 → UI_Task
 *
 * 调用时机：app_main 中各模块创建之后（LightControl 若要真实控制需先 create
 * 并把句柄传入 cfg）。cfg 可传 NULL，等价于全 NULL（全占位模式）。
 *
 * @return ESP_OK 成功；失败时 UI 不可用（屏幕黑屏，详见日志）
 */
esp_err_t ui_init(const ui_cfg_t *cfg);

/** @brief 反初始化（整机下电路径使用；正常运行不需要调用） */
void ui_deinit(void);

/**
 * @brief 灯控事件接线入口（app_main 创建 LightControl 时把 cfg.on_event 指向本函数）
 *
 * 调用上下文：SmartHome_Task。内部仅入队，由 UI_Task 刷新灯卡片，禁止阻塞。
 */
void ui_on_light_event(const light_event_t *evt, void *user_ctx);

#ifdef __cplusplus
}
#endif

#endif /* __UI_H__ */
