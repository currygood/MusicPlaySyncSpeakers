# FreeType 运行时矢量字体：从 TTF 子集到 UI 上屏（完整实现笔记）

> 目标：让 ESP32 上的 LVGL 界面显示**任意中文**，不再受"烘焙字库只有 520 字"的限制。
> 结果：GB2312 全量 6763 字 + ASCII，12/16/20 三档字号真实渲染，固件内无任何编译期中文字库。
>
> 实测报告见 `docs/03-开发/FreeType字体接入-行高适配与内存实测报告.md`，
> 操作手册见 `docs/03-开发/主音频节点中文字体（FreeType运行时字体）.md`。
>
> 环境：ESP-IDF 6.0.1 / LVGL 9.6 / espressif__freetype 2.14.3 / 经典 ESP32 + 8MB PSRAM。

---

## 一、为什么必须换掉烘焙字体

原来的 `ui_font_cn_16.c` 是 `lv_font_conv` 产物，`--symbols` 后面手工列了约 520 个字。
不在列表里的中文（歌名、WiFi SSID、联系人、生僻字）→ LVGL 查不到字形 → **空白/豆腐块**。

三条路：

| 方案 | 结论 |
|---|---|
| 继续烘焙，扩到 6763 字 | 一个字号约 3.5MB flash rodata，且加字就得重新生成 |
| TinyTTF（LVGL 自带的小型 TTF 解析） | 只支持 TTF 子集，无缓存机制，小字号栅格化慢 |
| **FreeType + 运行时 TTF** | ✔ 一份 2 MB 字体覆盖三档字号，加字不用重编固件 |

## 二、整体链路

```
 源字体 NotoSansSC-VF.ttf (17.7 MB, 可变字体 wght 100..900)
        │  ① make_ui_font_charset.py   生成字符集（GB2312 全量 + ASCII + 标点）
        │  ② make_ui_font.py           实例化 wght=400 → fontTools 子集化 → 校验 → 写 meta
        ▼
 storage/font/ui_font.ttf (2.02 MiB, 6876 字形)  +  ui_font.meta
        │  ③ 烧录时随 storage 分区一起进 flash（SPIFFS）
        │  ④ 开机 POSIX open/read 顺序读 → PSRAM（2.02 MB）
        │  ⑤ 注册只读内存 VFS "/fontmem"
        │  ⑥ FreeType 从 /fontmem 打开，创建 12/16/20 三个 lv_font_t
        ▼
 LVGL label 用 ui_font_get(UI_FONT_TEXT_16) 渲染
```

四步里 **④⑤ 是关键**：不能让 FreeType 直接读 SPIFFS（原因见第四节）。

---

## 三、Python 端：字体资产怎么造出来

工具链：`F:\python3.10.1\python`（fontTools 4.65.0 + Pillow 12.3.0）。
**注意**：系统默认的 `python`（3.13）没装 fontTools，必须用这个解释器。

### 3.1 选字体：两轮试错

第一次选的是**阿里汉仪智能黑体**（客户素材，1.73 MB，GB2312 6763 全量）。上板后用户反馈
"有点丑"，放大截图确认 16px 下"播放源"三个字笔画粘连、12px 那行连成一条。

原因不在排版，在**字重**：汉仪是标题级粗黑体，"中"字墨量约 **70.5%**，常规黑体约 **42%**。
而且关键一点——**两款字体都没有 hinting 指令**（0 个字形带指令，只有空 prep 表），
LVGL 又强制 `FT_LOAD_NO_AUTOHINT`，所以**字重是唯一变量**，无法靠 hinting 补救。

用 PIL 把三款字体在同字号下渲染成对比图（黑底白字 3 倍放大）后定案：

| 候选 | 子集大小 | 12px 可读性 | 结论 |
|---|---|---|---|
| 汉仪智能黑体 | 1.47 MiB | 糊成一团 | 弃用（授权文档保留，标注"已停用"） |
| **Noto Sans SC 400（Regular）** | **2.02 MiB** | 清晰 | ✔ 采用 |
| Noto Sans SC 300（Light） | 1.18 MiB（仅一级字） | 偏细，远看发虚 | 备选 |

Noto Sans SC 的好处：OFL-1.1（可商用、可嵌入）、与原来烘焙字体同族（视觉熟悉）、
本机 `C:\Windows\Fonts\NotoSansSC-VF.ttf` 就有。

### 3.2 生成字符集：`make_ui_font_charset.py`

