# FreeType 字体接入、行高适配与内存实测报告

> 对象：`Play_Master`（ESP32-WROVER-E，16MB Flash / 8MB PSRAM，ILI9341 320×240 SPI）
> 时间：2026-10-27 ~ 2026-10-29
> 结论：**已完成**。界面中文全部走运行时 TTF + FreeType，覆盖 GB2312 全量 6763 字，
> 固件内不再有任何烘焙中文字库。

---

## 1. 一句话总结

把「编译期烘焙的 520 字子集字体」换成「flash 分区里的 2.02 MiB TTF，开机预载进 PSRAM，
由 FreeType 运行时渲染」。字体链路本身只花了 3 天里的第一晚，剩下两天全花在三个
**没人预料到的地方**：SPIFFS 随机读会把 UI 任务卡到看门狗、渲染窗口数把绘制开销
放大 30 倍、以及栈用量与 hwm 读数完全不一致。

| 项目 | 结果 |
|---|---|
| 中文覆盖 | GB2312 **6763/6763** + ASCII 95/95 |
| 三档字号 | 12px（行高 14）/ 16px（19）/ 20px（24），均为真实渲染，无缩放 |
| app bin | 2,588,192 B（4MB 槽 **61.7%**），比接入前 +246 KB |
| 内部 DRAM | 启动后 43.5 KB 空闲 / 23.5 KB 最大连续块 |
| PSRAM | 字体镜像常驻 2.12 MB，余 1.60 MB |
| UI 任务栈 | 32768 B（实测渲染峰值 23852 B，余 8.9 KB） |
| 切页首帧 | **364 ms**（8 行窗口 + `-Og` 时为 599 ms，改善 39%） |
| 音频 | 不碰屏播放 3 分钟无杂音；滚屏/切页期间的 starve 告警显著减少 |
| 降级 | 字体缺失/损坏 → 英文错误页，不黑屏、不崩 |

---

## 2. 资产与加载链路

### 2.1 资产

| 文件 | 大小 | 说明 |
|---|---|---|
| `storage/font/ui_font.ttf` | 2,122,276 B | Noto Sans SC Regular（wght=400）子集 |
| `storage/font/ui_font.meta` | 610 B | 启动校验依据（size / sha256 / 覆盖率） |
| `tools/font/make_ui_font.py` | — | 实例化 + 子集化 + 校验 + 写 meta，一条命令复现 |
| `tools/font/ui_font_chars.txt` | 20 KB | GB2312 全量 + ASCII + 标点，6,876 字 |

字体为什么从「阿里汉仪智能黑体」换成 Noto Sans SC：汉仪是标题级粗黑体
（"中"字墨量 70.5%，常规黑体约 42%），16px 下笔画粘连、12px 几乎不可读。
两款字体**都没有 hinting 指令**，而 LVGL 强制 `FT_LOAD_NO_AUTOHINT`，
所以字重是唯一变量。授权说明见 `docs/07-许可/所用开源/`。

### 2.2 为什么不能直接把 `/storage/font/ui_font.ttf` 交给 FreeType

这是整个任务最有价值的一条经验。首次实现就是直接传 VFS 路径，结果：

- `FT_New_Face` 在 head / maxp / hhea / hmtx / cmap / glyf 之间反复跳转
- 每次跳转都触发 `spiffs_obj_lu_find_id_and_span()` 全盘扫描 LU 页
  （7.25 MB 分区 / 1 KB 页 ≈ 7000 页）
- 单次跳转 20~25 ms → **一个 face 超过 10 秒**
- UI 任务在 CPU0 prio 10 连续不让 CPU → IDLE0 饿死 → `task_wdt`
- 每次 SPIFFS 读都 `spi_flash_disable_interrupts_caches_and_other_cpu()`，
  与另一核的临界区互等 → 蓝牙播放 80s 后 `Interrupt wdt timeout on CPU1`

**现在的做法**：开机用 POSIX `open/read` **顺序**把整个 TTF 读进 PSRAM
（顺序读走 SPIFFS 的 cursor 快路径，不需要全盘扫描），再注册一个只读的**内存 VFS**
`/fontmem` 交给 FreeType。渲染热路径**零 flash 访问**。

因为走标准 stdio，还顺带绕开了 `LV_FREETYPE_USE_LVGL_PORT=y` 时
`FT_Stream_Open` 重复定义的坑（任务书第 10.1 条）。

