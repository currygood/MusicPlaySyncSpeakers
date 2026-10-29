# 主音频节点中文字体（FreeType 运行时字体）

> 适用工程：`Play_Master`（主音频节点，ESP32-WROVER-E / 16MB Flash / 8MB PSRAM）
> 相关代码：`components/App/UI/ui_font.c` / `ui_font.h`
> 相关资产：`Play_Master/storage/font/ui_font.ttf` + `ui_font.meta`
> 授权说明：`docs/07-许可/所用开源/NotoSansSC-授权说明.md`

---

## 1. 为什么不用编译期字库

早期界面中文是 `lv_font_conv` 生成的 `ui_font_cn_16.c`，只覆盖约 520 个手工挑选的字。
歌名、WiFi SSID、联系人、生僻字一旦超出这个集合就显示为空/豆腐块。

现在改为：**把一份 GB2312 全量 TTF 放进 flash 分区，开机用 FreeType 运行时渲染**。
固件里不再编入任何中文字库（原 `ui_font_cn_16.c` 已移到
`Play_Master/tools/font/legacy/ui_font_cn_16.c.bak`，仅作历史备查、不参与编译）。

图标字体 `ui_icons_16.c` / `ui_icons_24.c`（Material Icons PUA 码位）**保留不动**，
它们是图标字体，不是中文。

---

## 2. 字体资产

| 项 | 值 |
|---|---|
| 文件 | `Play_Master/storage/font/ui_font.ttf` |
| 大小 | 2,122,276 B（2.02 MiB） |
| SHA256 | `80B9AF11...AE2786`（完整值见 `ui_font.meta`） |
| 字形数 | 6,876 |
| 覆盖 | GB2312 **6763/6763**、ASCII **95/95**、常用中文标点 |
| 源字体 | `C:\Windows\Fonts\NotoSansSC-VF.ttf`（可变字体，实例化到 wght=400） |
| 授权 | SIL OFL-1.1，可商用、可嵌入 |

`ui_font.meta` 是启动校验依据，字段：

```
version=1
source_sha256=80B9AF11...
size=2122276
glyphs=6876
gb2312_cover=6763/6763
ascii_cover=95/95
path=/storage/font/ui_font.ttf
```

> **改动 `ui_font.ttf` 后必须同步更新 `size=`**，否则启动校验会失败并进入英文错误页。

---

## 3. 生成 / 升级字体

工具目录：`Play_Master/tools/font/`

```bash
# 1) 生成字符集（GB2312 全量 + ASCII + 标点）→ ui_font_chars.txt
python make_ui_font_charset.py

# 2) 子集化 + 校验 + 写 meta → ../storage/font/ui_font.ttf
python make_ui_font.py                 # 默认 wght=400
python make_ui_font.py --weight 300    # 想更细
python make_ui_font.py --verify-only   # 只校验现有产物
```

**本机注意**：`fontTools` 只装在 `F:\python3.10.1\python`，默认的 `python`（3.13）没有，
必须用这个解释器执行。

### 3.1 必须知道的坑：可变字体默认轴

`NotoSansSC-VF.ttf` 的 `fvar` 默认值是 **wght=100（Thin）**。不显式实例化就直接子集化，
LVGL 拿到的会是最细的一档，小字号直接糊掉。脚本已内置 `instancer.instantiateVariableFont`
到 400，改字重请用 `--weight`，不要手工跑 `fontTools.subset`。

### 3.2 子集参数

```
--layout-features=kern --recommended-glyphs --notdef-outline --drop-tables+=DSIG
```

**不要用 `--glyph-names`**，会保留约 70KB 无用的 `post` 表。

### 3.3 校验口径

`make_ui_font.py` 会区分两类缺失，避免真正的子集 bug 被噪声掩盖：

- **源字体本身没有该码位** → 只警告（例如 `⏭⏮⏸▶`，源码字体就没有，UI 里也用图标字体代替）
- **源字体有、子集丢了** → FAIL

---

## 4. 启动流程