```python
chars = set(chr(c) for c in range(0x20, 0x7F))          # ASCII 可见字符 95 个
for b1 in range(0xB0, 0xF8):                            # GB2312 汉字区
    for b2 in range(0xA1, 0xFF):
        try:
            chars.add(bytes([b1, b2]).decode("gb2312")) # 空位会抛异常，跳过
        except UnicodeDecodeError:
            pass
chars |= set("·×—…←→⏭⏮⏸▶　、。《》（），：；")            # UI 用到的额外符号
open("ui_font_chars.txt", "w", encoding="utf-8").write("".join(sorted(chars)))
```

几个容易记错的点：

- **`0xB0..0xD7` 是一级常用字 3755 个，`0xB0..0xF7` 才是全量 6763**。
  想缩体积就砍到 `0xD8`，代价是二级字（部分人名/地名/生僻字）变 □。
- 用**编码区间遍历 + decode 异常跳过**，不要手写码表——GB2312 有大量空位。
- 输出 **UTF-8 无 BOM、按码位升序**，`fontTools.subset --text-file` 直接吃。
- 脚本末尾做一次自检：一级 3755 字必须 100% 在集合里，缺失返回非 0。

实测产物：**6878 字 / 20,442 字节**（`U+0020 .. U+9FA5`）。

### 3.3 可变字体必须先实例化——最大的一个坑

`NotoSansSC-VF.ttf` 是**可变字体**，`fvar` 的 `wght` 轴范围 100..900，
而**默认值是 100（Thin）**。直接把 VF 丢给子集化，LVGL 会拿到**最细的那一档**，
在小屏上基本看不见。

```python
f = TTFont(src)
instancer.instantiateVariableFont(f, {"wght": 400}, inplace=True, updateFontNames=False)
f.save(tmp)
```

`updateFontNames=False` 是为了不重写 name 表（省事，LVGL 不看字重名）。
脚本里 `--weight` 可调，并会**先读 `fvar` 校验范围**，超范围直接报错。

### 3.4 子集化参数：每个都有理由

```bash
python -m fontTools.subset 源.ttf \
  --text-file=ui_font_chars.txt \
  --output-file=storage/font/ui_font.ttf \
  --layout-features=kern \
  --recommended-glyphs \
  --notdef-outline \
  --drop-tables+=DSIG,BASE
```

| 参数 | 作用 | 不加会怎样 |
|---|---|---|
| `--text-file` | 只保留字符集里的字形 | — |
| `--layout-features=kern` | 只保留字距调整 | 用 `*` 会带上 GSUB/GPOS 一堆排不用不上的表 |
| `--recommended-glyphs` | 补上 `.notdef`、空格等必需字形 | 部分渲染器会异常 |
| `--notdef-outline` | `.notdef` 画成**空心方框** | 缺字变**空白**，看不出是缺字还是没渲染 |
| `--drop-tables+=DSIG,BASE` | 丢数字签名/基线表 | 白占体积 |
| **不用 `--glyph-names`** | — | 会保留完整 `post` 表名字，约 **+70 KB** 纯浪费 |

实测：2,122,276 B，`maxp.numGlyphs = 6876`，cmap 表约 28.6 KB。

### 3.5 校验：要区分"源字体没有"和"子集丢了"

这是第二个坑。第一版校验直接把"字符集里有、产物里没有"全判失败，结果 4 个媒体符号
`⏭⏮⏸▶` 一直报错——查源字体 cmap 才发现**源字体本来就没有这几个码位**，不是子集的问题。

改用"两份 cmap 对照"：

```python
src_cmap = TTFont(源).getBestCmap()      # 源字体有什么
cmap     = TTFont(产物).getBestCmap()    # 子集后有什么
missing_by_subset = [c for c in missing if ord(c) in src_cmap]     # 真 bug → FAIL
missing_src_lack  = [c for c in missing if ord(c) not in src_cmap] # 无能为力 → WARN
```

这样以后**真出子集 bug 不会被噪声掩盖**。当前 `src_missing = U+23ED, U+23EE, U+23F8`
（`▶ U+25B6` 在 Noto 里有），已经写进 meta 备查。

脚本的硬性判据：体积 900 KB..3 MB、`numGlyphs ≥ 6500`、GB2312 **6763/6763**、
ASCII **95/95**、`missing_by_subset` 必须为空。

### 3.6 meta 文件：启动校验的依据

`ui_font.meta` 是 `key=value` 纯文本，固件只读其中 `size=` 一行（其余给人看）：

