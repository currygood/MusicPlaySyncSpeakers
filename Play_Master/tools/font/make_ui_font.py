# -*- coding: utf-8 -*-
"""
把源 TTF 子集化为 UI 用的 ui_font.ttf，并做覆盖校验 + 生成 ui_font.meta。

源字体：Noto Sans SC（可变字体，wght 100..900），默认取 wght=400（Regular）。
为什么不是汉仪智能黑体：那是标题级粗黑体（"中"字墨量约 70%，常规黑体约 42%），
在 16px 下笔画粘连成一团；且两款都没有 hinting 指令，字重就是唯一变量。
Noto Sans SC 为 OFL 授权，可免费商用、可嵌入固件。

设计约束：
  - 不带 --glyph-names：会保留约 70KB 无用的 post 表
  - 保留 kern（UI 里的数字/英文偶尔用得上），丢弃 BASE/GSUB 等用不上的排版表
  - 必须覆盖 GB2312 全量 6763 + ASCII 95，缺一个都算失败

用法：
  python make_ui_font.py                     # 生成并校验
  python make_ui_font.py --weight 300        # 换字重
  python make_ui_font.py --verify-only       # 只校验已有产物
"""

import argparse
import hashlib
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(os.path.dirname(HERE))          # Play_Master
CHARS_FILE = os.path.join(HERE, "ui_font_chars.txt")
OUT_TTF = os.path.join(PROJ, "storage", "font", "ui_font.ttf")
OUT_META = os.path.join(PROJ, "storage", "font", "ui_font.meta")

DEFAULT_SRC = r"C:\Windows\Fonts\NotoSansSC-VF.ttf"
DEFAULT_WEIGHT = 400

# 合理性区间（不追求精确，只挡住明显跑偏的产物）
SIZE_MIN, SIZE_MAX = 900 * 1024, 3 * 1024 * 1024
GLYPH_MIN = 6500


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest().upper()


def read_charset():
    with open(CHARS_FILE, "r", encoding="utf-8") as f:
        return f.read()


def font_metrics(path):
    """返回 unitsPerEm / hhea 度量 / 各字号的推导行高（与 LVGL 的算法一致：
    font->line_height = face->size->metrics.height >> 6，FreeType 按 hhea 走）。"""
    from fontTools.ttLib import TTFont
    f = TTFont(path, lazy=True)
    upem = f["head"].unitsPerEm
    hhea = f["hhea"]
    asc, desc, gap = hhea.ascender, -hhea.descender, hhea.lineGap
    out = {"upem": upem, "asc": asc, "desc": desc, "gap": gap}
    for px in (12, 16, 20):
        out["lh%d" % px] = int((asc + desc + gap) * px / upem)
    return out


def prepare_source(src, weight):
    """可变字体先实例化到指定字重，返回 (实际用于子集的文件, 源信息dict)。"""
    from fontTools.ttLib import TTFont

    if not os.path.isfile(src):
        raise SystemExit("[FAIL] 源字体不存在: %s" % src)

    src_size = os.path.getsize(src)
    src_sha = sha256_of(src)
    info = {"path": src, "size": src_size, "sha256": src_sha, "weight": None, "family": ""}

    f = TTFont(src, lazy=True)
    try:
        for rec in f["name"].names:
            if rec.nameID == 16:            # typographic family
                info["family"] = rec.toUnicode()
                break
        if not info["family"]:
            for rec in f["name"].names:
                if rec.nameID == 1:
                    info["family"] = rec.toUnicode()
                    break
    except Exception:
        pass

    axes = {}
    if "fvar" in f:
        axes = {a.axisTag: (a.minValue, a.defaultValue, a.maxValue) for a in f["fvar"].axes}
    info["axes"] = axes
    f.close()

    if "wght" not in axes:
        print("[info] 静态字体，无需实例化")
        return src, info

    lo, default, hi = axes["wght"]
    if not (lo <= weight <= hi):
        raise SystemExit("[FAIL] 字重 %s 超出可变轴范围 %.0f..%.0f" % (weight, lo, hi))

    from fontTools.varLib import instancer
    print("[run ] 实例化 wght=%d（轴范围 %.0f..%.0f，默认 %.0f）" % (weight, lo, hi, default))
    f = TTFont(src)
    instancer.instantiateVariableFont(f, {"wght": weight}, inplace=True, updateFontNames=False)
    tmp = os.path.join(tempfile.gettempdir(), "ui_font_w%d.ttf" % weight)
    f.save(tmp)
    f.close()
    info["weight"] = weight
    print("[ OK ] 实例化完成: %s (%d B)" % (tmp, os.path.getsize(tmp)))
    return tmp, info


