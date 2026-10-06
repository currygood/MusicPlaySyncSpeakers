# 主音频节点 UI 界面设计

> **屏幕**：2.8 寸 IPS（ILI9341，SPI）320×240 横屏 + 电容触摸（FT6336G，I²C 轮询）
> **文档范围**：仅描述**主音频节点**的 LVGL 界面；从节点无屏幕，灯控节点无屏幕。
> **核心原则**：UI 是控制指令的"期望发起者"，不是状态的主人——所有状态显示以各模块事件回流与查询接口为准；UI 不直接调用 `bt_audio` / ESP-IDF API；图片/字体资源随固件提供（3 张 PNG 在 Flash `storage` 分区、字体编译进固件）。
> **实现状态**：UI 模块已实现（`components/App/UI/`）；本文档为设计定稿，接口与资源策略以文档为准。

------

## 1. 设计原则与屏幕参数

### 1.1 设计原则

1. **分层边界不破例**：UI 属于 App 层，只调用 App 层模块（MusicPlay / PlayMode / LightControl）的定稿接口与系统服务查询接口（wifi_manager / node_role / OTA / CallPhone / sync_protocol）；**绝不**直接调用 `bt_audio`、`audio_decoder` 等"被上层模块持有的"中间件句柄，更不碰 ESP-IDF API。
2. **指令是期望，状态靠回流**：UI 按键后不自行翻转状态显示（只给按压动效），最终状态以 `music_event_t` / `light_event_t` 等事件携带的快照落定；事件未回流前控件呈"等待"半透明态。
3. **单订阅者接线**：UI 是 MusicPlay / PlayMode / LightControl / OTA 事件的唯一订阅者，由 `app_main` 在创建各模块时把 `on_event` 接线到 UI 提供的转发函数；事件回调上下文**只入队**，不碰 LVGL。
4. **资源随包、固件自带**：全量中文字体与图标（lv_font_conv 生成的 C 数组）编译进固件，3 张 PNG 图片经 `spiffs_create_partition_image` 烧录进 Flash `storage` 分区，SD 卡不参与 UI 资源，界面不依赖任何外置存储。
5. **小屏优先级**：320×240 空间有限，一屏只做一件事；次级功能收进二级页；状态全局常驻（状态栏 + Tab 栏），操作路径最多两跳。

### 1.2 屏幕与触摸参数

| 项目 | 参数 | 备注 |
| :--- | :--- | :--- |
| 分辨率 | 320 × 240，**横屏** | `LCD_Touch.h` 已按 `LCD_WIDTH=320 / LCD_HEIGHT=240` 初始化并实测符合观看方向，不改 MADCTL |
| 颜色格式 | RGB565（16bit） | `LCD_Write_PixelData()` 按 2 字节/像素发送 |
| 刷屏接口 | `LCD_Set_Window()` + `LCD_Write_PixelData()` | 已预留 LVGL flush 用法（见 LCD_Touch.c 注释） |
| 背光 | `LCD_Set_Backlight(bool)` | 本期只做开关，不做 PWM 调光 |
| 触摸 | FT6336G，I²C 地址 0x38 | INT 未接线；`LCD_Touch` 内部任务每 20ms 轮询并缓存 |
| 触摸读取 | `LCD_TOUCH_FT6336G_Get_Touch_Points()` | 非阻塞返回快照 `Touch_Point_t{touchX, touchY, touchCount}` |
| 多点 | 硬件支持 2 点，**UI 只用第 1 点** | 不做多指手势 |
| 总线 | LCD 与 SD 卡共用 SPI2 | 分时复用策略见 2.5 |

### 1.3 三段式布局（全局骨架）

```reStructuredText
┌────────────────────────────────────────────────┐
│  状态栏（常驻，高 24px）                         │
├────────────────────────────────────────────────┤
│                                                │
│           内容区（高 176px，各页面复用）          │
│                                                │
├────────────────────────────────────────────────┤
│       ♪        │        💡        │      ⚙      │  ← Tab 栏（常驻，高 40px，纯图标）
└────────────────────────────────────────────────┘
```

- 三个 Tab 页共享同一骨架，切换 = 切换内容区子容器（显示/隐藏），状态栏与 Tab 栏永不销毁。
- 二级页（本地音乐列表、WiFi 配网）在**内容区内**以覆盖层呈现，自带返回键；密码键盘页例外，全屏覆盖 Tab 栏以获得键盘高度（见 5.6）。

### 1.4 职责边界（速查）

| UI 想做的事 | 必须经过 | 绝不直接碰 |
| :--- | :--- | :--- |
| 播放/暂停/上下曲/音量/播放模式 | `music_play_command()` / `music_play_set_mode()` / `music_play_set_volume()` | `bt_audio_send_ctrl_cmd()`（AVRCP 由 MusicPlay 转发） |
| 本地选曲 | `music_play_select_track()` / `music_play_scan_local()` | `audio_decoder_*`、FATFS |
| 手动切蓝牙/本地源 | `play_mode_set()` | `music_play_switch_source()`（仅 PlayMode 接线回调可用） |
| 开关灯 | `light_control_send(cmd, "bedroom"/"livingroom")` | socket、JSON 组包、组播 |
| WiFi 配网 | `node_role_get/set()` + `wifi_manager_set_sta()` | NVS 直写、esp_wifi |
| 连接状态显示 | 轮询 `wifi_manager_get_state()/get_ip()` | 注册 wifi_manager 事件（约定 UI 轮询不订阅） |
| 固件版本/升级 | `OTA_Update_*` 全局 API + `ui_on_ota_event()` | esp_https_ota 内部 |
| 语音状态显示 | 轮询 `call_phone_get_state()` | esp-sr、HFP |

------

## 2. LVGL 显示与触摸接入方案

### 2.1 总体方案：自持 UI_Task（推荐）

不使用 `esp_lvgl_port` 的显示适配（它要求 `esp_lcd_panel_io` 句柄，而现有 LCD 走自研 `spi_driver` 裸 SPI），改为 UI 模块**自持 LVGL 初始化与任务循环**：

```reStructuredText
UI_Task（Core 1，优先级 12，栈 13312 —— 对齐架构文档任务表）
  ├─ lv_init()
  ├─ lv_display_create(320, 240) + flush_cb → LCD_Set_Window/LCD_Write_PixelData
  ├─ lv_indev_create() + read_cb → LCD_TOUCH_FT6336G_Get_Touch_Points()
  ├─ 挂载 Flash storage 分区（SPIFFS）＋注册 lv_fs 盘符 'F'（读 ui_img/*.png；字体已编译进固件）
  ├─ 页面构建（ui_screens_create）
  └─ 循环：事件队列出队 → lv_timer_handler() → vTaskDelay(10ms)
```