```
ui_run()  (UI_Task)
  └─ ui_font_init()
       ├─ 1. 校验 /storage/font/ui_font.ttf：存在 + 大小与 meta 一致 + 前 4 字节 TTF 魔数
       │      （不做全量 CRC，1.5MB 会拖慢启动；CRC 只在调试开关下做）
       ├─ 2. 顺序读整份文件 → PSRAM（heap_caps_malloc(MALLOC_CAP_SPIRAM)）
       ├─ 3. 注册只读内存 VFS：/fontmem/ui_font.ttf
       ├─ 4. lv_freetype_font_create() × 3：12px / 16px / 20px，常驻不销毁
       └─ 5. 打印实测 line_height / base_line / 耗时 / 堆余量
```

### 4.1 为什么必须预载到 PSRAM，不能让 FreeType 直接读 SPIFFS

这是本项目踩得最深的一个坑。SPIFFS 的 `spiffs_object_read` 只有在**同一对象索引页内
连续读**时才复用游标；一旦跨页就退化成从 LU 页 0 开始全盘扫描（7.25MB / 256B ≈ 116 页）。
FreeType 打开 face 时要在 head/maxp/hhea/hmtx/cmap/glyf 之间反复跳，**每次跳转都扫一遍**，
单次 20~25ms → 一个 face 10 秒以上，直接触发 task_wdt；同时每次 flash 读都会
`spi_flash_disable_interrupts_caches_and_other_cpu()`，把另一核的 BT 音频也拖死。

预载走的是**顺序读**（游标快路径），每 32KB `vTaskDelay(1)` 让出 CPU，不会触发看门狗。

### 4.2 为什么走内存 VFS 而不是 `LV_FREETYPE_USE_LVGL_PORT`

内存 VFS 让 FreeType 走标准 `fopen/fread`（newlib → VFS），因此
`CONFIG_LV_FREETYPE_USE_LVGL_PORT=n` 即可，也就绕开了
`FT_Stream_Open` 在 `lv_ftsystem.c` 与 FreeType `ftsystem.c` 之间的重复符号问题。

渲染热路径完全不碰 flash，也不会因 SPIFFS 随机读而卡音频。

### 4.3 关键配置（`sdkconfig`）

```
CONFIG_LV_USE_FREETYPE=y
CONFIG_LV_FREETYPE_USE_LVGL_PORT=n        # 走真实 VFS 路径
CONFIG_LV_FREETYPE_CACHE_FT_GLYPH_CNT=256 # 必须是 2 的幂，否则编译报错
CONFIG_LV_FREETYPE_CACHE_FT_GLYPH_L1=y
CONFIG_LV_CACHE_DEF_SIZE=131072           # 默认 16KB 太小，字形反复重光栅化
CONFIG_SPIFFS_PAGE_SIZE=1024              # 256 → 1024，预载快 4 倍
```

> `CONFIG_SPIFFS_PAGE_SIZE` 改了以后 **storage 分区格式变了，必须整片烧录**
> （`idf.py flash`），只烧 app 会导致挂载失败 → 直接进英文错误页。

---

## 5. 降级策略（Phase 5）

原则：**不偷偷用任何中文字库兜底。** 固件里已经没有中文字库，硬撑只会满屏 □，
比明确报错更难查。

`ui_font_init()` 任一环失败（storage 未挂载 / 文件缺失 / 尺寸或魔数不符 / PSRAM 不足 /
FreeType 创建失败）时：

1. `ui_run()` **不构建 4 个中文页面**；
2. 用 LVGL 内置 `lv_font_montserrat_14`（纯 ASCII）渲染一屏 **英文错误页**，
   显示错误码 + 具体原因（如 `unexpected size 1234`、`bad ttf magic 0000`）；
3. 打印一次 `E UI: fatal: ...`；
4. 之后只跑 `lv_timer_handler()`，不再排空事件队列（此时控件都不存在）。

页面代码通过 `ui_font_text(id)` 取字体，取不到时回退 `lv_font_montserrat_14`（永不返回 NULL），
这样单个控件不会因为取空而崩。正常流程下不会走到这条路径——init 失败就已经切错误页了。

**storage 挂载失败**只在 `ui_hw_bringup()` 里打一次警告，不刷屏。

---

## 6. 实测数据