```
[storage 分区] ui_font.ttf
      │ POSIX open/read 顺序读，每 32KB vTaskDelay(2)
      ▼
[PSRAM] 2.12 MB 镜像  ──►  只读内存 VFS /fontmem  ──►  FreeType  ──►  LVGL
```

---

## 3. 行高：任务书里的 17/23/28 是错的

任务书给的是 PIL `ascent + descent` 口径；LVGL 用的是
`face->size->metrics.height >> 6`（`lv_freetype.c`）。两者差 2px：

| 字号 | 任务书（PIL） | **实测 LVGL** | base_line |
|---|---|---|---|
| 12px | 17 | **14** | 3 |
| 14px | 20 | **17** | 3 |
| 16px | 23 | **19** | 4 |
| 20px | 28 | **24** | 4 |

**一个关键巧合**：原来烘焙的 `ui_font_cn_16` 的 `line_height` 正好也是 **19**。
所以所有原本按 16px 排的固定高度容器**一行都不用改**——只有 12px 和 20px 需要重新定位。

实际改动（只列真正动了坐标的）：

| 位置 | 改动 |
|---|---|
| 设置页分区标题 | `SEC_TITLE_H = 20`（19+1），卡片位移 20→20，y 步进 72 |
| 设置页从机列表（8 行） | 降为 12px：`SYNC_ROW_H = 15`、`SYNC_LIST_TOP = 37`，`37+8×15=157` 放进 176（旧版 195 会裁掉最后一行） |
| WiFi 密码对话框 | SSID 提示 y 25→28、输入框 48→52 |
| 播放页歌名 | 20px，y 38→**36**（占 36..60，行高 24） |
| 播放页副标题 | 12px，y 60→**61**（占 61..75，距进度条 82 还有 7px） |
| 播放列表 / 灯控卡片 | **不动**（行高 28 ≥ 19） |

---

## 4. 实测数据

### 4.1 启动（每次开机一次）

```
UIFONT: preload 2122276 B in 3674 ms (578 KB/s), psram free=1686684
UIFONT: validate ok: 2122276 B matches meta
UIFONT: 12px created in 37217 us: line_height=14 base_line=3
UIFONT: 16px created in   208 us: line_height=19 base_line=4
UIFONT: 20px created in   304 us: line_height=24 base_line=4
UIFONT: init ok in 6636 ms | internal free=43511 largest=23552 | psram free=1602908
```

拆解：预载 3.67 s + meta 校验 1.10 s + 建 3 个 face 0.05 s + 页构建 2.5 s。

- 12px 那 37 ms 是**第一个** face，要通读所有表；后续同 face 只改字号，0.2~0.3 ms。
- meta 那 1.1 s 无法再压：`ui_font.meta` 是独立小文件，SPIFFS 为它做一次全盘 LU 扫描。
- `SPIFFS_PAGE_SIZE` 256→1024 后预载从 377 KB/s 提到 **578 KB/s**；
  Kconfig 范围是 256..1024，**已到上限**，这条路没有更多空间。

### 4.2 内存

| 池 | 数值 |
|---|---|
| app bin | 2,588,192 B = 2.47 MiB（4 MB 槽 61.7%，余 1.5 MB） |
| 内部 DRAM（`ui_init` 后） | free 26,111 / largest 23,552 |
| 内部 DRAM（字体建完） | free 43,511 / largest 23,552 |
| PSRAM | 余 1,602,908（字体镜像占 2.12 MB） |

注意：心跳里的 `heap=` 是 `esp_get_free_heap_size()`，在
`CONFIG_SPIRAM_USE_MALLOC` + `ALWAYSINTERNAL=0` 下**把 PSRAM 一起算进去了**，
那个 1.5 MB 会让人误判。已在心跳里加上 `int=空闲/最大连续块`，以后直接看内部数字。

内部堆比接入前少了一半（54 KB → 26 KB），全部是 UI 栈从 10240 提到 32768 的代价。

### 4.3 栈：hwm 读数不可信，"差一点"就崩了三次

