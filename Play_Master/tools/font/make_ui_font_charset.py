# -*- coding: utf-8 -*-
"""
生成 UI 中文字体子集所需的字符集文件。

范围：
  1. ASCII 可见字符 0x20-0x7E（95 个）
  2. GB2312 全量汉字（一级 3755 + 二级 3008 = 6763 个）
     —— 按 GB2312 双字节编码区间遍历（0xB0-0xF7 / 0xA1-0xFE），
        decode 失败的码位（空位）自动跳过
  3. UI 实际用到的 GB2312 之外符号（箭头/媒体控制/全角空格等）

输出：ui_font_chars.txt（UTF-8，无 BOM，按码位升序，直接给 fontTools.subset
      --text-file 使用）

用法：
  python make_ui_font_charset.py
"""

import os
import sys

# 脚本所在目录 = Play_Master/tools/font
HERE = os.path.dirname(os.path.abspath(__file__))
OUT_PATH = os.path.join(HERE, "ui_font_chars.txt")

# UI 文案里出现、但不属于 GB2312 或需要显式保证的符号
# （·×—…←→、。《》（），：；　 本来就在 GB2312 里，列出来是为了防止
#  将来改动字符集生成逻辑时漏掉；⏭⏮⏸▶ 为媒体控制符号，GB2312 没有）
EXTRA_SYMBOLS = "·×—…←→⏭⏮⏸▶　、。《》（），：；"


def build_charset():
    chars = set()

    # 1. ASCII 可见字符
    for c in range(0x20, 0x7F):
        chars.add(chr(c))

    # 2. GB2312 汉字（含符号区以外的汉字区）
    gb_hanzi = 0
    for b1 in range(0xB0, 0xF8):
        for b2 in range(0xA1, 0xFF):
            try:
                ch = bytes([b1, b2]).decode("gb2312")
            except UnicodeDecodeError:
                continue
            chars.add(ch)
            gb_hanzi += 1

    # 3. 额外符号
    before = len(chars)
    chars |= set(EXTRA_SYMBOLS)
    extra_added = len(chars) - before

    return chars, gb_hanzi, extra_added


def main():
    chars, gb_hanzi, extra_added = build_charset()
    text = "".join(sorted(chars))

    with open(OUT_PATH, "w", encoding="utf-8", newline="") as f:
        f.write(text)

    size = os.path.getsize(OUT_PATH)
    print("output      : %s" % OUT_PATH)
    print("total chars : %d" % len(text))
    print("  ASCII     : 95")
    print("  GB2312汉  : %d" % gb_hanzi)
    print("  extra sym : %d（新增；已含于 GB2312 的重复项不计）" % extra_added)
    print("file bytes  : %d (UTF-8)" % size)
    print("codepoint   : U+%04X .. U+%04X" % (ord(text[0]), ord(text[-1])))

    # 关键覆盖自检：GB2312 一级 3755 字必须 100% 在集合内
    first_level = 0
    missing = 0
    for b1 in range(0xB0, 0xD8):
        for b2 in range(0xA1, 0xFF):
            try:
                ch = bytes([b1, b2]).decode("gb2312")
            except UnicodeDecodeError:
                continue
            first_level += 1
            if ch not in chars:
                missing += 1
    print("GB2312 一级: %d 字，缺失 %d" % (first_level, missing))
    return 0 if missing == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