- **`esp_lvgl_port` 依赖处置建议**：从 `idf_component.yml` 移除（避免引入第二个 LVGL 任务与双重加锁），仅保留 `lvgl/lvgl: "^9"`。此改动属实现期动作，本文档只做建议、不改配置。
- 实现期需将 `LCD_Touch.h` 中 `LCD_TOUCH_SELF_TEST` 改 0（自检让位给 LVGL 刷屏）。

### 2.2 显示适配（flush_cb 示意）

```c
static void ui_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    /* ILI9341 SPI 按 big-endian 收 RGB565：发送前交换字节序 */
    lv_draw_sw_rgb565_swap(px_map, lv_area_get_width(area) * lv_area_get_height(area));

    LCD_Set_Window(area->x1, area->y1, area->x2, area->y2);
    LCD_Write_PixelData((const uint16_t *)px_map,
                        (area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1));
    lv_display_flush_ready(disp);   /* Spi_Transmit 返回即已出队，视为完成 */
}
```

### 2.3 触摸适配（read_cb 示意）

```c
static void ui_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    Touch_Point_t tp;
    if (LCD_TOUCH_FT6336G_Get_Touch_Points(&tp) && tp.touchCount > 0) {
        data->point.x = tp.touchX;
        data->point.y = tp.touchY;
        data->state   = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
    data->continue_reading = false;   /* 每 30ms 由 LVGL 轮询一次即可 */
}
```

- 触摸数据来自 `LCD_Touch` 内部任务的 20ms 缓存快照，read_cb 无 I²C 开销；LVGL 侧读周期 30ms，等效跟手率 ≈33Hz，满足单点点击与列表滚动。
- 本期不做滑动手势切页（跟手性风险），仅使用点击与 LVGL 内建滚动。

### 2.4 渲染资源约束

| 资源 | 规格 | 说明 |
| :--- | :--- | :--- |
| 渲染缓冲 | 320 × 40 行 × 2B = **25.6KB**，`heap_caps_malloc(MALLOC_CAP_DMA \| MALLOC_CAP_INTERNAL)` | `LCD_Write_PixelData()` 将缓冲直接交给 `Spi_Transmit()`，ESP32 经典 SPI DMA 要求数据在内部 RAM；单块传输 ≈5ms@40MHz，全屏 6 块 ≈30ms+ |
| 刷新模式 | `LV_DISPLAY_RENDER_MODE_PARTIAL` | 脏区渲染，静止画面零刷屏流量 |
| 刷新周期 | `LV_DEF_REFR_PERIOD = 33ms`（≈30fps） | 限帧，降低 SPI 占空比 |
| LVGL 堆 | `LV_USE_STDLIB_MALLOC = 1`（走 ESP-IDF malloc，配合 SPIRAM_USE_MALLOC 落 PSRAM） | 内部 RAM 紧张（BT + WiFi + 音频 FIFO），对象/字体大块放 PSRAM |
| 字体缓存 | 字形位图驻留在 LVGL 堆（PSRAM） | 字体 C 数组已编译进固件，无需外部字体文件 |

### 2.5 SPI2 总线共存策略（LCD 刷屏 vs SD 读写）

LCD 与 SD 分时复用 SPI2（各持独立 CS；SD 侧用手动 CS 会话，事务仲裁由 IDF spi_bus_lock 保证，**不会死锁，只有时延抖动**）。缓解措施：

1. **部分刷新**：UI 大多数帧只刷脏区（进度条、时间、状态图标），全屏重绘仅发生在切页瞬间；
2. **限帧 30fps** + 40 行分块传输，把单次 SPI 占用切成 ≤5ms 的碎片；
3. **大图一次加载**：封面等 PNG 在进入播放页时一次性解码并缓存，播放过程中不反复读 Flash `storage` 分区；
4. **对音频的影响评估**：音频链路本身不走 SPI（I2S 独立），SPI 争用最坏情况是解码任务读卡变慢 → PCM 产出抖动，由 `sync_protocol` ingress 环形缓冲（16~32KB ≈ 0.2~0.4s @44.1kHz/16bit/单声道）吸收，不致爆音。

### 2.6 lv_conf 关键配置清单

| 配置项 | 值 | 说明 |
| :--- | :--- | :--- |
| `LV_COLOR_DEPTH` | 16 | RGB565 |
| 自定义 lv_fs 盘符（代码 `ui_disp.c` 注册） | 盘符 `'F'` + VFS 前缀 `/storage` | `F:/ui_img/…` → `/storage/ui_img/…`（见 8.3） |
| `LV_USE_LODEPNG` | 1 | Flash `storage` 分区 PNG（封面/图标）解码 |
| `LV_USE_FREETYPE` | 0 | 不启用运行时 TTF 渲染，字体一律为 lv_font_conv 生成的 C 数组并编译进固件 |
| `LV_USE_FONT_COMPRESSED` | 0 | 字体生成时 `--no-compress` 配套 |
| `LV_USE_STDLIB_MALLOC` | 1 | LVGL 堆走系统 malloc（配合 PSRAM） |
| `LV_DEF_REFR_PERIOD` | 33 | ≈30fps |
| `LV_INDEV_DEF_READ_PERIOD` | 30 | 对齐触摸轮询节奏 |
| `LV_FONT_DEFAULT` | `ui_font_cn_16` | 界面默认中文字体（编译进固件，见 3.2） |
| `LV_FONT_MONTSERRAT_16 / _24` | 1 | 启用内置 symbol 字形（控件图标，字号档见 3.3） |

------

## 3. 视觉规范

### 3.1 配色表（深色主题）

| 语义 | 色值 | 用途 |
| :--- | :--- | :--- |
| 背景色 BG | `#101418` | 页面/内容区底色 |
| 面板色 PANEL | `#1A2026` | 卡片、列表项、键盘 |
| 面板高亮 | `#232B33` | 列表项按压、当前曲高亮底 |
| 分隔线 | `#2A323B` | 面板描边、分区线 |
| 主色 PRIMARY | `#3D9BFF` | 蓝牙图标、进度条、开关选中、选中 Tab |
| 强调色 ACCENT | `#FFB03A` | 灯控"开"状态、音量图标 |
| 文字主色 | `#E8EAED` | 标题、正文 |
| 文字次色 | `#9AA0A6` | 副标题、说明文字 |
| 文字禁用 | `#5F6368` | 不可用控件 |
| 成功色 | `#51CF66` | 已同步、灯"开"回执 |
| 错误色 | `#FF5C5C` | 断开、失败 toast |
| 未知态 | `#6B7280` | 灯 UNKNOWN、图标灰显 |