```text
version=1
source_name=Noto Sans SC / source_weight=400 / source_license=OFL-1.1
size=2122276
sha256=80B9AF11...AE2786
glyphs=6876
charset=GB2312+ASCII
gb2312_cover=6763/6763 / ascii_cover=95/95
line_height_12=14 / line_height_16=19 / line_height_20=24
src_missing=U+23ED,U+23EE,U+23F8
path=/storage/font/ui_font.ttf
```

**改字体后必须同步改 `size=`**，否则启动校验失败 → 直接进英文错误页（这正是设计意图）。

### 3.7 一行命令复现

```bash
cd Play_Master/tools/font
F:/python3.10.1/python make_ui_font_charset.py
F:/python3.10.1/python make_ui_font.py                # 默认 wght=400
F:/python3.10.1/python make_ui_font.py --weight 300   # 换字重
F:/python3.10.1/python make_ui_font.py --verify-only  # 只校验现有产物
```

---

## 四、固件端：把 TTF 变成一个能画的字体

### 4.1 依赖与开关

`main/idf_component.yml` 直接声明（不依赖 LVGL 那个带 `$CONFIG{}` 条件的隐含依赖）：

```yaml
dependencies:
  lvgl/lvgl: "^9"
  espressif/freetype: "^2.14.3"
```

`sdkconfig` 关键项：

| 项 | 值 | 原因 |
|---|---|---|
| `CONFIG_LV_USE_FREETYPE` | `y` | 开 FreeType |
| `CONFIG_LV_FREETYPE_CACHE_FT_GLYPH_CNT` | **256** | **必须是 2 的幂**，否则 L1 缓存编译报错；字形条目 |
| `CONFIG_LV_FREETYPE_USE_LVGL_PORT` | **n** | 见 4.3，我们用标准 stdio 路径 |
| `CONFIG_LV_CACHE_DEF_SIZE` | **131072** | LVGL 自己那层字形缓存；16 KB 时一屏 150 字就满，每帧重新光栅化 |
| `CONFIG_SPIFFS_PAGE_SIZE` | **1024** | 256→1024 让预载从 377 → 578 KB/s（Kconfig 上限就是 1024） |

### 4.2 为什么不能直接把 `/storage/font/ui_font.ttf` 交给 FreeType

第一版就是这么做的，上板直接两个看门狗：

```text
task_wdt（CPU0，UI_Task 占住 >5s）
FT_New_Face → tt_face_load_hmtx → ft_ansi_stream_io → fseek
  → vfs_spiffs_lseek → spiffs_obj_lu_find_id_and_span → esp_partition_read
```

读 SPIFFS 源码才明白：`spiffs_object_read` **只在同一对象索引页内连续读时才复用
`fd->cursor_objix_spix`**；一旦跨索引页就退化成"从 LU 页 0 开始全盘扫描"
（7.25 MB 分区 / 1 KB 页 ≈ 7000 页）。而 FreeType 打开 face 时要在
head / maxp / hhea / hmtx / cmap / glyf 之间反复跳转，**每一次跳转都扫一遍**，
单次 20~25 ms → **一个 face 10 秒以上**。

更糟的是每次 SPIFFS 读都要 `spi_flash_disable_interrupts_caches_and_other_cpu()`，
与另一核的临界区互等 → 蓝牙播放 80s 后 `Interrupt wdt timeout on CPU1`。

**结论：FreeType 绝不能碰 SPIFFS，哪怕只为打开一次。**

### 4.3 解法：PSRAM 预载 + 只读内存 VFS

```c
/* ① 顺序读进 PSRAM（顺序读走 cursor 快路径，不需要全盘扫描） */
uint8_t *buf = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
while (off < sz) {
    read(fd, buf + off, CHUNK);            // CHUNK = 4096
    if ((++chunks % 8) == 0) vTaskDelay(2); // 每 32KB 让出，喂 IDLE0
}

/* ② 注册只读内存 VFS：open/close/read/write/lseek/fstat/fcntl/fsync 九个回调 */
esp_vfs_t s_memfs = { .flags = ESP_VFS_FLAG_DEFAULT, ... };
esp_vfs_register("/fontmem", &s_memfs, NULL);

/* ③ FreeType 走标准 fopen/fread 读内存 */
lv_freetype_font_create("/fontmem/ui_font.ttf",
                        LV_FREETYPE_FONT_RENDER_MODE_BITMAP, 16,
                        LV_FREETYPE_FONT_STYLE_NORMAL);
```

为什么这样最省事：