| 栈大小 | 现象 |
|---|---|
| 10240 | 首次点设置页 → `stack overflow in UI_Task` |
| 12288 | 同上，REFR_READY 处采样还有 5980 B 余量，却仍被压穿 |
| 20480 | 启动后 headroom=18100 B，8 秒后点设置页仍然压穿 |
| **32768** | 实测渲染峰值 **23852 B**，余 8916 B，连续运行 2.9 小时无异常 |

`-fstack-usage` 扫了 1839 个 `.su` 后得到一个反直觉结论：
**整条 LVGL/FreeType 绘制链的栈帧加起来不到 1.5 KB**
（`lv_obj_redraw 352 B`、`lv_draw_label 160 B`、`refr_area 64 B`），
全固件唯一的大帧是 mp3 解码的 16272 B（在 `MusicPlay_Task`，与 UI 无关）。

所以那 23.4 KB 不是"某个大数组"，而是**多层调用叠加 + 渲染期间落进来的
WiFi/BT 中断**（ESP32 的中断使用被打断任务的栈）。这解释了为什么
"采样还有 6 KB 余量"却仍然压穿。

### 4.4 渲染耗时：窗口数才是瓶颈，不是 SPI

`LV_DISPLAY_RENDER_MODE_PARTIAL` 下，缓冲高度 = 一次重绘窗口高度。
原来只有 **8 行**，240 行要分 **30 段**；每段都要把整棵对象树重走一遍、
每个相交对象重画一次——25 个对象 × 30 段 = **750 次绘制**，而像素只有一屏。

| 配置 | 窗口数 | flush | draw | 首帧总计 |
|---|---|---|---|---|
| 8 行 + `-Og` | 30 | 135 ms | 465 ms | **599 ms** |
| 16 行 + `-Os` | **15** | 101 ms | 252 ms | **364 ms** |

单窗口：flush 6.7 ms（10240 B @15 MHz ≈ 5.5 ms 理论值，吻合）、draw 均 18.0 ms / 峰 42.0 ms。

用两组数据解一个线性模型：`draw ≈ 14.7 ms × 窗口数 + 0.41 µs × 像素数`
（76800 px ≈ 31.5 ms）。也就是说 **220 ms 是"每窗口固定开销"**，真正的像素绘制只占 31 ms。
若再降到 10 段（24 行）预计首帧 ≈ 280 ms，代价是 15 KB 内部 RAM——
当前只剩 26 KB，属于临界，暂不做。

`-Os` 是给 `lvgl__lvgl` 与 `espressif__freetype`（真库目标，不是那个 INTERFACE 包装）
单独追加的，全局仍是 `-Og`。`-O2` 会触发 GCC 15.2 的 IRA 段错误（ICE），已放弃。

---

## 5. 踩过的坑（按代价排序）

| # | 坑 | 症状 | 解法 |
|---|---|---|---|
| 1 | FreeType 直读 SPIFFS | 一个 face 建 10 s+，`task_wdt`、CPU1 `Interrupt wdt` | 整块预载 PSRAM + 内存 VFS |
| 2 | `UI_FLUSH_ROWS_MAX` 8→16 却忘了 `SPI_MAX_TRANSFER_SZ` | 跑 1~3 h 后 `spi_master: txdata transfer > host maximum` 刷屏、屏幕花屏 | 宏移到 `.h` 并放大到 40 行余量；加 `_Static_assert` 让不同步**编译期失败** |
| 3 | `LCD_TOUCH_LOGE` 被 `LCD_TOUCH_DEBUG_LOGS` 编译成空宏 | 刷屏失败**完全静默**，白跑 3 小时 | 错误级日志永远开启，只让 LOGW/LOGI 跟开关 |
| 4 | hwm 与真实栈用量不一致 | 三次 `stack overflow`，每次"明明还有几 KB" | 直接读 SP + `-fstack-usage` 交叉验证；栈定 32768 |
| 5 | IDF 的 `i2c_master` 中断风暴 | 2.35 h 后 `Interrupt wdt timeout on CPU0`，PC 在 `i2c_master_isr_handler_default` | 触摸改 **GPIO 模拟 I2C（50 kHz）**，见 `触摸I2C改为GPIO模拟.md` |
| 6 | `file(GLOB_RECURSE)` 无 `CONFIGURE_DEPENDS` | 新增 `ui_font.c` 没进编译 → `undefined reference` | 正常模式加 `CONFIGURE_DEPENDS`，脚本模式（IDF 需求扫描）不加，两边分开写 |
| 7 | `CONFIGURE_DEPENDS` 在 `cmake -P` 脚本模式下非法 | configure 直接失败在 CMakeLists 第 3 行 | `if(CMAKE_SCRIPT_MODE_FILE)` 分支处理 |
| 8 | CMake 缓存键名 | 改了默认值却仍是旧值（`UI_LVGL_OPT`、`UI_STACK_USAGE` 各踩一次） | 换新键名，或 `idf.py -DKEY=VALUE build` |
| 9 | `NotoSansSC-VF.ttf` 默认轴是 **100（Thin）** | 不显式实例化会渲染成最细字重 | 脚本固定 `--weight 400` |
| 10 | `UI_DEBUG_LOGS=false` 让 `UI_LOGx` 全为空宏 | 第一版所有 `[PHASE2]` 诊断日志一条都没打出来 | 诊断用 `ESP_LOGx`，与 UI 日志开关解耦 |