### 3.2 字体方案与字号规范（C 数组编译进固件）

中文字体与图标字体**不入 SD 卡**：由 `lv_font_conv` 生成 C 数组（`ui_font_cn_16.c` / `ui_icons_16.c` / `ui_icons_24.c`）**编译进固件**，UI 通过 `const lv_font_t`（`ui_priv.h` `extern` 声明）直接引用，无 binfont/TTF 运行时文件。

| 字号 | 用途 | 字体资产（C 数组编译进固件） |
| :--- | :--- | :--- |
| 20px | 歌曲标题、页面标题 | 未单独生成时回退放大 16px |
| 16px | 正文、按钮、列表项、时间 | `ui_font_cn_16`：Noto Sans SC 子集 521 字 + ASCII（`--bpp 4` 抗锯齿） |
| 12px | 副标题、状态栏文字、说明 | 复用 16px 字形缩放 |
| 16/24px | 状态栏 / Tab 栏图标 | `ui_icons_16` / `ui_icons_24`（Material Symbols 字形） |

- 生成命令示例见 8.2（lv_font_conv，`--format lvgl` 输出 C 数组）。
- 数字/时间统一用 ASCII 字形，不单独做数字字体。

**原则**：所有功能控件一律用**图形图标**表达（播放/暂停/上下曲/设置/灯控等），不用文字按钮；仅"动作类按钮"（确定/取消/WiFi 配网等）和源 chip 保留文字。图标全部为字形（内置 symbol ＋ 自定义 iconfont），不依赖任何存储介质。

**字形来源（两类，均随固件编译）**：

| 来源 | 覆盖范围 | 说明 |
| :--- | :--- | :--- |
| LVGL 内置 symbol 字体（FontAwesome 子集） | 播放 ▶⏸、上下曲 ⏮⏭、循环 ↻、音量、WiFi、蓝牙、设置 ⚙、列表 ≡、刷新等 | lv_conf 启用 `LV_FONT_MONTSERRAT_16 / _24` 即含对应字号 symbol 字形；零额外文件 |
| 自定义图标字体（Material Icons 子集，lv_font_conv 生成 C 数组） | 内置 symbol 缺失的 4 个字形：**灯泡 lightbulb**（Tab + 灯控页 Tab 位）、**麦克风 mic**（状态栏 + 设置页）、**单曲循环 repeat_one**（播放模式）、**同步 sync** | ≈10KB，与 symbol 同用法（label 渲染），同样单色可着色 |

**图标 → 控件映射表**（实现对照）：

| 位置 | 图标 | 字形来源 |
| :--- | :--- | :--- |
| 播放页 ⏮ / ▶⏸ / ⏭ | `LV_SYMBOL_PREV` / `PLAY` / `PAUSE` / `NEXT` | 内置 |
| 播放模式（顺序 / 单曲循环） | `LV_SYMBOL_LOOP` / repeat_one | 内置 / 自定义 |
| 播放页列表入口 | `LV_SYMBOL_LIST` | 内置 |
| 音量 | `LV_SYMBOL_VOLUME_MAX / MID / MUTE` | 内置 |
| Tab 栏（纯图标，选中主色/未选灰） | ♪=`AUDIO`、灯泡=lightbulb、⚙=`SETTINGS` | 内置 / 自定义 / 内置 |
| 状态栏 WiFi / 蓝牙 | `LV_SYMBOL_WIFI` / `BLUETOOTH` | 内置 |
| 状态栏同步 / 麦克风 | sync / mic | 自定义 |
| 灯控页大图标 48px | bulb_on / bulb_off（彩色） | Flash `storage` 分区 PNG（`F:/ui_img/Light_On.png` / `Light_Off.png`；缺失回退自定义灯泡字形） |

**控件图标不建议用 PNG 的原因**：控件图标需要随状态变色（选中/未选、ON/OFF、pending 半透明、异常红色），PNG 颜色定死、每种状态都要出一张变体图；字体字形改一个 style 属性即可换色/变透明度。**PNG 只用于颜色固定的图**（封面、彩色灯泡大图标）。

- 无电量显示——硬件无电量计，不虚构。

### 3.4 布局网格与控件规范

| 项目 | 规范 |
| :--- | :--- |
| 网格 | 8px 基准网格；页面内边距 8px；面板间距 8px |
| 卡片圆角 | 卡片 8px、按钮 4px |
| 触控目标 | 主控制键 ≥ 48×48（播放页三键）；Tab 项 ≥ 96×40；列表项视觉高 32px、点击热区 ≥ 40px |
| 按压反馈 | 统一 `LV_STATE_PRESSED` 换底色（面板高亮色），不用缩放动画（省 CPU） |
| 跑马灯 | 长文本 `LV_LABEL_LONG_SCROLL_CIRCULAR`，仅标题类使用 |
| Toast | 底部浮出（Tab 栏上方 4px），2s 自动消失，同文案去重 |

------

## 4. 页面结构与导航总览

### 4.1 页面清单

| 页面 | 层级 | 说明 |
| :--- | :--- | :--- |
| 播放页 | Tab 1（默认） | 播放控制主界面，开机落在此页 |
| 本地音乐列表 | 二级（覆盖内容区） | 本地曲目选择 |
| 灯控页 | Tab 2 | 卧室/客厅灯开关 |
| 设置页 | Tab 3 | 源切换 / 网络 / 同步 / 语音 / OTA / 关于 |
| WiFi 扫描列表 | 二级（覆盖内容区） | 从设置页进入 |
| 密码键盘页 | 二级（全屏覆盖） | 从扫描列表点选进入 |

### 4.2 导航结构图

```reStructuredText
                ┌──────────────────────────────────────┐
                │        状态栏（常驻，三页共享）        │
                ├──────────────────────────────────────┤
   Tab 切换 ──► │            内容区 320×176            │ ◄── 二级页覆盖层
                ├──────────────────────────────────────┤
                │      ♪       │       💡       │  ⚙   │   ← Tab 栏纯图标（3.3）
                └──────────────────────────────────────┘

  播放页 ──[列表]──► 本地音乐列表 ──[←]──► 播放页
  设置页 ──[WiFi 配网]──► 扫描列表 ──[点选]──► 密码键盘 ──[确定]──► 设置页
```

### 4.3 通用规则