- **渲染热路径零 flash 访问**，字形光栅化不再有任何阻塞；
- 走标准 stdio，所以**不需要** `LV_FREETYPE_USE_LVGL_PORT=y`，
  也就绕开了"FreeType 的 `ftsystem.c` 和 LVGL 的 `lv_ftsystem.c` 都定义全局
  `FT_Stream_Open` → 重复符号"这个坑；
- 内存 VFS 支持多句柄（`FONT_MEMFD_MAX = 4`），FreeType 想开几个就开几个；
- `MALLOC_CAP_SPIRAM` 分配，**不占内部 DRAM**（内部 DRAM 只剩几十 KB）。

预载耗时：2,122,276 B / 3,674 ms ≈ **578 KB/s**。瓶颈是每 1 KB 一次
`esp_partition_read` 的 cache 关/开，`SPIFFS_PAGE_SIZE` 已经到 Kconfig 上限，没有更多空间。

### 4.4 创建时机：`lv_init()` 之后，页面构建之前

LVGL 9 的 `lv_init()` 内部已经调过 `lv_freetype_init(CONFIG_LV_FREETYPE_CACHE_FT_GLYPH_CNT)`
（`src/lv_init.c`），所以**不要自己再 init 一次**，直接 `lv_freetype_font_create()` 即可。

三个字号建一次、常驻、绝不每帧创建：

```text
12px created in 37217 us: line_height=14 base_line=3   ← 第一个 face 要通读所有表
16px created in   208 us: line_height=19 base_line=4   ← 同一个 face，只改字号
20px created in   304 us: line_height=24 base_line=4
```

所有 LVGL / FreeType 调用**只能在 UI_Task 里**：`CONFIG_LV_USE_OS=0` 时 `lv_mutex` 是空宏，
`FT_Face` 不可重入。

### 4.5 启动校验：轻量，不做全量 CRC

```text
文件存在 → size 与 meta 的 size= 一致 → 头 4 字节是 TTF 魔数
        （0x00010000 / 'true' / 'OTTO' / 'ttcf'）
```

2 MB 全量 CRC 会额外拖慢启动，只在 `UI_FONT_VERIFY_CRC` 调试开关里做。
代价：meta 那个小文件本身要 1.1 s（SPIFFS 为它做一次全盘 LU 扫描，无法避免）。

### 4.6 行高：别信 `ascent+descent`，要看 LVGL 怎么算

LVGL 取的是 `face->size->metrics.height >> 6`（FreeType 按 **hhea** 走），
**不是** PIL/浏览器那种 `ascent + descent`。两者差 2px：

| 字号 | PIL 口径 | **LVGL 实测** |
|---|---|---|
| 12px | 17 | **14** |
| 16px | 19 | **19**（巧合：和原烘焙字体一样，所以 16px 布局一行没改） |
| 20px | 24 | **24** |

Noto Sans SC 的 hhea 是 `asc 1000 / desc 200 / gap 0`，upem 1000——
脚本里的 `line_height = (asc+desc+gap) * px / upem` 与实测**逐像素吻合**，
所以以后换字号可以先算再验。

### 4.7 运行期字体不能当 `LV_FONT_DEFAULT`

`LV_FONT_DEFAULT` 是编译期常量。正确做法：

1. **screen 根对象**设 `text_font`，靠样式继承兜住所有"没显式指定字体"的控件；
2. 两个**不继承**父对象字体的地方要单独设：
   - `lv_textarea` 的 placeholder → `LV_PART_TEXTAREA_PLACEHOLDER`
   - `lv_keyboard` 的按键 → `LV_PART_ITEMS`

否则"请输入 WiFi 密码"和键盘字母还是豆腐块/默认字体。

---

## 五、三个血泪数字

### 5.1 栈：hwm 会说谎，信 SP

| 栈大小 | 现象 |
|---|---|
| 10240 | 首次点设置页 → `stack overflow in UI_Task` |
| 12288 | REFR_READY 采样还有 5980 B 余量，却仍被压穿 |
| 20480 | 启动后 headroom=18100 B，8 秒后点设置页仍然压穿 |
| **32768** | 实测峰值 **23852 B**，余 8.9 KB，2.9 小时无异常 |

`uxTaskGetStackHighWaterMark()` 在 **ESP-IDF 里返回字节**（标准 FreeRTOS 是字），
而且是"历史最小剩余"。它的原理是扫栈底 `0xA5` 填充——**深层函数只要有一大块
"只写一部分"的局部缓冲，扫描就会停在没写过的填充区，报出远大于真实值的余量**。

