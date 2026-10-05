/**
 * @file ui_theme.c
 * @brief UI 主题：深色配色（宏见 ui_priv.h）+ 字体实例 + 公共样式
 *
 * 字体为 lv_font_conv 生成产物（编译进固件，不依赖 SD 卡）：
 *   - ui_font_cn_16 : 界面中文 16px（覆盖界面全部用字 + ASCII 数字/符号）
 *   - ui_icons_16   : 控件图标 16px（状态栏，Material Icons 子集）
 *   - ui_icons_24   : 控件图标 24px（Tab 栏/大按钮，Material Icons 子集）
 */

#include "ui_priv.h"

void ui_theme_init(void)
{
    /* 配色/字体全部经宏与 extern 引用，无需运行时初始化；
     * 屏幕底色已在 ui_pages_create() 设置。占位注释保留扩展点：
     * 后续接入 SD 卡全量字体时，在此按 SD_Card_Is_Mounted() 切换字体实例。 */
}

void ui_style_card(lv_obj_t *obj)
{
    /* 先剥离默认主题样式再设卡片样式：修复"面板背景不生效/透底"问题，
     * 规避主题样式与局部样式的优先级和时序干扰。
     * 注意：LVGL 9 中 pos/size 也是样式属性，调用方必须在本函数之后再设。 */
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_bg_color(obj, UI_COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, 8, 0);
    lv_obj_set_style_border_color(obj, UI_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t *ui_label_create(lv_obj_t *parent, const char *text,
                          const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    return label;
}