def run_subset(src):
    cmd = [
        sys.executable, "-m", "fontTools.subset", src,
        "--text-file=" + CHARS_FILE,
        "--output-file=" + OUT_TTF,
        "--layout-features=kern",
        "--recommended-glyphs",
        "--notdef-outline",
        "--drop-tables+=DSIG,BASE",
    ]
    print("[run ] " + " ".join(cmd[1:]))
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print("[FAIL] subset 失败 (rc=%d)" % r.returncode)
        print(r.stdout[-2000:])
        print(r.stderr[-2000:])
        return False
    if r.stderr.strip():
        print("[warn] " + r.stderr.strip()[:500])
    return True


def verify(src_path):
    from fontTools.ttLib import TTFont
    from fontTools.ttLib.tables._c_m_a_p import table__c_m_a_p

    if not os.path.isfile(OUT_TTF):
        print("[FAIL] 产物不存在: %s" % OUT_TTF)
        return None

    # 源字体 cmap：用于区分「源字体本来就没有」与「子集化丢失」
    src_cmap = {}
    if os.path.isfile(src_path):
        src_cmap = TTFont(src_path, lazy=True).getBestCmap()

    size = os.path.getsize(OUT_TTF)
    digest = sha256_of(OUT_TTF)
    font = TTFont(OUT_TTF, lazy=True)

    num_glyphs = font["maxp"].numGlyphs
    cmap = font.getBestCmap()

    try:
        tmp = table__c_m_a_p()
        tmp.tableVersion = font["cmap"].tableVersion
        tmp.tables = font["cmap"].tables
        cmap_bytes = len(tmp.compile(font))
    except Exception:
        cmap_bytes = -1

    charset = read_charset()
    missing = [c for c in charset if ord(c) not in cmap]
    missing_src_lack = [c for c in missing if ord(c) not in src_cmap]
    missing_by_subset = [c for c in missing if ord(c) in src_cmap]

    ascii_total, ascii_hit = 95, 0
    for c in range(0x20, 0x7F):
        if c in cmap:
            ascii_hit += 1

    gb_total, gb_hit = 0, 0
    for b1 in range(0xB0, 0xF8):
        for b2 in range(0xA1, 0xFF):
            try:
                ch = bytes([b1, b2]).decode("gb2312")
            except UnicodeDecodeError:
                continue
            gb_total += 1
            if ord(ch) in cmap:
                gb_hit += 1

    name = ""
    try:
        for rec in font["name"].names:
            if rec.nameID == 4:
                name = rec.toUnicode()
                break
    except Exception:
        pass

    m = font_metrics(OUT_TTF)

    print("")
    print("========== 产物校验 ==========")
    print("文件        : %s" % OUT_TTF)
    print("大小        : %d B (%.3f MiB)" % (size, size / 1048576.0))
    print("SHA256      : %s" % digest)
    print("字体名      : %s" % (name or "(未取到)"))
    print("numGlyphs   : %d" % num_glyphs)
    print("cmap 条目   : %d，表大小 %d B" % (len(cmap), cmap_bytes))
    print("ASCII 覆盖  : %d/%d" % (ascii_hit, ascii_total))
    print("GB2312 覆盖 : %d/%d" % (gb_hit, gb_total))
    print("行高推导    : 12px→%d  16px→%d  20px→%d （hhea %d/%d/%d, upem %d）"
          % (m["lh12"], m["lh16"], m["lh20"], m["asc"], m["desc"], m["gap"], m["upem"]))
    print("字符集缺失  : %d" % len(missing))
    if missing_src_lack:
        print("  源字体无   : %d  %s" % (
            len(missing_src_lack),
            " ".join("U+%04X" % ord(c) for c in missing_src_lack)))
    if missing_by_subset:
        print("  子集丢字   : %d  %s" % (
            len(missing_by_subset),
            " ".join("U+%04X" % ord(c) for c in missing_by_subset)))

    ok = True
    if not (SIZE_MIN <= size <= SIZE_MAX):
        print("[FAIL] 体积 %.1f KB 超出合理区间 %d..%d KB"
              % (size / 1024.0, SIZE_MIN // 1024, SIZE_MAX // 1024))
        ok = False
    if num_glyphs < GLYPH_MIN:
        print("[FAIL] glyph 数 %d < %d" % (num_glyphs, GLYPH_MIN))
        ok = False
    if gb_hit != gb_total:
        print("[FAIL] GB2312 未全量覆盖")
        ok = False
    if ascii_hit != ascii_total:
        print("[FAIL] ASCII 未全量覆盖")
        ok = False
    if missing_by_subset:
        print("[FAIL] 子集化丢了源字体里存在的字")
        ok = False
    if missing_src_lack:
        print("[warn] 上列字符源字体不含，无法渲染（非子集缺陷）——"
              "UI 不得用中文 label 显示这些符号，图标请用 ui_icons")
    print("========== %s ==========" % ("通过" if ok else "不通过"))

    return {
        "size": size, "sha256": digest, "glyphs": num_glyphs,
        "cmap_bytes": cmap_bytes, "gb": gb_hit, "ascii": ascii_hit,
        "missing_src_lack": missing_src_lack, "metrics": m, "ok": ok,
    }


def write_meta(info, srcinfo):
    m = info["metrics"]
    lines = [
        "# UI 中文字体资产元信息（ui_font.c 启动校验依据）",
        "# 生成脚本: tools/font/make_ui_font.py",
        "version=1",
        "source_path=%s" % srcinfo["path"].replace("\\", "/"),
        "source_sha256=%s" % srcinfo["sha256"],
        "source_size=%d" % srcinfo["size"],
        "source_name=%s" % (srcinfo.get("family") or "unknown"),
        "source_weight=%s" % (srcinfo.get("weight") if srcinfo.get("weight") else "static"),
        "source_license=OFL-1.1",
        "size=%d" % info["size"],
        "sha256=%s" % info["sha256"],
        "glyphs=%d" % info["glyphs"],
        "charset=GB2312+ASCII",
        "gb2312_cover=%d/6763" % info["gb"],
        "ascii_cover=%d/95" % info["ascii"],
        "line_height_12=%d" % m["lh12"],
        "line_height_16=%d" % m["lh16"],
        "line_height_20=%d" % m["lh20"],
        "src_missing=%s" % (",".join("U+%04X" % ord(c) for c in info["missing_src_lack"]) or "none"),
        "path=/storage/font/ui_font.ttf",
    ]
    with open(OUT_META, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print("[ OK ] meta 写入: %s" % OUT_META)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--verify-only", action="store_true")
    ap.add_argument("--src", default=os.environ.get("UI_FONT_SRC", DEFAULT_SRC))
    ap.add_argument("--weight", type=int, default=DEFAULT_WEIGHT)
    args = ap.parse_args()

    os.makedirs(os.path.dirname(OUT_TTF), exist_ok=True)

    srcinfo = {"path": args.src, "size": 0, "sha256": "", "family": "", "weight": None}
    subset_src = args.src

    if not args.verify_only:
        if not os.path.isfile(CHARS_FILE):
            print("[FAIL] 字符集文件不存在，请先运行 make_ui_font_charset.py")
            return 2
        subset_src, srcinfo = prepare_source(args.src, args.weight)
        if not run_subset(subset_src):
            return 3
    else:
        srcinfo = prepare_source(args.src, args.weight)[1] if os.path.isfile(args.src) else srcinfo

    info = verify(subset_src)
    if info is None:
        return 4
    if not info["ok"]:
        return 5

    write_meta(info, srcinfo)
    return 0


if __name__ == "__main__":
    sys.exit(main())