用 `-fstack-usage` 扫了 1839 个 `.su`，结论反直觉：
**整条 LVGL/FreeType 绘制链的栈帧加起来不到 1.5 KB**
（`lv_obj_redraw 352`、`lv_draw_label 160`、`refr_area 64`），
全固件唯一的大帧是 mp3 解码的 16272 B（在 `MusicPlay_Task`）。
所以那 23.4 KB 不是大数组，是**多层调用叠加 + 渲染期间落进来的 WiFi/BT 中断**
（ESP32 的中断用被打断任务的栈）。

### 5.2 渲染窗口数：绘制开销的放大器

`LV_DISPLAY_RENDER_MODE_PARTIAL` 下，**缓冲高度 = 一次重绘窗口高度**。
原来 8 行 → 240 行要分 **30 段**，每段都要重新遍历整棵对象树、每个相交对象重画一次：
25 个对象 × 30 段 = **750 次绘制**，而像素只有一屏。

| 配置 | 窗口 | flush | draw | 首帧 |
|---|---|---|---|---|
| 8 行 + `-Og` | 30 | 135 ms | 465 ms | 599 ms |
| 16 行 + `-Os` | **15** | 101 ms | 252 ms | **364 ms** |

用两组数据解线性模型：`draw ≈ 14.7 ms × 窗口数 + 0.41 µs × 像素数`
→ 252 ms 里约 **220 ms 是每窗口固定开销**，纯像素只占 31 ms。

### 5.3 窗口数改了，SPI 上限没改 → 屏幕花屏

`SPI_MAX_TRANSFER_SZ` 原值是 `320*8*2+8 = 5128`，注释里就写着"若调大
`UI_FLUSH_ROWS_MAX` 需同步调大这里"。我把窗口从 8 改成 16（10240 B）却忘了改它，
而 `Spi_Next_Chunk()` 在 DMA 直传路径下**不切块**，于是整笔被
`check_trans_valid()` 拒收 → **LCD 一个字节都没写进去**。

表现：只有面积 > 2564 px 的 flush 会失败 → 屏幕"局部还在变、大片冻住"；
更糟的是失败告警走 `LCD_TOUCH_LOGE`，被 `LCD_TOUCH_DEBUG_LOGS=false` 编译成空宏，
**完全静默**，白跑 3 小时。

修法三条一起上：

1. 宏移到头文件并放大到 40 行余量（`320*40*2+8`）；
2. `ui_disp.c` 加 `_Static_assert(LCD_WIDTH*UI_FLUSH_ROWS_MAX*2 <= SPI_MAX_TRANSFER_SZ)`
   —— 不同步就**编译期失败**，不再变成运行时花屏；
3. 错误级日志**永远不受调试开关控制**。

### 5.4 优化级别：`-O2` 会触发编译器 ICE

给 `lvgl__lvgl` 加 `-O2` 编到 `lv_obj_style.c` 报
`during RTL pass: ira → internal compiler error: Segmentation fault`（GCC 15.2 的 bug）。
改成 `-Os` 可以通过，只作用于 **LVGL + FreeType 两个组件**，全局仍是 `-Og`。

坑中坑：`espressif/freetype` 组件本身只是 `idf_component_register()` + INTERFACE 包装，
真正的库是它在子目录 `add_library(freetype ...)` 出来的 **`freetype`** 目标。
遍历 `BUILD_COMPONENTS` 打不到它 → 必须额外 `if(TARGET freetype)` 再补一次。

---

## 六、降级策略：宁可报错，不要静默

| 失败点 | 行为 |
|---|---|
| storage 挂载失败 / 字体缺失 / size 不符 / 魔数错 | 显示 **英文错误页**（`lv_font_montserrat_14`）：错误码 + `ui_font_last_error()` 的具体原因 + 修复提示；**不构建 4 个中文页面**（那时控件全不存在，排空事件队列会打空指针） |
| 单个 label 取字体失败 | `ui_font_text()` 返回内置 ASCII，**永不返回 NULL** |

一个刻意的设计：**回退字体用内置 ASCII，不用中文字库**。
因为一旦走到回退就说明链路坏了，必须让错误页把问题暴露出来，
而不是用"能显示一部分中文"的字库掩盖成"只是几个字变方块"。

