> data：2026-10-20

## UI上按切换单曲循环，蓝牙播放还是不变（手机上还是一样，那播放还是按照手机那样的来）



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



## 断掉蓝牙后切换为本地播放，本地播放的声音很卡---->解决

本地解码速度只有实时播放速度的约 **0.91 倍**，因此会持续欠载、听起来卡顿；主要修复是把 CPU 频率从 **160 MHz 提到 240 MHz**。同时，解码器允许更快地解码以补足缓冲，同步播放的 PCM 环形缓冲在播放时提供最多 **30 ms 背压**，用来缓冲调度或读卡抖动。



## UI-wifi图标状态没切换---->解决

​	没有实时的判断wifi有没有连接上然后切换图标状态



## UI进行切换的时候播放sd卡音乐真的卡，不过好像没什么办法可以避免，共用一条总线，改不了，也不够引脚用了，无法再弄一个总线，只能无限性能调优。



## 当LCD屏幕刷新不一样的帧的时候就会导致sd卡音乐播放卡顿，然后就会加速，追回来之后才变为正常速度



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