# 设置页面的同步播放那里显示的：

从节点.....

改进为有多少个从节点接入

已经实现

---

# UI 卡顿优化记录（2026-10-23）

## 背景与目标

主节点（`Play_Master`，ESP32-WROVER-E-N16R8 / D0WD-V3 芯片，8MB PSRAM）UI 原先存在
卡顿，且切页/滚动时会干扰 SD 卡音乐播放。目标：UI 流畅 + 播放不受 UI 操作影响。

## 关键前提（结论性事实，供后续参考）

1. **芯片 PSRAM 不能用于 SPI DMA**：老款 ESP32 的 PSRAM 与 Flash 共用 QSPI 总线、经
   cache 访问，SPI/I2S/LCD 的 DMA 引擎无法直接读写 PSRAM。因此 **flush 缓冲必须留在
   内部 SRAM（`MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL`）**，不能搬 PSRAM。
2. **`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0`**：普通 `malloc()` 优先落 PSRAM。LVGL
   的 `lv_malloc` 走 `LV_USE_STDLIB_MALLOC=1`（clib malloc），所以 LVGL 图片缓存等
   运行时分配会**自动落 PSRAM**，不占内部 DRAM。
3. **`lv_image_decoder_dsc_t`、`lv_draw_buf_handlers_t` 都是私有结构**（定义在
   `src/.../private.h`），外部 App 无法访问其内部字段——手动预解码缓存、覆盖图片缓存
   分配器的做法在公共 API 层面都行不通，已放弃。
4. **内部 RAM 是硬约束**：启动到 bt_audio 完成时内部堆仅剩个位数 KB，flush 缓冲
   当初被迫压到 8 行（5.1KB）。"加大 flush 缓冲"这条路被内部 RAM 卡死。

## 已实施的优化

### 1. 播放页去掉封面（`scr_player.c`）

- 删除 `cover_build()` 及其调用，删除封面资源宏 `UI_COVER_SIZE`、`UI_IMG_COVER`。
- 右信息列（源、标题、副标题、进度条、时间、音量）由 `colX=132` 改为整屏居中
  铺满（`colX=12`、`colW=LCD_WIDTH-24`），文字加 `LV_TEXT_ALIGN_CENTER`。
- 列表入口按钮 `s_btnList` 由 32×26 放大到 43×35。
- **收益**：切播放页不再解码 128×128 PNG（原卡顿主因）。

### 2. 开启 LVGL 图片缓存（`sdkconfig`）

```
CONFIG_LV_CACHE_DEF_SIZE = 16384          # 0 → 16KB
CONFIG_LV_IMAGE_HEADER_CACHE_DEF_CNT = 8  # 0 → 8 条
```

- **收益**：灯控页两张灯图（Light_On/Off.png，48×48）不再每次状态切换/切页都重新
  fopen+fread+LODEPNG 解码，改为命中缓存。缓存 16KB 足够装下两张 48×48 RGB565 图。
- **内存**：缓存经 clib malloc 分配，配合 `ALWAYSINTERNAL=0` 自动落 PSRAM，不占内部 RAM。
- 修正了 `ui.c:278` 一处过时注释（原写 `SPIRAM_MALLOC_ALWAYSINTERNAL=4096`，与实际 `=0`
  语义相反）。

### 3. UI_Task 从 Core 1 挪到 Core 0（`app_main.c`）

- `App_Boot_Task_Create(App_UI_Task, "UI_Task", ..., core)` 的 core 参数 `1` → `0`。
- **收益**：音频实时链路（audio_decoder/MusicPlay/SyncLocalPlay 等）独占 Core 1，UI
  切页/重绘的 CPU 重活与音频解码彻底分核，不再同核抢占。

### 4. 加大 PCM 入口环（ingress）缓冲（`sync_protocol.c/.h`）

- `SYNC_CFG_DEFAULT_INGRESS_BYTES`：24576 → 65536（24KB → 64KB，约 278ms → 741ms
  音频存量）。
- **收益**：切页造成的解码欠载被存量完全吸收，缓解"变慢→加速追回"的抖动。
- **内存**：走 `sync_alloc_psram()`（`MALLOC_CAP_SPIRAM` 优先），纯 PSRAM 开销，内部
  RAM 零占用；不影响主从同步误差（误差由 `sync_delay_ms` 决定）。

### 5. SD 卡 SPI 会话优先级提到 HIGH（`sd_card.c`）

- `Sd_SessionCfg.priority` 与 `SD_Card_Init` 设备注册 `.priority`：`SPI_PRIO_NORMAL`
  → `SPI_PRIO_HIGH`。
- **背景**：SD 卡与 LCD 共用 SPI2 总线，原 SD（NORMAL）低于 LCD（HIGH），滚动刷屏会
  长时间饿死音频读卡。
- **收益**：音频读卡（短事务，一扇区几十 µs）与刷屏（长事务）同级竞争，按
  `session_budget_ms=10` 轮换，配合已有的 `Sd_Session_Yield()` 防饿死，读卡不再被饿死。
- **代价**：滚动可能轻微变慢，但音乐保持连续（这正是"音乐优先"的取舍）。

## 未实施的优化（后续待评估）

- **加大 flush 缓冲**（8→16 行，flush 次数 22→11）：被内部 RAM 见底卡住，需先确认
  内部 DMA RAM 余量才能决定。
- **设置页整页滚动改局部重绘 / lv_list 分页**：改动大，暂缓。
- **预转 C 数组替代运行时 PNG 解码**：灯图/封面彻底绕开解码，需 lv_img_conv 工具，
  改动较大，暂缓。
- **FreeType + 中文矢量字库**：尚未实施；若上需注意其 glyph 缓存会占 RAM、中文渲染
  CPU 密集，需单独权衡。

## 效果

UI 切换已流畅（用户反馈"很舒服"）。播放中切页/滑动仍有轻微卡顿，经上述 3/4/5 项
优化后已大幅缓解；设置页上下滑动 + 播放中的剩余轻微卡顿留待后续继续优化。

## 涉及文件清单

| 文件 | 改动 |
| :--- | :--- |
| `components/App/UI/scr_player.c` | 去封面、信息列居中加宽、列表按钮放大 |
| `components/App/UI/ui_priv.h` | 删除 `UI_COVER_SIZE`、`UI_IMG_COVER` 宏 |
| `components/App/UI/ui.c` | 修正过时注释（ALWAYSINTERNAL 语义） |
| `Play_Master/sdkconfig` | 开 LVGL 图片缓存 16KB + header 缓存 8 条 |
| `main/app_main.c` | UI_Task core 1→0 |
| `components/middlewares/sync_protocol/sync_protocol.c` | ingress 默认 24KB→64KB |
| `components/middlewares/sync_protocol/sync_protocol.h` | ingress_bytes 注释更正 |
| `components/middlewares/sd_card/sd_card.c` | SD 会话优先级 NORMAL→HIGH（两处） |

# 设置页面翻页->为了更流程

**进一步：把翻页的前一页和后一页放大，把前一页按键放到左边的中间。。。1/4页数和当前页面显示放在中间上方。**