配套动作：烘焙中文字体 `ui_font_cn_16.c` 已 `git mv` 到
`tools/font/legacy/ui_font_cn_16.c.bak`（`.c.bak` 不会被 `GLOB *.c` 收进编译），
全仓库零引用。图标字体 `ui_icons_16/24` 保留——它们是 PUA 码位，不是中文。

---

## 七、踩坑清单（按代价排序）

| # | 坑 | 症状 | 解法 |
|---|---|---|---|
| 1 | FreeType 直读 SPIFFS | 一个 face 10 s+，两个核的看门狗 | PSRAM 预载 + 内存 VFS |
| 2 | `UI_FLUSH_ROWS_MAX` 改了没改 `SPI_MAX_TRANSFER_SZ` | 跑几小时后屏幕花屏、`spi_master: txdata transfer > host maximum` | 宏放大 + `_Static_assert` + 错误日志永不静默 |
| 3 | hwm 不可信 | 三次栈溢出，每次"明明还有几 KB" | 读 SP + `.su` 交叉验证，栈 32768 |
| 4 | `NotoSansSC-VF` 默认轴是 100 | 渲染成最细字重，小屏看不清 | `instancer` 显式实例化到 400 |
| 5 | 运行期字体不能当 `LV_FONT_DEFAULT` | 编译不过 | screen 根对象 `text_font` 继承 + textarea/keyboard 单独设 |
| 6 | `LV_FREETYPE_CACHE_FT_GLYPH_CNT` 必须 2 的幂 | L1 缓存编译断言失败 | 128 / 256 / 512 |
| 7 | LVGL 字形缓存太小 | 一屏 150 字就淘汰，每帧重新光栅化 | `LV_CACHE_DEF_SIZE` 16 KB → 128 KB |
| 8 | `file(GLOB_RECURSE)` 无 `CONFIGURE_DEPENDS` | 新增 `ui_font.c` 不进编译 → `undefined reference` | 正常模式加，`cmake -P` 脚本模式不加（分开写 if） |
| 9 | CMake 缓存键名 | 改了默认值却不生效（`UI_LVGL_OPT`、`UI_STACK_USAGE` 各踩一次） | 换新键名，或 `idf.py -DKEY=VALUE build` |
| 10 | `UI_DEBUG_LOGS=false` 让 `UI_LOGx` 成空宏 | 诊断日志一条都没打出来 | 诊断一律用 `ESP_LOGx` |
| 11 | 烧录方式 | 只烧 app → storage 里没字体 → 英文错误页 | **必须 `idf.py flash` 整片烧** |

---

## 八、实测汇总（2.02 MiB Noto / 16 行窗口 / `-Os`）

```text
UIFONT: preload 2122276 B in 3674 ms (578 KB/s), psram free=1686684
UIFONT: validate ok: 2122276 B matches meta
UIFONT: 12px/16px/20px created, line_height=14/19/24
UIFONT: init ok in 6636 ms | internal free=43511 largest=23552 | psram free=1602908
UI: [REFR] page 0 total 363672 us | flush 101009 us / 15 win | draw 252307 us / 14 win
UI: stack[after render]: headroom=8916 B, used<=23852 B, stack=32768 B
```

| 项 | 数值 |
|---|---|
| app bin | 2,588,192 B（4 MB 槽 61.7%） |
| 内部 DRAM | 启动后 43.5 KB 空闲 / 23.5 KB 最大连续块 |
| PSRAM | 字体镜像常驻 2.02 MB，余 1.6 MB |
| 开机字体总耗时 | 6.6 s（预载 3.7 + meta 1.1 + 建 face 0.05） |
| 切页首帧 | 364 ms |

---

## 九、操作手册

**换字体 / 改覆盖范围**

1. 改 `make_ui_font_charset.py` 的遍历区间或 `EXTRA_SYMBOLS` → 重跑字符集脚本；
2. `make_ui_font.py --src=新字体.ttf --weight=N` → 自动实例化、子集化、校验、写 meta；
3. `idf.py build && idf.py flash`（**整片烧**，storage 分区才有新字体）；
4. 看启动日志：`preload / validate ok / line_height` 三项都对才算成功。

**改字号**

改 `ui_font.c` 里的 `s_sizes[]`，并按 `(asc+desc+gap)*px/upem` 重算行高，
把布局常量（如 `SEC_TITLE_H`、`SYNC_ROW_H`）按实测值调整。

**验证降级路径**

```bash
idf.py -p COMx erase-flash    # 连 storage 一起擦
idf.py -p COMx app-flash      # 只烧 app → 应出现英文错误页
idf.py -p COMx flash          # 再整片烧回来
```