---

## 6. 遗留与观察项

| 项 | 现状 | 建议 |
|---|---|---|
| 开机 `task_wdt: IDLE0` 告警 | **不 panic**，只打印。触发瞬间 CPU0 跑的是 `wifi/ppTask`（prio 23），回溯全是 `ieee80211_output_process` | 已在预载/校验/建页三处插 `vTaskDelay(2)` 让出。若仍在，属 WiFi 关联期发包，可接受或把 `CONFIG_ESP_TASK_WDT_TIMEOUT_S` 5→15 |
| 设置页渲染峰值 23852 B | 未定位到单点，`.su` 证明是叠加 | 栈 32768 兜住。若以后内部堆紧张，这是第一个该压的 |
| 切页首帧 364 ms | draw 252 ms 里约 220 ms 是每窗口固定开销 | 窗口 16→24 行预计 280 ms，需 15 KB 内部 RAM（现余 26 KB，临界） |
| 长曲名滚动 | 未专项实测 | 首次出现新字会光栅化。字形缓存现为 LVGL 128 KB + FreeType 256 字形，若确有掉帧再加"进页前预热" |
| 启动 6.6 s | 预载 3.7 s + meta 1.1 s + 建 face 0.05 s | 想再省只能改格式（meta 并入字体文件头，省 1.1 s） |
| `bsp/i2c_driver` | 触摸已不用，但仍保留 | 以后接别的 I2C 设备（如传感器）可直接用 |

---

## 7. 验收清单（可复现）

```bash
cd Play_Master
idf.py build
idf.py -p COMx flash monitor       # 必须整片烧：字体在 storage 分区
```

预期日志：

```
UIFONT: preload 2122276 B in ~3700 ms (~580 KB/s)
UIFONT: validate ok: 2122276 B matches meta
UIFONT: 12px/16px/20px created ... line_height=14/19/24
UIFONT: init ok in ~6600 ms | internal free=~43KB | psram free=~1.6MB
Touch: FT6336G ready on soft I2C: SDA=32 SCL=33 ~50kHz, chip id=0x36
UI: [REFR] page N total ~364ms | flush ~101ms / 15 win | draw ~252ms / 14 win
UI: stack[after render]: headroom=~8900 B, used<=23852 B, stack=32768 B
```

降级验证（可完全恢复）：

```bash
idf.py -p COMx erase-flash     # 连 storage 一起擦
idf.py -p COMx app-flash       # 只烧 app → 应出现英文错误页
idf.py -p COMx flash           # 再整片烧回来
```

改字体后必须同步改 `ui_font.meta` 里的 `size=`，否则启动校验失败（会直接看到错误页，
这正是设计意图）。

---

## 8. 相关文档

- `note/FreeType运行时矢量字体-从TTF子集到UI渲染.md` —— **完整实现笔记**（含 Python 处理全过程）
- `docs/03-开发/主音频节点中文字体（FreeType运行时字体）.md` —— 资产/升级/降级操作手册
- `docs/03-开发/触摸I2C改为GPIO模拟.md` —— I2C 中断风暴与 bit-bang 实现
- `docs/07-许可/所用开源/NotoSansSC-授权说明.md` —— OFL-1.1，可商用可嵌入
- `docs/07-许可/所用开源/阿里汉仪智能黑体-授权说明.md` —— 已停用，保留备查
