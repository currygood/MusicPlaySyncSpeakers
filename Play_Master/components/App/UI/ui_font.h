/**
 * @file ui_font.h
 * @brief 运行时中文字体管理（FreeType + Noto Sans SC 400 子集）
 *
 * 设计要点（对应 docs 里的字体接入方案）：
 *   1. 字体源文件放在 flash storage 分区：/storage/font/ui_font.ttf（SPIFFS）
 *   2. FreeType 不能直接读 SPIFFS：SPIFFS 每次跨索引页的 seek 都要全盘扫描对象
 *      索引，实测单次 20~25ms；FreeType 打开 face / 取字模是随机读，会直接把
 *      UI_Task 卡死到触发看门狗，还会因反复关 cache 拖垮 BT 音频。
 *   3. 因此：开机一次性顺序读进 PSRAM（1.5MB），再用一个只读的内存 VFS
 *      （挂载点 /fontmem）把内存镜像暴露成普通文件给 FreeType。
 *      FreeType 走标准 fopen/fread → newlib → VFS，不需要 LV_FREETYPE_USE_LVGL_PORT，
 *      也就不存在 FT_Stream_Open 重复符号问题。
 *   4. 全流程只在 UI_Task 上下文调用（LV_USE_OS=0，FT_Face 不可重入）。
 */

#ifndef __UI_FONT_H__
#define __UI_FONT_H__

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ======================== 字号档位 ========================
 * 12px：状态栏/副标题/说明
 * 16px：正文/按钮/列表/时间
 * 20px：歌曲标题/页面标题
 * 实际 line_height 由字体度量决定（本字体偏大），ui_font_init 会打印实测值。 */
typedef enum {
    UI_FONT_TEXT_12 = 0,
    UI_FONT_TEXT_16,
    UI_FONT_TEXT_20,
    UI_FONT_COUNT
} ui_font_id_t;

/**
 * @brief 初始化字体模块：校验源文件 → 预载到 PSRAM → 注册内存 VFS → 创建三档字体
 * @note  必须在 lv_init() 之后、且只在 UI_Task 上下文调用一次（可重复调用，幂等）
 * @return ESP_OK 成功；其余失败（此时 ui_font_get() 返回 NULL，调用方走降级路径）
 */
esp_err_t ui_font_init(void);

/**
 * @brief 取字体对象
 * @return 成功返回常驻字体指针；未初始化/失败返回 NULL（调用方必须判空降级）
 */
lv_font_t *ui_font_get(ui_font_id_t id);

/**
 * @brief 取字体对象，取不到时回退 LVGL 内置 Montserrat 14（永不返回 NULL）
 *
 * 给页面构造代码用：`ui_label_create(p, "标题", ui_font_text(UI_FONT_TEXT_16), color)`
 * 这样单个控件永远不会因为取不到字体而崩；整体失败由 ui_run() 切到英文错误页兜底。
 *
 * @note 回退字体只有 ASCII，中文会是 □。这是刻意的：固件里不再有任何中文字库，
 *       一旦走到回退就说明字体链路坏了，应当让错误页明确暴露问题，而不是用
 *       某个"看起来还能显示一部分中文"的字库掩盖过去。
 */
const lv_font_t *ui_font_text(ui_font_id_t id);

/**
 * @brief 校验字体源文件（存在 / 大小与 meta 一致 / TTF 魔数正确）
 * @note  不读整份文件做 CRC（1.5MB 会拖慢启动），CRC 只在调试开关下做
 */
esp_err_t ui_font_validate(void);

/** @brief 释放字体与 PSRAM 镜像（UI 退出时调用；正常流程不调用） */
void ui_font_deinit(void);

/** @brief 最近一次失败原因（静态字符串，失败时用于日志/错误页） */
const char *ui_font_last_error(void);

/** @brief 当前字号档位对应的像素值（用于日志/布局断言） */
uint32_t ui_font_size_of(ui_font_id_t id);

#ifdef __cplusplus
}
#endif

#endif /* __UI_FONT_H__ */