- **返回**：二级页左上角 `←`（44×44 热区）；Tab 切换自动关闭已打开的二级页。
- **空态**：列表无数据时居中显示图标 + 一句说明 + 可执行的补救入口（如"放入 /music 目录"）。
- **错误提示**：一律 Toast，不打断当前页；状态类错误同时反映到常驻图标（状态栏/设置页）。
- **状态唯一来源**：页面重建（切页返回）时先取各模块 `get_view()/get_state()` 快照渲染，再靠事件增量刷新——不允许 UI 内部缓存"上次的猜测"。

------

## 5. 各页面详细设计

### 5.1 状态栏（常驻，24px）

```reStructuredText
┌────────────────────────────────────────────────┐
│  (WiFi图标)      (BT)      (同步)      (麦克风) │   左：WiFi　中：BT　右：同步+麦克风
└────────────────────────────────────────────────┘
```

图标数据源与状态映射（全部轮询/事件驱动，见 6.7 轮询表）：

| 图标 | 数据源 | 状态映射 |
| :--- | :--- | :--- |
| WiFi | 轮询 `wifi_manager_get_state()` | IDLE→灰；CONNECTING→白闪烁(800ms)；CONNECTED→白满格；DISCONNECTED→空心+闪烁；ERROR→红× |
| 蓝牙 | `music_play_get_view()->source` 推断 | `MUSIC_SOURCE_BT`→亮主色；否则暗灰（不直接查 bt_audio） |
| 同步 | 轮询 `sync_protocol_master_get_status()` | 查询失败/未 init→灰(未启用)；`slave_online && PLAYING`→绿；`PLAYING && !slave_online`→黄闪；ERROR→红；其余→灰 |
| 麦克风 | 轮询 `call_phone_get_state()` | IDLE→暗；LISTENING→白；CONNECTING→黄闪；STREAMING→绿 |

- 状态栏 v1 不可点；可选增强：点击任意图标跳转设置页对应分区。

### 5.2 播放页（Tab 1，默认主页）

内容区 176px 纵向分两段：顶区 128px（左封面 + 右信息列）+ 控制行 48px：

```reStructuredText
┌────────────────────────────────────────────────┐
│ ┌──────────┐   [●蓝牙]               [≡ 列表]  │
│ │          │   歌曲标题歌曲标题……（20px 跑马灯） │   顶区 128px：
│ │ 专辑封面 │   歌手 · 专辑（12px 次色）          │   左：封面 128×128（占位图）
│ │ 128×128  │   ──────────●────────────          │   右：信息列
│ │          │   01:23            /  04:56        │
│ │          │   🔊 ───●──────────                │
│ └──────────┘                                    │
│     [🔂]       ⏮        ▶⏸        ⏭           │ ← 控制行 48px：模式+三主控键
└────────────────────────────────────────────────┘
```

**元素与数据通道**：

| 元素 | 数据来源（显示） | 动作（控制） |
| :--- | :--- | :--- |
| 封面 128×128 | `F:/ui_img/Music_Album.png`（Flash `storage` 分区），**恒为占位图**（BT 源无封面接口、本地源不解析 ID3 内嵌图）；分区缺失→内置色块占位 | 无 |
| 源 chip | `view->source`；`PLAY_MODE_EVT_CHANGED` 刷新 | 点击 → `play_mode_set(另一源)`（见 6.3） |
| 列表入口 | — | 点击 → 本地音乐列表；**BT 源时禁用置灰**（列表是本地源功能） |
| 标题/副标题 | `view->track`：本地源=`display_name`+文件名；BT 源=歌名+`artist · album`（承载方式见 9.2 建议） | — |
| 进度条+时间 | 500ms 轮询 `music_play_get_position_ms()`（9.2 建议）；时长 `view->track->duration_ms` | **只读，不支持拖动**（`audio_decoder` 不做 seek，接口不虚构）；**BT 源整段隐藏**（AVRCP 无 position 接口） |
| ⏮ ▶⏸ ⏭ | `view->state` 决定 ▶/⏸ 图标 | `music_play_command(TOGGLE/NEXT/PREV)` |
| 模式按钮 | `view->mode`：顺序=↻ / 单曲循环=🔂（图标切换，字形见 3.3） | `music_play_set_mode()` |
| 音量 slider | `view->volume`（`MUSIC_EVT_VOLUME_CHANGED` 回流） | 拖动实时 `music_play_set_volume()`（UI 侧"拖动中"标志防回流抖动） |

**"指令 = 期望"的 UI 呈现**：

- 按键只做按压动效，**不本地翻转** ▶/⏸；
- `view->pending_cmd == true` 或发出命令 500ms 内未收到对应 `MUSIC_EVT_PLAY_STATE_CHANGED` → 主控键呈半透明"等待回流"态，事件到达后恢复；
- 蓝牙源状态最终以 AVRCP 回流事件为准（架构文档 6.2 约定），UI 不做超时"乐观提交"。

### 5.3 本地音乐列表（二级页，覆盖内容区）

```reStructuredText
┌────────────────────────────────────────────────┐
│ [← 返回]   本地音乐 (12)                        │
├────────────────────────────────────────────────┤
│  ▶ 春天在哪里.mp3            （高亮=当前曲）     │
│  晚风.wav                                      │
│  City Pop Mix.mp3                              │
│  …（可滚动，文件序=播放序）                       │
└────────────────────────────────────────────────┘
```

- 数据：进入页面时 `music_play_scan_local()` → `music_play_get_track_count()` / `music_play_get_track(i)` 渲染；当前曲按 `view->track_index` 高亮（主色底 + ▶）；收到 `MUSIC_EVT_PLAYLIST_CHANGED` 重建列表。
- 点击项 → `music_play_select_track(i)` → 自动返回播放页。
- 空态：SD 未挂载 → "SD 卡未挂载"；挂载但无文件 → "无音乐文件，请将 MP3/WAV 放入 /music 目录"。

### 5.4 灯控页（Tab 2）

```reStructuredText
┌────────────────────────────────────────────────┐
│  ┌──────────────┐        ┌──────────────┐      │
│  │      💡      │        │      💡      │      │
│  │    卧室灯    │        │    客厅灯    │      │
│  │   ● 开(琥珀) │        │  ○ 未知(灰)  │      │
│  │  ─────────●  │        │  ●─────────  │      │
│  └──────────────┘        └──────────────┘      │
│   灯状态以回执与 30s 周期上报为准                 │
└────────────────────────────────────────────────┘
```

