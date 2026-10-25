> data：2026-10-20

## UI上按切换单曲循环，蓝牙播放还是不变（手机上还是一样，那播放还是按照手机那样的来）——> 已解决

**现象**：SD 卡播放时按 UI 模式键切单曲循环，曲末重播本曲，正常；切到蓝牙源再按同一个键，图标变成 🔂，但曲末仍按手机的列表循环播下一首，**手机上也看不到任何变化**。用户感知就是"按了没用，而且图标还骗人"。

### Bug 分析

#### 调用链：只走了一半

| 步骤 | 位置 | 实际动作 |
| --- | --- | --- |
| 1 | [scr_player.c](file:///F:/EmbeddedProject/MusicPlaySyncSpeakers/Play_Master/components/App/UI/scr_player.c) `mode_click_cb()` | `music_play_set_mode(cfg->music, next)` |
| 2 | [music_play.c](file:///F:/EmbeddedProject/MusicPlaySyncSpeakers/Play_Master/components/App/MusicPlay/music_play.c) `Music_Set_Mode_Msg()` | 只做三件事：写 `view.mode`、写 `node_role.play_mode`（NVS）、发 `MUSIC_EVT_PLAY_MODE_CHANGED` |
| 3 | `scr_player_on_music_event()` | 图标翻转 🔁 → 🔂 |
| 4 | **无人** | ← 缺口就在这里 |

关键证据是第 3 步之后，**全工程唯一消费模式值的地方只有一处**：

```c
// music_play.c - Music_On_Local_Eof()：仅本地解码器报 EOF 时进入
if (h->local_mode == MUSIC_PLAY_MODE_SINGLE_LOOP)
{
    next = h->view.track_index;              // 重播本曲
}
else
{
    next = (h->view.track_index + 1) % h->track_count;
}
```

蓝牙源走的是完全另一条路径 `Music_Handle_Bt_Event()`（`STREAM_START` / `STREAM_STOP` / `PLAY_STATE` / `TRACK_CHANGED` / `TRACK_INFO`），**从头到尾没有一处读模式**，也不存在蓝牙侧的"EOF"事件；而 `bt_audio_send_ctrl_cmd()` 只支持 play / pause / prev / next / 音量上下透传，`bt_audio.h` 里**根本不存在设置循环模式的接口**。

结论：设备端唯一发生的事是自家枚举和图标变了，对手机零影响。曲末继续下一首，是手机播放器按它自己的设置执行的，设备只是个音频出口。

#### 架构约束

A2DP Sink 不持有播放列表、没有选曲权，本地一个 enum 对手机没有任何约束力。想影响手机只有两条路：**协议层 AVRCP 下发**，或**应用层事后纠正**（检测到自动切歌就把播放位置拉回去）。这也决定了修复只能是这两条路的组合。

#### 顺带发现的第二个 bug：状态漂移

原 `Music_Set_Mode_Msg()` 在蓝牙源下**同样**把模式写进了 `node_role.play_mode`（NVS）。重启后 UI 按 NVS 显示"单曲循环"，但手机侧实际是列表循环 —— 同一个 bug 的另一半，改的时候要一起收掉。

### 方案对比

| 方案 | 做法 | 结论 |
| --- | --- | --- |
| A 只做 UI 降级 | 蓝牙源下按钮置灰 + 提示"由手机控制" | 消除假反馈，但用户诉求没实现 |
| B 只发 AVRCP | `SetPlayerApplicationSettingValue(REPEAT)` | 协议层正解，但手机作 A2DP source 时多数不实现 AVRCP TG 角色，可能石沉大海 |
| C 纯软件守卫 | 曲末收到 `TRACK_CHANGE` → 发 `PREV` 拉回原曲 | 100% 设备可控，但曲末会漏一小段前奏 |
| **D 组合（采纳）** | B + C + 状态按源区分 | 手机支持就交给手机（无漏音），不支持则设备兜底，两者不冲突 |

### 解决方案

#### 一、bt_audio：新增 AVRCP repeat 下发

`bt_audio.h` / `bt_audio.c` 新增接口：

```c
/**
 * @brief 向手机下发播放器的重复模式（AVRCP SetPlayerApplicationSettingValue）
 *
 * best-effort 能力：手机作为 A2DP source 时不一定实现 AVRCP TG 角色，
 * 命令可能被忽略或拒绝；bluedroid 的 CT 侧也没有该命令的响应事件，
 * 因此调用成功只代表帧已发出，不代表手机已生效。
 */
esp_err_t bt_audio_set_repeat_mode(bt_audio_handle_t audio, bool single);
```

实现：

```c
if (!s_bt_audio.avrc_connected)
{
    return ESP_ERR_INVALID_STATE;   /* AVRCP 未连接，无法向手机下发 */
}
err = esp_avrc_ct_send_set_player_value_cmd(bt_audio_avrc_alloc_tl(),
                                            ESP_AVRC_PS_REPEAT_MODE,
                                            single ? 0x01u : 0x00u);
```

**这里有一个必须绕开的坑（否则改了等于没改）**：

| | 取值 |
| --- | --- |
| IDF `esp_avrc_api.h` 的 `esp_avrc_ps_rpt_value_ids_t` | `OFF=0x1` / `SINGLE=0x2` / `GROUP=0x3` |
| AVRCP 规范 | `0x00=OFF` / `0x01=SINGLE` / `0x02=ALL TRACK` |

`avrc_bld_ct.c` 把 `attr_val` **原样打包，不做任何重映射**。所以按枚举名传 `ESP_AVRC_PS_RPT_SINGLE(0x02)`，实际发出去的是"全列表循环"，现象依然是"设了等于没设"。必须直接传规范字面值 `0x01` / `0x00`。

另一个限制：bluedroid 的 CT 侧**没有** `set_player_setting` 的响应事件，拿不到回执，手机是否生效只能从行为反推（曲末还会不会来 `TRACK_CHANGE`）。

#### 二、music_play：模式按源各存一份

```c
/* local_mode：本地列表播放，由本模块状态机执行，持久化到 node_role；
 * bt_mode：手机侧播放器，只能 AVRCP 请求 + 守卫兜底纠正。手机的真实循环
 *          状态既不可回读也不可枚举，故只存会话内存 —— 持久化会让设备重启后
 *          显示"单曲循环"而手机实际是列表循环，制造新的假状态。 */
music_play_mode_t local_mode;
music_play_mode_t bt_mode;
```

- `Music_Set_Mode_Msg()` 按 `view.source` 分派：本地源写 `local_mode` + NVS；蓝牙源只写 `bt_mode` 并调 `Music_Bt_Mode_Apply()`（AVRCP 请求 + 重置守卫）。
- `Music_On_Local_Eof()` 改读 `local_mode`，不再读对外副本 `view.mode`。
- `Music_Switch_Source_Msg()` 切源后调 `Music_Mode_To_View()` 刷新 `view.mode` / `view.mode_owner`；进入蓝牙源且为单曲循环时**重新请求一次**（重连后手机侧可能已复位）。
- 启动时 `local_mode` 从 NVS 恢复，`bt_mode` 固定回 `SEQUENTIAL`。

```c
static void Music_Set_Mode_Msg(music_play_handle_t h, music_play_mode_t mode)
{
    if (h->view.source == MUSIC_SOURCE_BT)
    {
        h->bt_mode = mode;
        Music_Bt_Mode_Apply(h, mode);      // AVRCP 请求，失败也不回滚
    }
    else
    {
        h->local_mode = mode;
        if (node_role_get(&nr) == ESP_OK) { nr.play_mode = (uint8_t)mode; node_role_set(&nr); }
    }
    Music_Mode_To_View(h);                 // 只把当前源那一份落到 view
    Music_Emit(h, MUSIC_EVT_PLAY_MODE_CHANGED);
    Music_Publish_View(h);
}
```

两份模式互不覆盖：蓝牙下切单曲循环，回到 SD 卡仍是原来的本地模式。

#### 三、设备侧守卫：手机不支持 repeat 时兜底

`Music_Bt_Guard_Tick()` 随 `MusicPlay_Task` 的 50ms 拍运行，作用有三：维护"曲尾窗口"、到点解除静音、清理过期的本机切歌标记。

```text
每 500ms 查一次 bt_audio_get_position_ms() + bt_track.duration_ms
    └─ pos + 2000ms >= dur  →  bt_guard_armed = true（进入曲尾窗口）
    └─ 拿不到位置/时长（dur==0）→ 一律不武装

收到 BT_AUDIO_EVT_TRACK_CHANGED → Music_Bt_Guard_On_Track_Change()
    ├─ bt_self_switch  →  本机命令的回执，放行
    ├─ 不在曲尾窗口    →  用户手动切歌，放行（日志 manual track change, pass）
    └─ 在曲尾窗口      →  手机刚自动切了下一首：
                          1. 静音（功放 0 + 广播从节点 SYNC_CMD_VOLUME=0）
                          2. 记录 self_switch（回原曲的那次 TRACK_CHANGE 是回执）
                          3. 发 BT_AUDIO_CMD_PREV
```

为什么 `PREV` 能回到原曲：新曲刚开始播放位置 < 3s，安卓把 `PREV` 解释为"回上一首"，正好等于刚播完那首从头重播。

**静音是自适应的**，不是固定时长：

- 触发瞬间按 `MP_BT_GUARD_MUTE_MAX_MS`（2500ms）上限压住，防止回执丢失导致永久静音；
- 收到回执（`TRACK_CHANGE` 回来）后把到期时间改成 `now + 500ms`，只盖住 A2DP FIFO 里残留的下一首尾音，**不吃原曲开头**；
- 静音只动功放和从节点广播，**不碰 `view.volume` 和 NVS**，UI 音量条不动，解除时按 `view.volume` 还原（期间用户调音量以最新值为准）。

`Music_Cmd_BT()` 的 `NEXT` / `PREV` 会先调 `Music_Bt_Guard_Note_Self()`，所以单曲循环下用户手动切歌不会被守卫拽回来；而手动切到的新曲播完后，守卫的 `PREV` 正好把该曲拉回重播，语义自洽。

#### 四、UI：不再承诺设备执行

`music_play_view_t` 增加 `mode_owner`：

```c
typedef enum {
    MUSIC_MODE_OWNER_LOCAL = 0,  /* 设备状态机直接执行 */
    MUSIC_MODE_OWNER_BT,         /* AVRCP 尽力下发 + 设备侧守卫兜底 */
} music_mode_owner_t;
```

[scr_player.c](file:///F:/EmbeddedProject/MusicPlaySyncSpeakers/Play_Master/components/App/UI/scr_player.c) 三处配套：

1. 蓝牙源副标题从"蓝牙音频"改为"**蓝牙音频 · 循环跟随手机**"，明确归属；
2. 模式图标在 `mode_owner == MUSIC_MODE_OWNER_BT` 时改用次色 `UI_COLOR_TEXT_SUB`，与本地源"设备自己执行"在视觉上区分；
3. `mode_click_cb()` 的日志区分两种语义（"蓝牙源（请手机 + 设备兜底）" / "本地源"），便于现场排查。

### 参数

| 宏 | 值 | 含义 |
| --- | --- | --- |
| `MP_BT_GUARD_TAIL_MS` | 2000 | 曲尾窗口，多久算"即将自然播完" |
| `MP_BT_GUARD_SELF_MS` | 3000 | 本机切歌后忽略 `TRACK_CHANGE` 的时长 |
| `MP_BT_GUARD_POS_POLL_MS` | 500 | 查询播放位置的限频周期 |
| `MP_BT_GUARD_MUTE_MAX_MS` | 2500 | 静音上限（回执丢失的保险） |
| `MP_BT_GUARD_RELEASE_MS` | 500 | 收到回执后再压的静音时长 |
| `MP_BT_GUARD_MUTE_EN` | 1 | 0 = 不静音，直接接受前奏泄漏 |

### 已知限制

- 手机不上报 `PLAYING_TIME`（`duration_ms == 0`）时守卫不武装 ⇒ 只能指望手机支持 AVRCP repeat；
- 手机不上报 `TRACK_CHANGE` 时守卫同样失效（部分播客/视频 App）；
- 手机侧自己改循环模式，设备无法感知（AVRCP 没有 repeat 状态通知），UI 与实际仍可能不一致；
- 兜底本质是"曲末回退一次"，会有一次极短的静音/停顿，**不是无缝循环**；
- `bt_mode` 不落 NVS：重启后蓝牙模式回到"跟随手机"，属预期行为。

### 验证

代码已增量编译通过（`Project build complete`），真机待验证：

1. **SD 卡回归**：切单曲循环 → 曲末重播本曲；切顺序 → 顺延下一首；
2. **蓝牙 + 手机播放器**：切单曲循环，日志应出现
   `bt mode -> SINGLE_LOOP, AVRCP repeat request sent`
   （这行只证明帧发出去了，不代表手机生效）；曲末分两种：
   - 手机支持 → 手机自己循环，日志**没有** `guard: auto advance detected`（最理想，无漏音）；
   - 手机不支持 → 日志出现 `guard: auto advance detected, PREV to replay`，可闻一次极短静音；
3. **蓝牙下手动按 ⏭** → 日志 `guard: manual track change, pass`，不被拽回；该曲播完 → 回到该曲重播；
4. **模式互不继承**：蓝牙切单曲循环后切回 SD 卡 → 本地模式保持原设置；
5. **重启**：本地模式按 NVS 恢复，蓝牙模式回到"跟随手机"；
6. 若握手失败（AVRCP 未连），日志为
   `AVRCP repeat request failed: ... (device-side guard still armed)`，此时纯靠守卫兜底。



## 本地播放的时候，暂停后再播放，会重新从歌曲的最前面播放->解决

### Bug 分析

#### 调用链路

1. UI 的播放/暂停按钮在 [scr_player.c](file:///F:/EmbeddedProject/MusicPlaySyncSpeakers/Play_Master/components/App/UI/scr_player.c) 中按下时，发送的是 **MUSIC_CMD_TOGGLE**：

```c
// scr_player.c - play_click_cb()
ret = music_play_command(cfg->music, MUSIC_CMD_TOGGLE);
```

1. 消息到达 [music_play.c](file:///F:/EmbeddedProject/MusicPlaySyncSpeakers/Play_Master/components/App/MusicPlay/music_play.c) 的 `Music_Cmd_Local()`，进入 TOGGLE 分支：

```c
case MUSIC_CMD_TOGGLE:
    if (h->view.state == MUSIC_STATE_PLAYING)
    {
        Music_Pause_Local(h);      // ✅ 播放中 → 暂停，位置保持
    }
    else if (h->track_count > 0)
    {
        uint16_t idx = (h->view.track_index < h->track_count)
                           ? h->view.track_index : 0;
        Music_Start_Local_Track(h, idx);  // ❌ 这里！重新从文件头开始
    }
```

1. **问题根源**：TOGGLE 只有两个分支——`PLAYING` 和「其他」。暂停时状态是 `MUSIC_STATE_PAUSED`，不等于 `PLAYING`，直接落入 `else if` 分支，调用 `Music_Start_Local_Track()`。该函数内部先 `Music_Close_Local(h)` 关闭旧解码器（丢掉所有解码状态和文件位置），再 `audio_decoder_open` 重新打开文件——**自然从头播放**。
2. 对比：`MUSIC_CMD_PLAY` / `MUSIC_CMD_RESUME` 的处理器是正确的——它先判断 `PAUSED` 状态，走 `Music_Resume_Local()` 从暂停点继续。但 UI 根本没有发送这两个命令，只发送 TOGGLE。

#### 解决方案

**改动位置**：[music_play.c](file:///F:/EmbeddedProject/MusicPlaySyncSpeakers/Play_Master/components/App/MusicPlay/music_play.c) 的 `Music_Cmd_Local()` 中 TOGGLE case。

**思路**：在 TOGGLE 中增加对 `PAUSED` 状态的判断，让暂停→恢复走 `Music_Resume_Local`。

**伪代码**：

```c
case MUSIC_CMD_TOGGLE:
    if (h->view.state == MUSIC_STATE_PLAYING)
    {
        Music_Pause_Local(h);
    }
    else if (h->view.state == MUSIC_STATE_PAUSED)
    {
        Music_Resume_Local(h);            // ← 新增：暂停态→恢复
    }
    else if (h->track_count > 0)
    {
        // IDLE/STOPPED/ERROR 态：重新起播
        uint16_t idx = ...;
        Music_Start_Local_Track(h, idx);
    }
```

**优点**：

- 改动最小，一行判断
- 不改变 UI 层和其他模块的行为
- 解码器位置自然保持（pause 时解码任务内部状态 `in_consumed`、`mp3` 解码器核心状态、`cur_samples` 等全部未丢失）

**缺点**：

- 依赖 `audio_decoder` 暂停后内部状态确实没有丢失（目前代码保证了这一点：`audio_decoder_pause` 只发送队列消息让解码任务跳出循环，不清理任何缓冲区状态）



## 连接蓝牙后，在UI上切换为本地播放，还是会继续蓝牙播放

这个问题不算问题！！！本来就这样设计的



## 断掉蓝牙后切换为本地播放，本地播放的声音很卡---->解决

本地解码速度只有实时播放速度的约 **0.91 倍**，因此会持续欠载、听起来卡顿；主要修复是把 CPU 频率从 **160 MHz 提到 240 MHz**。同时，解码器允许更快地解码以补足缓冲，同步播放的 PCM 环形缓冲在播放时提供最多 **30 ms 背压**，用来缓冲调度或读卡抖动。



## UI-wifi图标状态没切换---->解决

​	没有实时的判断wifi有没有连接上然后切换图标状态



## UI进行切换的时候播放sd卡音乐真的卡，不过好像没什么办法可以避免，共用一条总线，改不了，也不够引脚用了，无法再弄一个总线，只能无限性能调优。

## 当LCD屏幕刷新不一样的帧的时候就会导致sd卡音乐播放卡顿，然后就会加速，追回来之后才变为正常速度

这个在UI优化那有具体优化





## sd卡一首歌快播放完的时候就会一直嗡嗡嗡响，直到下一首歌解析到；经过测试，他有时候播放了一会也会这样，然后我滑动音量设置后马上又恢复正常了。

The user wants me to write a summary document about the bug cause and our fixes. Let me write this in their language (Chinese) since they've been communicating in Chinese.

The user wants a clear write-up of:
1. Bug root cause
2. What we changed and why

I should write this in a structured way. Let me provide it as a response, not create a file (the user didn't ask me to create a file).
### Bug 分析与修复报告

---

#### 一、Bug 现象

| 场景                | 表现                                   |
| ------------------- | -------------------------------------- |
| SD 卡一首歌快播完时 | 一直**嗡嗡嗡**响，直到下一首歌解码完成 |
| 播放中途偶发        | 播放一段时间后突然嗡嗡嗡响             |
| 滑动音量滑块        | **立刻恢复正常**                       |

---

#### 二、根因分析

##### 音频数据链路

```
SD卡 → MP3解码器 → PCM回调 → 入口环缓冲
     → SyncTx_Task(攒帧→本地延迟队列)
     → SyncLocalPlay_Task(到点出队→单声道转立体声)
     → Amplifier_Play_Buffer(软件音量衰减)
     → i2s_channel_write → I2S DMA → NS4168功放 → 扬声器
```

##### 根因 1：歌曲切换间隙 —— I2S DMA 缓冲区放空，数据线浮空

当解码器读到文件末尾（EOF）时，[`Music_On_Local_Eof()`](file:///f:/EmbeddedProject/MusicPlaySyncSpeakers/Play_Master/components/App/MusicPlay/music_play.c#L509) 立即调用 `Sync_Stop()`。`sync_protocol_master_stop()` 做了三件事：

1. **清空延迟队列**
2. **复位入口环缓冲**
3. **设 `started = false`** → SyncLocalPlay_Task 停止出队写 I2S

但此时 **I2S DMA 里还有约 46ms 的残留数据**（4 个描述符 × 512 帧 × 4 字节 = 8KB）。这 46ms 播完后，DMA 描述符全部耗尽，TX FIFO 变空。

ESP32 上 I2S 外设的 BCLK/WS 时钟由 `i2s_channel_enable()` 启动后**持续运行**，不依赖是否有数据。当 FIFO 为空时，数据引脚（GPIO13）处于不确定状态，NS4168 功放将其放大输出，表现为 **嗡嗡嗡底噪**。

这个噪声持续到下一首歌 `audio_decoder_open()` → 首帧探测解码 → `Sync_Start()` → PCM 重新填满 DMA —— 整个过程可能 100~500ms。

##### 根因 2：播放中途偶发 —— ESP32 legacy DMA 半卡不死

ESP32 的 legacy DMA（非 GDMA）在长时间运行后，DMA 描述符环可能进入"半卡死"状态：不产生 EOF 但也不完全停止，写入耗时异常偏高（接近超时边缘），输出端产生噪声。

原代码虽有超时恢复机制（`ESP_ERR_TIMEOUT` 触发 `disable→enable→重试`），但只在 DMA **完全卡死**（500ms 超时）时才生效。"半卡不死"状态不会被检测到。

##### 根因 3：为什么滑动音量就恢复？

滑动音量时 UI 密集发送 `MUSIC_CMD_VOLUME_UP/DOWN`，两个间接效应：
- 歌曲切换间隙：处理音量命令的过程中，下一首歌的解码已就绪并开始灌入 PCM
- DMA 半卡死：密集的命令处理改变了任务调度时序，间接触发 DMA 恢复

---

#### 三、修复方案

##### 方案一：SyncLocalPlay_Task 空队列时喂静音帧

**修改文件**：[sync_protocol.c](file:///f:/EmbeddedProject/MusicPlaySyncSpeakers/Play_Master/components/middlewares/sync_protocol/sync_protocol.c)

**核心思路**：当延迟队列为空时，不再干睡，而是写入一帧全零 PCM（静音帧）到 I2S。这样 I2S DMA **永远有有效数据**，不会出现浮空噪声。

**改动 1：STOPPED / PAUSED 状态**（原第 630-634 行）

```c
// 修复前：
if (!s_master.started || s_master.state == SYNC_STATE_PAUSED)
{
    vTaskDelay(pdMS_TO_TICKS(5));  // 干睡 → DMA 放空 → 嗡嗡嗡
    continue;
}

// 修复后：
if (!s_master.started || s_master.state == SYNC_STATE_PAUSED)
{
    if (s_master.play_buf != NULL)
    {
        memset(s_master.play_buf, 0, SYNC_FRAME_MAX_SAMPLES * 2 * sizeof(int16_t));
        Amplifier_Play_Buffer((const uint8_t *)s_master.play_buf,
                              SYNC_FRAME_MAX_SAMPLES * 4, NULL, 100);
    }
    vTaskDelay(pdMS_TO_TICKS(15));  // 每 15ms 喂一帧静音，匹配帧率
    continue;
}
```

**改动 2：PLAYING 状态下空队列**（原第 643-649 行）

```c
// 修复前：
if (cnt == 0)
{
    xSemaphoreGive(s_lock);
    vTaskDelay(pdMS_TO_TICKS(2));  // 干睡 → DMA 欠载
    continue;
}

// 修复后：
if (cnt == 0)
{
    xSemaphoreGive(s_lock);
    if (s_master.play_buf != NULL)
    {
        memset(s_master.play_buf, 0, SYNC_FRAME_MAX_SAMPLES * 2 * sizeof(int16_t));
        Amplifier_Play_Buffer((const uint8_t *)s_master.play_buf,
                              SYNC_FRAME_MAX_SAMPLES * 4, NULL, 100);
    }
    vTaskDelay(pdMS_TO_TICKS(15));  // 喂静音帧兜底
    continue;
}
```

静音帧规格：`700 采样 × 2 声道 × 2 字节 = 2800 字节`，在 44.1kHz 下覆盖约 **15.8ms**，与发送帧率精确匹配。

---

##### 方案四：I2S 硬件层 DMA 健康监控 + 主动恢复

**修改文件**：[i2s_driver.c](file:///f:/EmbeddedProject/MusicPlaySyncSpeakers/Play_Master/components/bsp/i2s_driver/i2s_driver.c)

**核心思路**：在 `i2s_bus_phy_write()` 中全程监控每次写入的耗时。当连续多次写入耗时异常偏高（DMA 半卡不死），**在它彻底卡死之前主动 flush 恢复**。

**改动概要：**

| 位置       | 内容                                                         |
| ---------- | ------------------------------------------------------------ |
| 头文件     | 新增 `#include "esp_timer.h"`                                |
| 宏定义     | `I2S_DMA_HEALTH_SLOW_THRESHOLD_MS = 80`（写入 >80ms 视为异常）、`I2S_DMA_HEALTH_MAX_CONSECUTIVE = 3`（连续 3 次触发）、`I2S_DMA_HEALTH_RECOVERY_COOLDOWN_US = 5s`（恢复冷却） |
| struct     | 新增 `dma_health_bad_count`、`dma_last_recovery_us` 两个监控字段 |
| write 函数 | 注入健康检测状态机                                           |

**健康检测状态机：**

```
i2s_channel_write() 调用:
  │
  ├─ 成功 + 耗时 ≤80ms → counter=0           ✅ 健康
  ├─ 成功 + 耗时 >80ms → counter++           ⚠️ 疑似异常
  │     └─ counter ≥3 + 冷却已过 → flush DMA  🔧 主动恢复
  └─ 超时(ESP_ERR_TIMEOUT) → 走原有超时恢复   ❌ 已卡死
```

**为什么是 80ms？** 正常写入 2800 字节约需 16ms，DMA 缓冲深 46ms。超过 80ms 说明 DMA 描述符回收明显滞后，已处于不健康状态。

---

#### 四、协同效果

```
歌曲切换间隙:
  Stop → 方案一喂静音帧(15ms/帧) → I2S输出纯静音 → 下一首就绪 → 无缝切回

播放中途DMA半卡:
  写入变慢 → 方案四检测3次 → 主动flush DMA → 恢复健康

播放中途DMA全卡:
  写入超时 → 方案四记录 + 原有超时恢复 → 重启DMA通道
```



## UI配网问题--->已经解决，

解决方案在note





## sd卡播放的时候UI显示的播放时长不对

**现象**：UI显示3:02，实际3:09，差~7秒

**根因1**：probe阶段首帧指针定位错误

- `frame_start = in_buf + in_consumed - frame_bytes`，漏了minimp3的`frame_offset`
- 有中文ID3v2标签的文件，minimp3会跳过标签字节找sync word，`frame_bytes`含跳过量
- 不加`frame_offset`，指针落到ID3v2中文标签上 → Xing头解析失败 → fallback到`fsize×8/bitrate`估算
- 修改：`audio_decoder.c` — `frame_start + frame_offset`，`frame_size - frame_offset`

**根因2**：Xing头`total_frames`由编码器估写偏小（本例少3.7%）

- 修改：`audio_decoder.c` — 读Xing的`audio_bytes`字段，与`fsize - frame_offset`交叉校验。偏差>3%按比例修正：`duration_ms × (fsize - frame_offset) / audio_bytes`





## UI显示系统版本问题

**Bug 原因**

`OTA_Update_GetLocalVersion()` 内部读的是 `esp_app_desc_t.version`，这个值由 ESP-IDF 构建系统的 `PROJECT_VER` 决定。`CMakeLists.txt` 里写的是 `set(VERSION 0.1)`，但 ESP-IDF 只会认 `project(Play_Master VERSION 0.1)` 这种写法。单独 `set(VERSION 0.1)` 不生效，`PROJECT_VER` 为空，构建系统回退到 `git describe` 输出 commit hash（如 `754a35c-dirty`），所以显示 git hash 而非 `0.1`。

**解决办法**

绕开 ESP-IDF 的 `PROJECT_VER`，用 CMake 的 `file(WRITE)` 直接把 `${VERSION}` 写进一个头文件：

1. `CMakeLists.txt` 加一行 `file(WRITE "${CMAKE_SOURCE_DIR}/components/App/UI/fw_version.h" "#define FW_VERSION \"${VERSION}\"")`
2. `scr_settings.c` 里 `#include "fw_version.h"`，所有版本显示直接用 `FW_VERSION` 宏

`FW_VERSION` 是编译期字符串字面量，不经过任何运行时读取，不受 git describe 影响。改 `CMakeLists.txt` 的 `VERSION` 后重新 CMake 即可同步。





## 嗡嗡声复发（2026-10-23）：喂静音的节拍错了 + 两个状态根本没喂

**现象**：和上面「sd 卡一首歌快播完就一直嗡嗡嗡」一模一样 —— 播放一段时间后有概率持续嗡嗡，暂停再播放才恢复。

**先排除一件事**：之前的修复并没有被覆盖。`git diff 2137447 HEAD` 对 `i2s_driver.c` / `music_play.c` / `audio_decoder.c` 为空，方案一的静音帧、方案四的 DMA 健康监控、播放任务优先级 12 都还在 HEAD 里。问题出在方案一本身实现得不完整，所以「有修复」和「还会响」同时成立。

### 根因

1. **静音喂得太慢，只有实时的一半**

   方案一写的是「写一帧静音（约 15.8ms）+ 再 `vTaskDelay(15ms)`」。但 `Amplifier_Play_Buffer()` 在 DMA 满时**本身就会阻塞约一帧**，再睡 15ms 等于每 30.9ms 才喂 15.8ms 的数据。

   后果：DMA 缓冲（4×512 帧 ≈ 46ms）先被抽干；抽干后写操作不再阻塞，又开始快速回填。于是「空一阵、响一阵」抖动。换歌、解码卡顿等间隙稍长，就表现为持续嗡嗡。这就是为什么只在「播了一段时间/快播完」时出现——需要先等一次较长的空档。

   正确做法：**把阻塞写本身当节拍源**。写满一帧就不再睡；只有写返回得很快（DMA 有余量）时才让出 1 tick。

2. **PREPARING 状态一帧数据都不写**

   播放任务在「攒够 D 时长（200ms）才允许出声」的阶段只 `vTaskDelay(2ms)`，不写 I2S。每首歌开头的开卡/探测/预缓冲动辄几百 ms，DMA 46ms 就放空了 —— 所以每次切歌都会响一段。

3. **从节点在 preroll 阶段同样什么都不写**

   从机 I2S 在 `Amplifier_Init()` 后就一直使能，BCLK/WS 持续运行。`ua_play_task` 在「还没攒够 preroll / 断流后重新等」的分支里只 `vTaskDelay(20ms)`，DMA 放空 → 数据脚浮空 → NS4168 放大成持续嗡嗡。主节点暂停、换歌、从机上电后主节点还没发流时都会触发。这一条是「从机侧一直噪音」的根因。

### 修复

- `Play_Master/components/middlewares/sync_protocol/sync_protocol.c`
  - 新增 `sync_feed_silence()`：写 700 采样 × 2 声道静音；写耗时 ≥ 半帧就视为已被 DMA 限速，不再额外延时；否则 `vTaskDelay(1)`。
  - 三处调用：`!started / PAUSED`、`delay_count == 0`、`PREPARING` 攒缓冲阶段。
- `Play_Slave/components/middlewares/udp_audio/udp_audio.c`
  - 新增 `ua_write_silence()`：写 441 采样（10ms）静音，节拍策略与主节点一致。
  - `ua_try_start()` 失败时改为喂静音，替代原来的 `vTaskDelay(20ms)`。

两边的静音帧都与正常音频帧同尺寸（主 700 帧立体声、从 441 采样单声道），所以喂入速率与播放速率严格一致，不会改变已调好的主从同步余量。

### 验证

两工程均已用 IDF 6.0.3 编译通过（`Project build complete`）。上板重点看四个场景：

1. 单曲循环播 30 分钟：切歌间隙应完全无声，不再嗡嗡；
2. 播放中暂停 10 秒再恢复：从机不应嗡嗡；
3. 主节点暂停、从机单独上电：从机应保持安静；
4. 串口观察主节点 `state -> PLAYING` 前后的输出，从机 `level/offset` 不因喂静音发生跳变。

### 复盘：这一轮没解决，因为根因不在欠载

2026-10-23 实测仍然会复发，说明「DMA 放空 → 数据脚浮空」不是这次持续噪音的根因（而且 IDF 6.0.3 的 `auto_clear_after_cb` 会在每块 DMA 发完后把缓冲清零，欠载本来就只会输出 0）。真正根因见下一节。

---

## 嗡嗡声二次复发（2026-10-23）：入口环写进半个采样，整条流永久错位

**现象**：播放一段时间后有概率持续噪音（嗡嗡/沙沙），主节点和从节点的扬声器都会响；暂停→播放（或换歌）立刻恢复正常，之后可能再过一段时间复发。

**为什么「暂停→播放」能治**：`sync_protocol_master_resume()` 会走 `sync_reset_stream()` → `xStreamBufferReset()`，把入口环连同帧化状态全部清空。只有这条路径能把错位的数据扔掉，所以表现为"只有重新暂停播放才能解决"。

### 根因：流缓冲的"部分写入"

音频数据进主节点 `sync_protocol` 的**入口环**是一条 FreeRTOS 流缓冲（`xStreamBufferCreateStatic(65536, ...)`），它是**字节流**——没有消息边界，也没有"按采样对齐"的概念：

```c
// 旧实现，同步协议入口
if (xStreamBufferSend(s_master.ingress, pcm, bytes, wait) != bytes)
    s_master.dropped_pkts++;
```

`xStreamBufferSend()` 带超时时，**超时到达后只写"当前空闲字节数"那么多**（部分写入），返回实际写入的字节数。而 FreeRTOS 计算空闲空间是：

```text
xSpace = xLength + xTail - xHead - 1
```

流缓冲永远保留 1 字节用来区分满/空。入口环长度 65536 是偶数，读写游标一直按整采样（2 或 4 字节）移动，所以 **xSpace 恒为奇数**。于是：

- 只要发生一次"带超时的部分写入"，写进去的字节数一定是奇数；
- 奇数个字节 = 半个 16bit 采样 → 环里之后所有采样的字节边界永久错开 1 字节；
- 消费端 `SyncTx_Task` 按 `int16_t` 拼采样，拼出来就是满量程随机噪声；
- 同一份错位数据又被组包单播给从节点，所以**主从两边同时响**。

什么时候会超时：解码器以约 4 倍实时速度往里灌（每块 minimp3 帧 2304 采样 = 4608 字节），入口环长期是满的，每次 `push_pcm` 都在等 30ms 背压窗口。只要 `SyncTx_Task`（优先级 11 / Core 0）被 WiFi、lwIP、UI 重绘等打断超过 30ms 一次——甚至只是跨核调度抖动导致这一轮没攒够 4608 字节——这次发送就退化为部分写入。**一次就够，之后永远错位**。这就是"播放一段时间后、有概率"的来源。

### 修复：生产端按整采样写入 + 消费端错位兜底

`Play_Master/components/middlewares/sync_protocol/sync_protocol.c`：

1. **`sync_protocol_master_push_pcm()` 改成按整采样发送**，不再依赖 `xStreamBufferSend` 的超时语义：
   - 用 `xStreamBufferSpacesAvailable()` 取空闲空间，向下取整到 `channels × 2` 字节的整数倍，只写整采样块；
   - 写满一个块后继续轮询（`vTaskDelay(1)`）直到整块送完或 30ms 背压窗口到期，到期只丢弃**整采样**的尾巴；
   - 新增 `s_push_lock` 生产者互斥：本地解码和蓝牙 A2DP 是两个生产者，空闲空间判定和写入必须原子，否则两者交错时仍可能写出非整采样字节；
   - 复位/换流后（`stream_gen` 变化）立即放弃本次剩余数据，旧通道的尾巴不会混进新流。
2. **`SyncTx_Task` 增加错位兜底**：从环里读出 `got % 采样字节数 != 0` 时，说明环里已经有半个采样。此时采样边界无法恢复，直接 `xStreamBufferReset()` 清空重来（最多丢 D=200ms 的数据），并打一条 `ingress misaligned` 日志。生产端修好后这条分支正常不会触发，留着是为了防止将来再引入同类写入时退化成"一直响到用户手动暂停"。

**为什么不在 `xStreamBufferSend` 外面简单判断 `sent % unit`**：部分写入发生之后数据已经进环了，没有办法"退回"；只能在写入之前就保证不写非整采样。

### 验证方法

- `Play_Master` 已用 IDF 6.0.3 编译通过（`Project build complete`）。
- 上板长播 30 分钟以上，串口不应出现 `ingress misaligned`（若出现说明还有别的写入路径没按整采样）。
- 从节点无需改动：它收的是带长度字段的独立 UDP 包，不存在字节流对齐问题；主节点的错位数据以前是"跟着包一起发过去"才让从机也响的。

---

## 修好同步播放后，主节点连蓝牙只有"吱吱吱"（2026-10-24）—— 已解决

**现象**：主从同步播放正常之后，主节点连手机放 A2DP，喇叭里只有满量程的"吱吱吱 / 蹦蹦蹦"，一个音符都没有。手机侧显示正常播放，日志里采样率 `44100Hz 2ch` 完全正确。

### 根因：A2DP FIFO 写进了半个采样，整条字节流永久错位

与上面入口环那个 bug **同一类**，只是发生在 `bt_audio` 自己的 PCM FIFO 上：

- `xStreamBufferSend(fifo, data, len, 0)` 在空间不足时是**部分写入**；
- 流缓冲空闲字节数 = `xLength - used - 1`，长度是偶数、每次写整帧 4 字节 ⇒ **空闲字节数恒为奇数**；
- 只要溢出过一次，就必然写进 1 或 3 个字节 = 半个 `int16`；此后所有采样边界永久偏移，拼出来就是满量程随机噪声；
- 只有断开重连（FIFO 重建）才会恢复 —— 所以表现为"一连上就一直吱吱"。

### 修复

| 位置 | 改动 |
| --- | --- |
| `bt_a2dp_data_cb` | 写入前把 `len` 和空闲空间**都向下取整到 4 字节**，只写整帧；宁可丢整帧，绝不写半帧 |
| `bt_audio_task` 读侧 | 收到 `bytes % 4 != 0` 直接 `xStreamBufferReset()` 自愈（生产端修好后不应触发） |
| FIFO | 24KB → 64KB（≈371ms），并加开流预灌 24KB |
| 任务 | 优先级 5 → 11、绑 core 1（原来和 WiFi/BT 控制器挤在 core 0） |
| 射频 | 开流置 `ESP_COEX_BT_ST_A2DP_STREAMING`，停流/断连清除 |

**为什么还改了优先级和 coex**：只修错位后能出声了，但会"卡"。蓝牙是实时生产者，优先级 5 会被下游压 15ms 以上；WiFi 每 15ms 给从机发包，与经典蓝牙抢同一个 2.4G 天线。这两条不修，声音干净但断断续续。

### 验证

`[btpcm]` 每秒一行：`samp/s ≈ 88200`、`peak` 在 2000~26000（真实音乐）、`resync=0`、`drop=0`。

---

## 播放中突然"卡死"（2026-10-24）—— 已解决

**现象**：放着放着突然没声音，**串口日志同时全部停止**，手机仍显示"已连接"并持续给主节点发音频。当时看门狗没开，第一判断是死锁。

### 先补观测，再说结论

"日志全停"本身不携带任何信息，所以先做三件事：

1. 打开 `CONFIG_ESP_TASK_WDT_EN`（5s，监控 IDLE0/IDLE1）；
2. 三个音频任务（`bt_audio_task` / `SyncTx_Task` / `SyncLocalPlay_Task`）订阅 TWDT；
3. 加独立心跳任务 `app_hb`（core0 / prio2 / 3s），只读单调计数器：

```text
[hb] up=.. reset=.. heap=.. psram=.. | a2dp_cb=.. beat=.. fifo=..B |
     sync st=.. dq=.. tx=.. drop=.. sil=.. | cp st=.. f/s=.. proc=../..us over=..
```

`a2dp_cb` + `beat` 两个计数器能把"没声音"唯一拆成三种：**两者都停** = 手机没送；**cb 涨 beat 停** = 蓝牙任务卡住；**cb 停 beat 涨** = 任务活着但没数据。`reset=` 还能区分"真死住"和"其实重启过"（主机重启后手机端往往还显示已连接几秒）。

### 根因：不是死锁，是 core1 被唤醒词检测吃满

WDT backtrace 每次都停在 `CallPhone_Listening() → afe_fetch() → wakenet9`，IDLE1 饿死。三处叠加：

1. **零阻塞空转**：`Microphone_Read_Pcm16` 读不到数据是**立刻返回 0**、不阻塞。LISTENING 分支原来什么都不等就让出，主循环退化成死循环，core1 被 100% 占满，IDLE1 一次都轮不到。
2. **档位过高**：AFE 用 `AFE_MODE_HIGH_PERF`，但 AEC/NS/VAD/AGC 全关、只跑唤醒词 —— 高精度档位零收益，却要多花近一倍 CPU。
3. **单帧超预算后不会自愈**：一帧 16ms 音频，处理一旦 ≥16ms，麦克风 FIFO 就持续积压 → 以后永远读得到 → 更不会阻塞。

音频链路（`SyncLocalPlay` prio12 也在 core1）因此失去全部抖动余量；而 `sync_play_task` 里还有一段**无上限忙等**（等 `play_at`），一旦延迟队列混进异常时间戳，就会把 core1 彻底锁死 —— 这才是"日志全停"的真正形态。

### 修复

- `call_phone.c`：`AFE_MODE_HIGH_PERF` → `AFE_MODE_LOW_COST`；`CallPhone_Listening` 返回 bool，读空时 `vTaskDelay(2ms)`；单帧处理 ≥ `frame_us` 主动让 1 tick（唤醒词按半帧率处理，"你好小智"跨 500ms 仍能命中）；新增 `call_phone_get_load()` 提供 `proc_avg/max_us`、`overrun_cnt`。
- `sync_protocol.c`：`play_at` 忙等加 10ms 硬上限；`SYNC_PLAY_WRITE_TIMEOUT_MS` 500 → 100；`sync_feed_silence()` 改为严格按 `frame_ms` 节拍补静音（原来写得快就立刻写下一帧，把 DMA 灌满静音，真数据回来还要先播完 46ms 静音）；`sync_reset_stream()` 加 `s_push_lock`；两个任务订阅 TWDT。
- `i2c_driver.c`：事务超时 100 → 10ms，并静默 `i2c.master` / `i2c.common`（触摸不通时每秒刷 20 行，把音频日志全冲掉）。
- `LCD_Touch.c`：硬件复位低电平 10 → 30ms，读失败指数退避 20 → 1000ms，删掉"I2C 恢复"日志。
- `app_main.c`：UI 优先级 12 → 10（LVGL 刷一整屏约 123ms，原来比 SyncTx(11) 还高）。

### 验证

正常播放时 `[hb]` 中 `proc` 的峰值应低于 `frame_us(16000)`、`over=0`、`cp f/s ≈ 62`。若 `over` 持续增长，说明 core1 仍然超载，下一步是在音乐播放期间把唤醒检测降频或直接暂停。

> 后续：`[hb]` 的 `cp st= / afe= / f/s= / proc= / over=` 字段已在 2026-10-25 随语音唤醒功能整体下线删除，见下一节。

---

## 播放 1~2 首后 INT WDT 仍未断根（2026-10-25）—— 已解决：砍掉语音唤醒/麦克风

**现象**：关掉 `CONFIG_BTDM_CTRL_HLI`、播放态暂停 AFE、打开 `CONFIG_I2C_ISR_IRAM_SAFE` 之后，崩溃频率从"开机几分钟"降到"播放第 1~2 首之后"（`up≈2850s` 一次），但仍然会来：

```text
W (945766) BT_PCM: A2DP starved ~80ms (cnt=173, avail=0)
W (947749) AppMain: [hb] up=945s ... | i2c f=2848 r=1 last=ESP_ERR_INVALID_RESPONSE | cp st=0 afe=OFF f/s=31 proc=13762/24753us over=4
Guru Meditation Error: Core 0 panic'ed (Interrupt wdt timeout on CPU0)
PC : 0x400967ca  A0 : 0x80096e0c  A2 : 0xffffffff  A3 : 0x0000cdcd
--- 0x400967ca: spinlock_acquire at spinlock.h:142
--- (inlined by) xPortEnterCriticalTimeout at port.c:489
Backtrace: spinlock_acquire → vPortEnterCritical → xQueueGiveFromISR (queue.c:1385)
           → i2c_master_isr_handler_default (i2c_master.c:861) → shared_intr_isr → _xt_lowint1
Core 0 was running in ISR context: EPC1: uart_hal_write_txfifo   ← panic 打印覆盖，红鲱鱼
```

### 根因链

1. `xPortEnterCriticalTimeout()` 第一步就 `portSET_INTERRUPT_MASK_FROM_ISR()`，把 level 1~3 全部屏蔽（**喂 INT WDT 的 tick 就在 level 1**）；
2. 接着 `spinlock_acquire(mux, portMUX_NO_TIMEOUT)`（A2=`0xFFFFFFFF` 就是这个 timeout），抢不到就**原地无限重试**，没有让出、没有超时返回；
3. 于是 ISR 里"关中断 + 死等"：tick 停 → `Interrupt wdt timeout on CPU0`。

所以这是一条**二级故障**：真正的病在"触摸 I2C 反复硬件超时"（`ESP_ERR_INVALID_RESPONSE`，`i2c f=` 已涨到 2848），驱动每次失败都做 FSM reset + `clear_bus`，把 I2C ISR 推进更深的中断嵌套路径；而 CPU 侧又被唤醒词常驻推理（Core1 ~78%、`afe_fetch()` 阻塞）和蓝牙中断/coex 仲裁挤到没有余量，`i2c_master_isr_handler_default` 不能及时跑完，超时率进一步上升 —— 正反馈。

### 处置：功能取舍，不是继续调参数

前两轮（关 HLI、播放态暂停 AFE）已经把语音唤醒的价值压到"播放时不能唤醒"，再调只剩关掉。主节点定位是**同步播放音箱**，语音助手属可选功能，于是整条链路下线：

- 移出构建：`CallPhone`（esp-sr 唤醒词 + HFP 上行）、`microphone`（INMP441 I2S RX + 读取任务）→ `Play_Master/legacy_disabled/`（源码留档，附恢复清单）
- `main/idf_component.yml` 注释 `esp-sr`（连带 esp-dl/esp-dsp/dl_fft 不再参与构建）、`App/CMakeLists.txt` 去 `esp-sr`、`sdkconfig` 删 `ESP Speech Recognition` 整段
- `app_main.c`：删 `Microphone_Init`、`App_CallPhone_Task`、`App_Afe_Arbiter_Task`、`APP_EVT_CALL_*`，心跳去掉 `cp/afe/f-s/proc/over` 字段
- UI：`ui_cfg_t` 去 `call_phone`；设置页删"语音助手"分区（第 3 页只剩"固件"）；状态栏由四图标改三图标（WiFi / 蓝牙 / 同步）
- 特意不动：`bt_audio` 的 HFP 代码（已无 `esp_hf_client_connect` 调用者，SLC/SCO 永不建立，运行期零开销）、`i2s_driver` 的 RX 能力、分区表 `model` 区

### 验证要点

- 启动日志中不再出现 `[CallPhone] / afe / esp-sr / wakenet`；`[heap] after amp` 的内部 DRAM 余量应上升；
- `[hb]` 行尾只到 `i2c f= r= last=`，是判断"跑的是新固件"的最快标志；
- `[btpcm]` 的 `un=` 与 `gap=` 应显著下降（原 173 / 77~117ms），`A2DP starved` 基本消失；
- `i2c f=` 增速：明显变慢说明总线失败确实由 CPU 挤占间接造成；若仍每秒数次稳定增长，则是触摸硬件/上拉问题，需硬件侧处理。

### 遗留（与语音无关）

- 触摸 I2C ≈3% NACK：优先查上拉与事务超时（10ms 偏紧），可考虑失败直接跳过、减少 `clear_bus` 引发的深层 ISR 嵌套；
- `LCD_Touch.c` 读失败退避计数成功后未归零，触摸会"越用越迟钝"；
- 可选加固：`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` 0 → 4096，减少 PSRAM 自旋锁路径（第 3 次崩溃 `cpu.c:205`）被触发的机会。