| 项 | 值 | 说明 |
|---|---|---|
| 字体文件 | 2,122,276 B | 占 storage 分区（7.25MB）约 29% |
| PSRAM 占用 | ~2.1MB | 预载镜像；开机后 psram free 约 1.5MB |
| 预载耗时 | 5.6s（256B 页）→ 预计 ~1.5s（1024B 页） | 瓶颈是 `esp_partition_read` 每页一次关/开 cache |
| 12/16/20px 创建 | 首次 60ms 左右，之后 200~500µs | 首次含 FreeType 初始化 |
| 行高（hhea 1000/-200/0，upem 1000） | 12px=**14**、16px=**19**、20px=**24** | 以启动日志 `UIFONT: xxpx created ... line_height=` 为准 |
| UI_Task 栈 | **32768 B** | 实测单帧峰值 23852 B，曾按 12288/20480/24576 三次压穿栈底 |

> 行高一定要看实测日志。之前用汉仪字体时是 12px=16 / 16px=21 / 20px=27；
> 而 v9 之前的文档里写的 17/23/28 是 PIL 的 `ascent+descent` 口径，**不是** LVGL 的口径
> （LVGL 用 `face->size->metrics.height >> 6`）。两者差 2px，照 PIL 的值排布局会出问题。

---

## 7. 坑清单（都实际踩过）

| # | 坑 | 现象 | 处理 |
|---|---|---|---|
| 1 | FreeType 直读 SPIFFS | `task_wdt`，UI_Task 卡 10s+；BT 播放 80s 后 `Interrupt wdt timeout` | 预载 PSRAM + 内存 VFS |
| 2 | IDF 的 `uxTaskGetStackHighWaterMark()` 返回**字节**（标准 FreeRTOS 是字） | 把"剩余"当"已用"，判定全错 | 日志里明确区分 headroom / used |
| 3 | ESP32 中断使用**被打断任务的栈** | 采样余 5980B，下一个 WiFi 中断就把 canary 压穿 | 栈按"峰值 + 8KB"留，宁大勿小 |
| 4 | `UI_FLUSH_ROWS_MAX` 与 `SPI_MAX_TRANSFER_SZ` 耦合 | 行数调大后 `E spi_master: txdata transfer > host maximum` 刷屏，**屏幕大片不更新、程序照跑** | 大小值放在 `spi_driver.h`，`ui_disp.c` 用 `_Static_assert` 交叉校验 |
| 5 | `LCD_TOUCH_LOGE` 受 `LCD_TOUCH_DEBUG_LOGS` 控制 | 写屏失败的唯一告警被编译掉，问题完全静默 | 错误级日志改为**永远编译** |
| 6 | GCC 15.2 + `-O2` 编 lvgl | `lv_obj_style_state_compare()` 在 RTL pass `ira` 段错误（编译器 ICE） | 用 `-Os`（`UI_UI_DRAW_OPT`），`-O2` 会自动附 `-fjump-tables` 再试 |
| 7 | CMake `add_compile_options()` 写在 `project()` 之后 | 对 IDF 组件目标完全无效（`.su` 一个都没产出） | 遍历 `BUILD_COMPONENTS` 对 `COMPONENT_LIB` 追加 |
| 8 | 缓存键名改了默认值不生效 | 明明改了默认值，构建日志还是旧值 | 换新键名（`UI_LVGL_OPT` → `UI_UI_DRAW_OPT`） |
| 9 | `CONFIG_LV_CACHE_DEF_SIZE=16KB` | 16px 字形约 300B/个，一屏 150 字就满，每帧重新光栅化 | 提到 128KB |
| 10 | `lv_refr_now()` 强制同步刷屏 | 整屏 744ms 不让 CPU，同核的 `wifi_mcast_rx` 栈被顶穿 | 用 `LV_EVENT_REFR_READY` 统计，零阻塞 |

---

## 8. 待观察 / 未完成

- **单次整屏绘制 ~440ms（-Og 下 30 个渲染窗口）**：已把渲染窗口从 8 行提到 16 行
  （30 段 → 15 段）、LVGL/FreeType 改 `-Os`，实际效果待 `[REFR]` 日志确认。
  横向调窗口要同时看 `SPI_MAX_TRANSFER_SZ`（已按 40 行预留）。
- **蓝牙播放偶发 `A2DP starved ~80ms` 与 `Sequence numbers error`**：UI 停手后仍出现，
  疑似 WiFi/BT 共存或链路进 sniff，与字体链路无直接关系，需单独排查。
- **不做的**：不做中文分词、多语言、字重系统、emoji 字体；不动分区表尺寸和 OTA 流程。