- 两张卡片 ≈148×128：图标 48px + 名称 16px + 状态文字 12px + switch；灯名固定 `bedroom` / `livingroom`（灯控协议规范）。大图标优先用 Flash `storage` 分区彩色 PNG（`F:/ui_img/Light_On.png` / `F:/ui_img/Light_Off.png`，点亮态可做琥珀色发光效果），缺失时回退自定义灯泡字形（3.3）。
- **三态显示**：UNKNOWN（灰"未知"）/ ON（琥珀"开"）/ OFF（面板色"关"）；初始与重连后用 `light_control_get_state()` 恢复。
- **交互时序**：点击 switch → `light_control_send(cmd, name)` → 卡片进入等待态（switch 半透明 + 图标闪烁）→ `LIGHT_EVT_ACK` 按回执 `ok` 落定；`LIGHT_EVT_ERROR`（回执异常或 90s 上报超时）→ Toast"控制失败/状态超时"，状态回落 UNKNOWN。
- `LIGHT_EVT_STATUS`（30s 周期上报）静默刷新图标，无动画。
- LightControl 句柄为 NULL → 整页显示"灯控未启用"。

### 5.5 设置页（Tab 3，可滚动）

```reStructuredText
┌────────────────────────────────────────────────┐
│ ── 播放源 ──────────────────────────────────── │
│   [ ● 蓝牙 ]   [ ○ 本地 ]                      │
│   蓝牙连接期间将自动优先蓝牙模式（12px 次色）     │
│ ── 网络 ────────────────────────────────────── │
│   WiFi:  MyHomeWiFi            已连接           │
│   IP:    192.168.1.23                          │
│   [ WiFi 配网 ]                                │
│ ── 同步播放 ────────────────────────────────── │
│   从节点: 已同步    偏移: +2ms    丢包: 0 包     │
│ ── 语音助手 ────────────────────────────────── │
│   麦克风: 监听唤醒词中（唤醒词：你好小智）        │
│ ── 固件更新 ────────────────────────────────── │
│   当前版本: v1.0.2        [ 检查更新 ]          │
│   ▓▓▓▓▓▓░░░░ 62%  正在下载固件…                 │
│ ── 关于 ────────────────────────────────────── │
│   设备: PlayMaster    版本: v1.0.2              │
│   SD 卡: 已挂载                                │
└────────────────────────────────────────────────┘
```

| 分区 | 数据/动作 | 备注 |
| :--- | :--- | :--- |
| 播放源 | `play_mode_get()` 显示、`play_mode_set()` 切换 | 与播放页源 chip 等效，二选一入口即可，数据同源 |
| 网络 | 轮询 `wifi_manager_get_state()/get_ip()`；`[WiFi 配网]` → 5.6 | ERROR 态红字"凭据错误/重试用尽" |
| 同步播放 | 1s 轮询 `sync_protocol_master_get_status()` | 未 init → "未启用"；显示 `slave_online`、`last_slave_offset_us`（换算 ms）、`dropped_pkts` |
| 语音助手 | 1s 轮询 `call_phone_get_state()` | 只读展示，无开关（CallPhone 由唤醒词驱动，不对外发事件） |
| 固件更新 | `OTA_Update_GetLocalVersion()`；`[检查更新]` → `OTA_Update_CheckAndUpgrade()`；进度走 `ui_on_ota_event()` | busy（`OTA_Update_IsBusy()`）时按钮禁用；事件文案映射见 6.6 |
| 关于 | 版本 / 设备名 / `SD_Card_Is_Mounted()` | SD 状态用于排障 |

### 5.6 WiFi 配网（二级页）

**① 扫描列表页（覆盖内容区）**

```reStructuredText
┌────────────────────────────────────────────────┐
│ [← 返回]   选择网络                  [⟳ 刷新]  │
├────────────────────────────────────────────────┤
│  MyHomeWiFi                              ▂▄▆█  │
│  Xiaomi_Room_5G                         ▂▄▆    │
│  TP-LINK_2F                             ▂▄     │
│  …（按 RSSI 降序，可滚动）                       │
│                                                │
│  扫描完成：8 个网络            [ 手动输入 SSID ] │
└────────────────────────────────────────────────┘
```

- 进入页面自动 `wifi_manager_scan_start()`（**接口为上游补充建议，见 9.1**）；列表先显示"扫描中…"，完成后按 RSSI 降序填充；`[⟳ 刷新]` 重新扫描。
- 扫描期间音频组播可能短暂中断（信道跳转），页面顶部显示 12px 提示"扫描时音乐可能短暂停顿"。
- 扫描失败/为空 → `[手动输入 SSID]` 兜底，进入与点选相同的键盘页（多一个 SSID 输入框）。

**② 密码键盘页（全屏覆盖 Tab 栏，状态栏保留）**

```reStructuredText
┌────────────────────────────────────────────────┐
│ (状态栏)                                       │
│ [← ] 连接 MyHomeWiFi                           │
│ ┌──────────────────────────────────────┐ [👁]  │
│ │ password_••••••                      │       │
│ └──────────────────────────────────────┘       │
│ ┌────────────────────────────────────────────┐ │
│ │ [ q w e r t y u i o p ]                    │ │
│ │ [ ⇧  a s d f g h j k l  ⌫ ]                │ │
│ │ [123] [        空   格        ]            │ │
│ │ [ 确  定 ]                 [ 取  消 ]      │ │
│ └────────────────────────────────────────────┘ │
└────────────────────────────────────────────────┘
```

- 键盘用 LVGL 内建 `lv_keyboard`（大小写/数字符号切换），密码默认掩码，`👁` 切换明文；
- `[确定]` → 配网流程（见 6.5）→ 返回设置页；`[取消]`/`←` → 返回扫描列表；
- 键盘页临时隐藏 Tab 栏以获得键盘高度（状态栏 24 + 标题 24 + 输入框 36 + 键盘 ≈120 + 按钮行 32 = 236 ≤ 240）。

------

## 6. UI ↔ 各模块数据流

### 6.1 事件接线总览（app_main 装配）

```reStructuredText
app_main 初始化顺序（UI 最后创建）：
  MusicPlay 创建  cfg.on_event = ui_on_music_event
  PlayMode 创建   cfg.on_event = ui_on_play_mode_event
  LightControl 创建 cfg.on_event = ui_on_light_event
  OTA_Update_Init  cfg.event_cb = ui_on_ota_event
  ……
  ui_init(&ui_cfg)   ← 注入各模块句柄（见第 7 章）
```

