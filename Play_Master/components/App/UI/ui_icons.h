/**
 * @file ui_icons.h
 * @brief 控件图标字形宏（Material Icons 子集，UTF-8 编码字符串）
 *
 * 码位来源：google/material-design-icons 官方 codepoints 表（逐项核对，非猜测）；
 * 配合 ui_icons_16.c / ui_icons_24.c 生成的图标字体用于 lv_label 显示。
 * 宏值为 U+E0xx 码位对应的 3 字节 UTF-8 转义（由脚本按码位计算）。
 */

#ifndef __UI_ICONS_H__
#define __UI_ICONS_H__

#define UI_ICON_BACK       "\xee\x97\x84"   /* U+E5C4 */
#define UI_ICON_BT         "\xee\x86\xa7"   /* U+E1A7 */
#define UI_ICON_BULB       "\xee\x83\xb0"   /* U+E0F0 */
#define UI_ICON_LIST       "\xee\xa2\x96"   /* U+E896 */
#define UI_ICON_MIC        "\xee\x80\xa9"   /* U+E029 */
#define UI_ICON_MUSIC      "\xee\x90\x85"   /* U+E405 */
#define UI_ICON_PAUSE      "\xee\x80\xb4"   /* U+E034 */
#define UI_ICON_PLAY       "\xee\x80\xb7"   /* U+E037 */
#define UI_ICON_REPEAT     "\xee\x81\x80"   /* U+E040 */
#define UI_ICON_REPEAT_ONE "\xee\x81\x81"   /* U+E041 */
#define UI_ICON_SETTINGS   "\xee\xa2\xb8"   /* U+E8B8 */
#define UI_ICON_NEXT       "\xee\x81\x84"   /* U+E044 */
#define UI_ICON_PREV       "\xee\x81\x85"   /* U+E045 */
#define UI_ICON_SYNC       "\xee\x98\xa7"   /* U+E627 */
#define UI_ICON_VOLUME     "\xee\x81\x90"   /* U+E050 */
#define UI_ICON_WIFI       "\xee\x98\xbe"   /* U+E63E */
#define UI_ICON_WIFI_OFF   "\xee\x99\x88"   /* U+E648 */

#endif /* __UI_ICONS_H__ */
