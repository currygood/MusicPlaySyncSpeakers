# Noto Sans SC 授权说明

> 本文档记录 UI 运行时中文字体（`Play_Master/storage/font/ui_font.ttf`）的来源、派生关系与授权状态。
> 结论先行：**OFL-1.1（SIL Open Font License 1.1），允许免费商用、允许嵌入与再分发，
> 允许子集化/修改后随产品分发**；唯一要求是保留版权声明与许可文本，且修改版不得使用保留字体名（Reserved Font Name）。
> 本项目使用的是"子集化 + 固定字重实例"，属于 OFL 允许的派生行为，派生文件已改名 `ui_font.ttf`，不占用保留名。

## 1. 源字体信息

| 项 | 值 |
|---|---|
| 字体名 | Noto Sans SC |
| 源文件（本机） | `C:\Windows\Fonts\NotoSansSC-VF.ttf` |
| 文件大小 | 17,773,244 B |
| SHA256 | `763146584CF0710223441356B4395E279021B0806C196614377A7A0174AE074A` |
| 形态 | 可变字体（`fvar`：wght 100..900，默认 100 = Thin） |
| unitsPerEm | 1000 |
| 字形数（源） | 31,036 |
| 授权 | SIL Open Font License 1.1（OFL-1.1） |
| 上游 | Google Noto / Adobe Source Han Sans 同源设计（CJK 合并字形） |

## 2. 本项目的派生链

```
NotoSansSC-VF.ttf (17,773,244 B, wght 100..900)
  │  ① fontTools.varLib.instancer 固定 wght=400（Regular）
  ▼
ui_font_w400.ttf (10,596,368 B, 静态单字重)
  │  ② fontTools.subset：字符集 = GB2312 全量 6763 + ASCII 95 + 常用标点，
  │     保留 kern，丢弃 DSIG/BASE 等无用表，保留 .notdef 轮廓
  ▼
storage/font/ui_font.ttf (2,122,276 B, 6876 字形)
```

生成命令（可复现）：

```bash
F:\python3.10.1\python Play_Master\tools\font\make_ui_font.py            # 默认 wght=400
F:\python3.10.1\python Play_Master\tools\font\make_ui_font.py --weight 300  # 换细体
```

## 3. 产物规格

| 项 | 值 |
|---|---|
| 文件 | `Play_Master/storage/font/ui_font.ttf` |
| 大小 | 2,122,276 B（2.02 MiB） |
| SHA256 | `80B9AF11B6ECBAC13B282BF7EC4427CB3A6776320AB1312FD6D4CF2AE4AE2786` |
| 字形数 | 6,876 |
| cmap 表 | 28,700 B，6,875 条目 |
| 覆盖 | GB2312 **6763/6763**、ASCII **95/95** |
| 行高（LVGL `metrics.height`） | 12px→14、16px→19、20px→24 |
| 运行时路径 | `/storage/font/ui_font.ttf`（SPIFFS），开机整块预载进 PSRAM 后由内存 VFS 供给 FreeType |

字符集里只有 3 个符号源字体不含（`U+23ED ⏭`、`U+23EE ⏮`、`U+23F8 ⏸`），
UI 中这些符号一律走图标字体 `ui_icons_16/24` 的 PUA 码位，不使用文本字符。

## 4. 为什么从「阿里汉仪智能黑体」换过来

| | 阿里汉仪智能黑体 | Noto Sans SC 400 |
|---|---|---|
| 设计定位 | 标题级粗黑体 | 正文黑体 |
| "中"字墨量填充率 | 70.5% | ≈42% |
| 16px 实机观感 | 笔画粘连成一团，12px 几乎不可读 | 清晰 |
| hinting 指令 | 0 个字形 | 0 个字形（两者相同，LVGL 又强制 `FT_LOAD_NO_AUTOHINT`，字重是唯一变量） |
| 商用授权 | 需按官方条款确认（原结论"待确认"） | OFL-1.1，明确可商用 |

实机对比与决策过程见 git 提交记录；汉仪版授权说明保留在同目录 `阿里汉仪智能黑体-授权说明.md`（已标注停用）。

## 5. 合规要求

随固件分发时需保留 OFL 文本与版权声明。建议在产品的"关于/开源许可"页或随附文档中列出：

```
Noto Sans SC (subset, wght=400)
Copyright 2014-2021 Adobe (http://www.adobe.com/), with Reserved Font Name 'Source'.
Copyright Google Inc.
Licensed under the SIL Open Font License, Version 1.1.
https://scripts.sil.org/OFL
```

> 注：Noto Sans SC 与 Source Han Sans 共享字形与版权头，字体内部 `name` 表已包含上述版权与许可声明，
> 子集化时未删除 `name` 表，因此字体文件本身即携带合规信息。