- UI 是以上四路事件的**唯一订阅者**（各模块回调为单槽位，由 app_main 让渡给 UI）。
- PlayMode 决策（自动切源）产生的 `PLAY_MODE_EVT_CHANGED` 同样走该链路刷新源 chip。

### 6.2 播放控制流（UI → 手机 AVRCP / 从节点）

```reStructuredText
UI 触摸(播放页) → music_play_command(TOGGLE/NEXT/PREV/VOL)
   ├─ BT 源：→ bt_audio_send_ctrl_cmd() → AVRCP → 手机
   │         → 手机回流 BT_AUDIO_EVT_* → MusicPlay → MUSIC_EVT_*(view 快照) → ui_on_music_event → 队列 → UI 刷新
   └─ 同时： → sync_protocol_master_broadcast_cmd(SYNC_CMD_*) → 从节点跟随
```

- UI 只发"期望"，最终状态以 `music_event_t` 携带的 `music_play_view_t` 快照为准（与架构文档 6.2 完全一致）。

### 6.3 手动切源流（UI → PlayMode）

```reStructuredText
UI 点击源 chip / 设置页源按钮 → play_mode_set(source)
   → PlayMode 决策 → on_switch 回调（app_main 接线）→ music_play_switch_source()
   → PLAY_MODE_EVT_CHANGED → UI 刷新
```

- **蓝牙优先规则提示**：手机仍连接时手动切本地，会在下一次轮询（≤1s）被自动拉回蓝牙——UI 检测到 `PLAY_MODE_EVT_CHANGED` 与用户期望不符时，Toast 提示"蓝牙已连接，已保持蓝牙模式"。

### 6.4 灯控流（UI → ESP8266）

```reStructuredText
UI 点击 switch → light_control_send(on/off, "bedroom")   （等待态）
   → wifi_manager 组播 239.0.0.2:8889 → ESP8266 ack → LIGHT_EVT_ACK → UI 落定图标
   （另：30s 周期 status / 90s 超时 → LIGHT_EVT_STATUS/ERROR → UI 静默刷新/Toast）
```

### 6.5 配网流（UI → node_role → wifi_manager）

```reStructuredText
UI [确定] → node_role_get()          （读缓存副本）
          → 改 cfg.wifi_ssid / wifi_password
          → node_role_set(&cfg)      （持久化，NVS 唯一入口）
          → wifi_manager_set_sta(ssid, password)   （发起连接）
          → 返回设置页；状态栏 WiFi 图标闪烁
轮询 wifi_manager_get_state()：
   CONNECTED    → Toast "已连接" + 显示 IP
   ERROR        → Toast "连接失败，请检查密码"
   DISCONNECTED → Toast "已断开，重连中"
```

- 持久化与发起连接分离（node_role 写 NVS、wifi_manager 只连接），UI 严格按此两步调用，不自行封装"保存"语义。

### 6.6 OTA 流（UI → ota_manager）

`[检查更新]` → `OTA_Update_CheckAndUpgrade()`（模块内部自动建任务，UI 立即返回），进度与结果全部经 `ui_on_ota_event()` 刷新设置页分区：

| 事件 | UI 表现 |
| :--- | :--- |
| `CHECK_STARTED` | 分区显示"正在检查更新…" |
| `UP_TO_DATE` | Toast"已是最新版本" |
| `UPDATE_AVAILABLE` | 分区显示"发现新版本 vX" |
| `DOWNLOAD_STARTED` | 进度条出现，"开始下载…" |
| `DOWNLOAD_PROGRESS` | 进度条 n% |
| `DOWNLOAD_FINISHED` | "下载完成，校验通过" |
| `READY_TO_REBOOT` | "升级完成，即将重启" |
| `ABORTED` | 进度条消失，Toast"升级失败已中止" |

- 下载期间音频/组播由 OTA 事件机制由上层决定是否让路（架构文档 3.4.4），UI 不做额外处理，仅如实展示进度。

### 6.7 轮询表（UI_Task 内 lv_timer 驱动）

| 数据 | 接口 | 周期 | 用途 |
| :--- | :--- | :--- | :--- |
| 本地播放进度 | `music_play_get_position_ms()`（9.2 建议） | 500ms | 播放页进度条（仅本地源） |
| WiFi 状态/IP | `wifi_manager_get_state()` / `get_ip()` | 1s | 状态栏、设置页 |
| 同步状态 | `sync_protocol_master_get_status()` | 1s | 状态栏、设置页 |
| 语音状态 | `call_phone_get_state()` | 1s | 状态栏、设置页 |
| 灯最近上报 | `light_control_get_state()` | 仅进入灯控页时一次 | 图标恢复，之后靠事件 |

- 慢查询统一在 UI_Task 上下文执行（各查询接口自身线程安全）；不注册 wifi_manager 事件（架构文档约定 UI 走查询）。

### 6.8 线程与加锁约束

1. **所有 `lv_*` 调用只允许发生在 UI_Task**（自持方案下天然单任务，无需额外互斥锁）；
2. `ui_on_*_event()` 四个转发函数运行在 MusicPlay_Task / SmartHome_Task / OTA_Task 上下文，内部**只做 `xQueueSend`**（深度 16，事件结构含快照拷贝；队满丢弃并置"脏标记"，下一轮全量刷新兜底）；
3. UI_Task 主循环每次先排空事件队列，再 `lv_timer_handler()`，再 `vTaskDelay(10ms)`；
4. UI 发出的控制命令（`music_play_command` 等）本身线程安全（各模块内部命令队列），UI_Task 直接调用即可。

------

## 7. UI 模块对外接口（ui.h 定稿）

### 7.1 规划头文件 `components/App/UI/ui.h`

