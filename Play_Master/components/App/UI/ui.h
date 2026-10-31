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
 *   - 已实现模块（MusicPlay / LightControl / wifi_manager）句柄注入后
 *     走真实接口；MusicPlay 已全量接线（源切换、播放/暂停/上下曲、播放模式、音量、
 *     进度轮询、本地列表，见 scr_player.c / scr_playlist.c，第十一阶段）；
 *   - 未注入或未实现的模块（sync_protocol / OTA / WiFi 配网）触摸后仅 ESP_LOGI 占位提示。
 *
 * 依赖注入：ui_cfg_t 各句柄可 NULL，NULL 时对应功能为占位显示。
 */

#ifndef __UI_H__
#define __UI_H__

#include "esp_err.h"
#include "light_control.h"   /* light_control_handle_t / light_event_t */
#include "music_play.h"      /* music_play_handle_t / music_event_t（事件接线入口） */
#include "wifi_manager.h"    /* wifi_manager_handle_t */
#include "OTA_Update.h"      /* ota_update_event_info_t（OTA 事件接线入口） */

#ifdef __cplusplus
extern "C" {
#endif

/* ======================== UI 任务栈 ========================
 * 全工程唯一来源：app_main.c 的 APP_UI_TASK_STACK 直接引用本宏。
 *
 * 注意 uxTaskGetStackHighWaterMark() 在 ESP-IDF 里返回**字节**（不是标准 FreeRTOS
 * 的字），且返回的是"历史最小剩余"。实测量级：
 *   页面构建完成    used≈2.3KB
 *   首次整屏渲染后  used≈6.3KB（FreeType 光栅化 + LVGL 绘制最深）
 *
 * 为什么从 12288 再提到 24576（实测踩坑，两次）：
 *   12288 时 REFR_READY 采样仍有 5980 B 余量，但紧接着就报
 *   "stack overflow in task UI_Task"。
 *   20480 时更离谱：启动后 headroom=18100 B，8 秒后点设置页仍然压穿栈底。
 * 真实峰值（24576 档实测，2026-10-28）：
 *   点开设置页整屏渲染后 headroom=724 B → **峰值用量 23852 B**。
 *   这就是 12288 / 20480 两次崩溃的直接原因，不是"余量估计偏保守"。
 *   23.8KB 对一页 320x240 的绘制明显偏高（同一份代码在别处只用 6KB 量级），
 *   大概率是深层递归或渲染期间恰好叠了一次 WiFi/BT 中断（ESP32 上中断用被打断
 *   任务的栈）——所以峰值本身有波动，不能按 23852 + 1KB 去抠。
 *   现取 32768：比实测峰值留 ~8.7KB，够叠 1~2 层中断。
 *   日志里的 `depth peak` 是 SP 实测值，继续观察其它页面，若都远低于 23.8KB
 *   再回头查这页为什么这么深（技术债，见 .workbuddy/memory/2026-10-28.md）。 */
#define UI_TASK_STACK_BYTES     32768

/* ======================== 类型定义 ================================================== */

/** 创建参数（全部可空：NULL 的句柄对应功能显示占位 + 日志） */
typedef struct {
    wifi_manager_handle_t  wifi;       /* 网络状态显示：NULL 时状态栏/设置页占位 */
    light_control_handle_t light;      /* 灯控页真实开关：NULL 时本地演示 + 日志 */
    music_play_handle_t    music;      /* 播放页全量（控制/显示/列表）：NULL 时占位日志 */
    /* 后续阶段扩展：sync_protocol / OTA；UI 内部已按"NULL = 占位日志"设计 */
} ui_cfg_t;

/* ======================== API ============================================================ */

/**
 * @brief UI 初始化：SPIFFS storage 挂载 → I2C 总线 → LCD/触摸 → LVGL
 *
 * 调用时机：app_main 中各模块创建之后（LightControl 若要真实控制需先 create
 * 并把句柄传入 cfg）。cfg 可传 NULL，等价于全 NULL（全占位模式）。
 *
 * @return ESP_OK 成功；失败时 UI 不可用（屏幕黑屏，详见日志）
 */
esp_err_t ui_init(const ui_cfg_t *cfg);

/**
 * @brief UI 任务主体：主题/页面构建 + 事件分发 + LVGL 渲染循环
 *
 * 必须在 ui_init() 成功后、由调用方（UI 任务）本任务上下文调用，且不再返回。
 * 之所以不再由 ui_init() 内部建任务：内部 RAM 紧张时，最大的那块任务栈
 * 必须在堆还很完整时先分配（见主音频节点开发过程 十三A）。
 */
void ui_run(void);

/** @brief 反初始化（整机下电路径使用；正常运行不需要调用） */
void ui_deinit(void);

/**
 * @brief UI 首帧就绪回调（启动时序用）
 *
 * 触发时刻：页面构建完成 + 第一屏真正刷完（LVGL LV_EVENT_REFR_READY）之后，
 * 只触发一次；若显示异常拿不到 REFR_READY，ui_run 内部 3s 兜底触发。
 *
 * 用途：App 在这里才放行 SD 卡音乐自动起播 —— 保证"先出界面，再出声"。
 *
 * 上下文：UI_Task，且处于 LVGL 渲染完成事件里。实现必须**非阻塞**
 * （只投递消息/置事件位），禁止在这个回调里做文件 I/O、解码或阻塞等待。
 */
typedef void (*ui_ready_cb_t)(void *ctx);

/**
 * @brief 注册首帧就绪回调（ui_init 之后、ui_run 之前调用；传 NULL 取消）
 */
void ui_set_ready_cb(ui_ready_cb_t cb, void *ctx);

/**
 * @brief 灯控事件接线入口（app_main 创建 LightControl 时把 cfg.on_event 指向本函数）
 *
 * 调用上下文：SmartHome_Task。内部仅入队，由 UI_Task 刷新灯卡片，禁止阻塞。
 */
void ui_on_light_event(const light_event_t *evt, void *user_ctx);

/**
 * @brief 播放事件接线入口（app_main 创建 MusicPlay 时把 cfg.on_event 指向本函数）
 *
 * 调用上下文：MusicPlay_Task。内部仅入队，由 UI_Task 刷新播放页，禁止阻塞。
 */
void ui_on_music_event(const music_event_t *evt, void *user_ctx);

/**
 * @brief OTA 事件接线入口（OTA 回调注册后由 ota_sync_task 上下文调用）
 *
 * 调用上下文：ota_sync_task（优先级 5）。内部仅入队，由 UI_Task 刷新设置页状态，
 * 禁止阻塞。注意：此函数仅负责将事件转发到 UI_Task 消费；停止其他业务任务等
 * 重操作应由注册在 OTA 模块上的上层回调完成（见 scr_settings.c::ota_ui_evt_cb）。
 */
void ui_on_ota_event(const ota_update_event_info_t *info);

#ifdef __cplusplus
}
#endif

#endif /* __UI_H__ */