```c
/* ===== UI 模块对外接口（定稿） =====
 * 装配约束：ui_init() 在各 App 模块创建之后、storage 分区挂载检查之后调用；
 *           各 on_event 接线（6.1）发生在各模块 create 之前。
 * 降级约定：ui_cfg_t 中任何句柄为 NULL 时 UI 照常运行，对应功能显示"未就绪"。
 */
typedef struct ui_s *ui_handle_t;

typedef struct {
    music_play_handle_t    music;       /* 可 NULL：播放页/列表页显示"播放模块未就绪" */
    play_mode_handle_t     play_mode;   /* 可 NULL：手动切源入口禁用 */
    light_control_handle_t light;       /* 可 NULL：灯控页显示"灯控未启用" */
    wifi_manager_handle_t  wifi;        /* 可 NULL：WiFi 图标灰显、配网入口禁用 */
    call_phone_handle_t    call_phone;  /* 可 NULL：麦克风图标灰显 */
    /* sync_protocol / OTA / node_role 为全局 API，无需句柄；未初始化时按错误返回值降级 */
} ui_cfg_t;

/* 生命周期 */
esp_err_t ui_init(const ui_cfg_t *cfg);   /* 内含 LVGL 初始化、storage 分区图片加载（可失败降级）、页面构建、UI_Task 创建 */
void      ui_deinit(void);                /* 删除 UI_Task、释放 LVGL（整机下电路径才使用） */

/* ===== 事件接线入口（app_main 将各模块 on_event 指向以下函数） =====
 * 调用上下文：MusicPlay_Task / SmartHome_Task / OTA_Task 等
 * 硬性约束：内部只做入队（6.8），禁止调用任何 lv_* 与 UI 查询接口
 */
void ui_on_music_event(const music_event_t *evt, void *user_ctx);
void ui_on_play_mode_event(const play_mode_event_t *evt, void *user_ctx);
void ui_on_light_event(const light_event_t *evt, void *user_ctx);
void ui_on_ota_event(const ota_update_event_info_t *info, void *user_ctx);
```

### 7.2 内部结构（供实现参考，不对外）

```reStructuredText
UI/
├── ui.h / ui.c            对外接口 + 生命周期 + 事件队列
├── ui_disp.c              flush_cb / 触摸 read_cb / 渲染缓冲 / 资源加载（挂载 storage, lv_fs 'F'）
├── ui_theme.c             配色常量 / 样式 / 字体引用（字体已编译进固件）
├── ui_statusbar.c         状态栏（轮询 lv_timer）
├── ui_tabbar.c            Tab 栏与页面切换
├── scr_player.c           播放页（含 pending 态逻辑）
├── scr_playlist.c         本地音乐列表
├── scr_light.c            灯控页
├── scr_settings.c         设置页（源/网络/同步/语音/OTA/关于）
└── scr_wifi.c             扫描列表 + 密码键盘
```

### 7.3 句柄/资源缺失降级矩阵

| 缺失项 | 播放页 | 灯控页 | 设置页 | 状态栏 |
| :--- | :--- | :--- | :--- | :--- |
| `music == NULL` | "播放模块未就绪"，控制键禁用 | — | 源分区禁用 | BT 灰显 |
| `play_mode == NULL` | 源 chip 禁用 | — | 源分区禁用 | BT 灰显 |
| `light == NULL` | — | "灯控未启用" | — | — |
| `wifi == NULL` | — | — | 配网入口禁用 | WiFi 灰显 |
| `call_phone == NULL` | — | — | 语音分区"未启用" | 麦克风灰显 |
| storage 未挂载（分区镜像缺失/未烧录） | 字体/图标不受影响；图片回退字形/色块 | 灯图标回退字形 | storage 图片用占位 | 状态栏图标均内置 |
| sync_protocol 未 init | — | — | "未启用" | 同步图标灰显 |
| SD 未挂载（仅音乐） | UI 资源（字体/图片）不受影响 | 不受影响 | 不受影响 | 不受影响 |

------

## 8. UI 资源规范与容错（Flash storage 分区 + 固件编译字体）

> **资源策略**：UI 图片（3 张）随固件烧录进 Flash `storage` 分区（SPIFFS），中文字体/图标全部由 `lv_font_conv` 生成 C 数组**编译进固件**，SD 卡不参与 UI 资源。

### 8.1 资源目录（storage 分区，随固件烧录）

```reStructuredText
/storage/                         # SPIFFS 分区（挂载点 /storage，LVGL 盘符 'F'）
└── ui_img/                       # UI 图片（spiffs_create_partition_image 烧录）
    ├── Music_Album.png           专辑占位图（128×128，必需）
    ├── Light_On.png              灯控页大图标·点亮态（彩色）
    └── Light_Off.png             灯控页大图标·熄灭态（彩色）

字体不在文件系统：ui_font_cn_16（中文 16px，编译进固件）
图标字体：ui_icons_16 / ui_icons_24 + LVGL symbol（全部编译进固件/自带）
```

### 8.2 字体生成规范（lv_font_conv，C 数组编译进固件）

```bash
# 中文 16px：覆盖界面全部文案 + ASCII（--format lvgl，输出 C 数组）
npx lv_font_conv --font NotoSansSC-Regular.ttf --size 16 --bpp 4 \
  --range 0x20-0x7E --symbols "《界面全部文案汉字集》" \
  --format lvgl --no-compress -o ui_font_cn_16.c

# 自定义图标字体（灯泡/麦克风/单曲循环/同步；C 数组编译进固件）
npx lv_font_conv --font MaterialIcons-Regular.ttf --size 24 --bpp 4 \
  --range <灯泡码字>,<麦克风码字>,<单曲循环码字>,<同步码字> \
  --format lvgl -o ui_icons_24.c
```

- `--bpp 4`（抗锯齿）+ `--no-compress`（`LV_USE_FONT_COMPRESSED=0` 配套），生成 `const lv_font_t` 数组。
- 内置 symbol 字形随 Montserrat 字号启用（lv_conf 开 `LV_FONT_MONTSERRAT_16 / _24`），无需额外文件。
- 运行时零字体文件 IO（无 .bin、无 binfont、无 TTF）。

### 8.3 lv_fs 映射（只读 storage 分区图片）

- 自定义 LVGL 文件系统盘符 `'F'`（`ui_disp.c` 注册，VFS `stdio` 后端，根 `/storage`）；
- 代码中一律写 `"F:/ui_img/xxx.png"` → `/storage/ui_img/xxx.png`；
- 前置条件：`ui_hw_bringup()` 内 `esp_vfs_spiffs_register()` 挂载 `/storage` 成功后才允许加载图片（失败按 8.4 降级）。

### 8.4 资源缺失/异常容错矩阵

| 异常 | 字体 | 图标 | 播放页 | 列表页 | 配网/设置 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| storage 未挂载（分区镜缺失/未烧录） | 不受影响（编译进固件） | 字形图标全部内置不受影响；PNG 回退字形 | 正常（封面占位色块） | 正常 | 正常 |
| ui_img/*.png 缺失/损坏 | 不受影响 | 字形图标回退 | 封面内置色块占位 | 正常 | 正常 |
| SD 未挂载（仅音乐） | 不受影响 | 不受影响 | 正常（无本地播放） | 空态"SD 卡未挂载" | 正常 |
| SD 播放中拔卡 | 不受影响 | 不受影响 | `MUSIC_EVT_ERROR` → Toast + 停止态 | 空态 | 正常 |

- 图片在 `ui_init()` 内按需加载（失败逐项降级，不阻塞启动）；字体全程无文件 IO，SD 只负责音乐文件。
------

## 9. 上游接口补充建议（仅记录，未回写架构文档）

> 本章是 UI 设计过程中发现的**上游接口缺口**与配套建议。按约定本文档不修改《主音频节点软件构架分层设计.md》，是否采纳由你决定；未采纳前，UI 实现到对应功能时按"接口缺失 → 功能禁用"降级。

### 9.1 wifi_manager 扫描 API（配网扫描列表依赖）

现有定稿接口只有 `wifi_manager_set_sta()`，无扫描能力。建议在 3.4.2 补充：

```c
/* 配网扫描（UI 用；异步，内部走 esp_wifi_scan_start 阻塞式全信道扫描） */
esp_err_t wifi_manager_scan_start(wifi_manager_handle_t h);        /* 异步发起，立即返回 */
bool      wifi_manager_scan_is_done(wifi_manager_handle_t h);      /* UI 1s 轮询 */
esp_err_t wifi_manager_scan_get_results(wifi_manager_handle_t h,
                                        wifi_ap_record_t *aps,    /* 调用方提供数组 */
                                        uint16_t max, uint16_t *count); /* 按 RSSI 降序返回 */
```

**风险标注**：WiFi 扫描会跳信道，期间音频组播丢包；从节点网络中断 >500ms 即进入静音重连（架构文档第三阶段异常表）。**建议：扫描动作提示用户在播放间隙执行**（配网页已有固定文案提示）；若不可接受，备选方案是扫描前由 UI 先暂停播放、扫描后恢复（多一步交互，暂不推荐）。

### 9.2 MusicPlay 曲目元数据与进度（播放页显示依赖）

现有 `music_play_view_t` 仅含本地曲目结构 `music_track_t`（path/display_name/duration_ms），BT 源的 AVRCP 元数据（歌名/歌手/专辑/时长）与播放进度**没有到达 UI 的通道**（UI 又不得直接调 `bt_audio`）。建议二选一：

- **方案 A（推荐）**：`music_track_t` 扩展 `char artist[48]; char album[48];` 两个字段；MusicPlay 在 BT 源下维护"虚拟 track"——`display_name` 映射歌名、`artist/album` 取自 AVRCP、`duration_ms` 取自 `bt_audio_track_info_t`。UI 单一数据通道（`view->track`），无需感知音源差异；
- 方案 B：`music_play_view_t` 单独内嵌 BT 元数据字段，UI 按 source 分支取值（字段冗余，不推荐）。

进度补充建议（两方案通用）：新增

```c
esp_err_t music_play_get_position_ms(music_play_handle_t player, uint32_t *position_ms);
/* 本地源：转发 audio_decoder_get_position_ms()；BT 源：返回 ESP_ERR_NOT_SUPPORTED（UI 隐藏进度行） */
```

### 9.3 其他确认项（无需改接口）

- 蓝牙图标数据源用 `play_mode_get() == MUSIC_SOURCE_BT` 推断，UI 不查 `bt_audio_get_a2dp_info()`——与"UI 不直接调 bt_audio"边界一致；
- 音量对 BT 源即 AVRCP absolute volume，由 MusicPlay 内部转发 `bt_audio_set_volume()` 并广播 `SYNC_CMD_VOLUME`，UI 无感知。

------

## 10. 实现分期建议

| 阶段 | 内容 | 准出条件 | 依赖 |
| :--- | :--- | :--- | :--- |
| **M1 显示/触摸底座** | 自持 UI_Task + flush/read 适配、主题与内置字体（C 编译进固件）、图标字形资产（symbol 字号启用 + 自定义 iconfont 生成，见 3.3）、状态栏/Tab 栏骨架、三页静态布局（假数据）、页面切换 | 320×240 稳定刷屏 ≈30fps、触摸点击/滚动正常、storage 分区 PNG 加载与占位回退生效；`LCD_TOUCH_SELF_TEST=0` | LCD_Touch（已实现）、storage 分区（已实现，随固件烧录）；MusicPlay 等可为 NULL |
| **M2 播放链路** | 播放页真实数据、本地列表、音量/模式、pending 态、跑马灯 | 本地源完整闭环（选曲/播放/暂停/切曲/进度/音量）；BT 源标题与控制可用（AVRCP 回流） | MusicPlay / PlayMode 实现（+9.2 建议落地） |
| **M3 灯控 + 配网** | 灯控页三态与回执时序、扫描列表、密码键盘、配网两步流程 | 开灯→ack 落定 <50ms；断网重连图标正确；配网成功落盘 NVS | LightControl / wifi_manager 实现（+9.1 建议落地） |
| **M4 系统状态收尾** | 设置页同步/语音/OTA 分区、容错矩阵全量验证、Toast 体系 | SD 拔卡降级、各句柄 NULL 降级逐项通过；OTA 进度展示正确 | sync_protocol / CallPhone / OTA |

------

## 11. 风险与对策

| 风险 | 影响 | 对策 |
| :--- | :--- | :--- |
| SPI2 上 LCD 刷屏与 SD 解码读争用 | UI 卡顿 / 解码抖动 | 部分刷新 + 30fps 限帧 + 大图一次缓存（2.5）；音频侧由 sync_protocol ingress 缓冲吸收 |
| 内部 RAM 紧张（BT 栈 + WiFi 栈 + 音频 FIFO + LVGL） | 分配失败 | 渲染缓冲 25.6KB DMA 内部 RAM 之外，LVGL 堆全走 PSRAM；字体位图不驻留内部 RAM |
| BT 源无 position 接口 | 播放页进度缺失 | 本期隐藏进度行；不虚构接口（5.2） |
| WiFi 扫描打断音频组播 | 从节点短暂静音 | 配网页固定提示 + 建议播放间隙执行（9.1） |
| 触摸为 20ms 轮询快照（无中断） | 快速滑动跟手性一般 | 本期不做滑动手势；列表滚动用 LVGL 内建滚动实测调优 |
| esp-sr 许可证不确定（CallPhone 可能换唤醒方案） | 语音状态来源变化 | UI 只依赖 `call_phone_get_state()`，底层替换不影响 UI |
| AVRCP 回流慢（部分手机 >500ms） | 播放键等待态偏长 | pending 态 500ms 阈值 + `view->pending_cmd` 双信号；不做乐观提交 |
| 编译字体字符集遗漏 | 个别字显示为方块 | 文案表与字符集统一维护；新增文字需重新生成 C 数组并更新固件 |
