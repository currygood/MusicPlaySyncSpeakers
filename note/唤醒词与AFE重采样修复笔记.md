# 唤醒词与 AFE 重采样修复笔记

> 日期：2026-10-01 ｜ 项目：Play_Master（CallPhone 语音唤醒 + 上行通话）

## 1. 背景

- 唤醒词：**你好小智**（esp-sr `wn9s_nihaoxiaozhi`，sdkconfig 已配）；
- 链路：mic(44.1k stereo) → audio_bus(fifo) → CallPhone(取左声道 → 44.1→16k 重采样 → ×4 增益 → AFE feed → fetch) → wake → bt_audio(44.1→16k 上行) → SCO(mSBC) → 手机。

## 2. 三个 bug / 对应修复（按发现顺序）

### 2.1 feed() 缺失 → AFE 环缓冲为空

现象：`W AFE: Ringbuffer is empty, Please use feed()` 每 200ms 一条刷屏；唤醒无反应。

原因：文件恢复事故后，`CallPhone_Listening()` 丢失了：

1. `CallPhone_Resample_16_16()`（44.1k mono → 16k，填 feed_buf）的调用；
2. `afe_if->feed()` 的调用（只有 fetch，没有 feed）。

修复：恢复「重采样一帧 → `feed()` → `fetch()`」的配对调用（参考 ESP_AI 工程 ASR.c）。

### 2.2 重采样游标未折回 → feed 每帧恒定值（静音）

- 现象：`feed16k avg=640 peak=640`（avg==peak、值全是 4 倍数）；或 `avg=0 peak=0` 但 mono 有信号；
- 根因：16.16 定点游标 `rs_pos` 跨帧累计，但每帧输入窗口只有 `in_need`(1412) 个新样本；
  从第 2 帧起 `idx = pos>>16` 越界，输出被钳位成“最后一个样本”→ 整帧恒定（≈ 静音）；
- 修复：每帧重采样后 `rs_pos -= (in_need << 16)`，负值归 0；
- 定位手段：临时打印 `mono[0..3] / feed[0..3] / pos`，看“mono 有变化但 feed 恒定”。

### 2.3 上行（手机听不到）同一类 bug

- 根因：`bt_audio.c bt_hfp_data_send_cb()` 里的 `hfp_rs_pos` 同样是跨回调累加、每帧只装新样本 → 第2帧起静音；
- 修复：回调内重采样后同样折回 `in_need << 16`。

## 3. 关键参数（当前落地值）

| 项 | 值 |
|---|---|
| mic 采样 | 44.1kHz stereo，I2S 32bit 槽位，取左声道 |
| AFE 输入 | 16kHz mono int16，`get_feed_chunksize()` = 512 |
| 重采样 | 16.16 定点线性插值，step = (44100<<16)/16000 = 180633 |
| 唤醒增益 | ×4（实测说话平均幅度 200~600，偏小）|
| 对话单帧 | 读 32ms + AFE 处理 ~11ms → wall 约 43ms/帧 |

## 4. 经验

- esp-sr：feed/fetch 成对；feed 是 16k 单声道 int16，不能漏帧；
- “每帧喂新窗口”的重采样要么：游标按窗口折回/复位；要么：维护绝对游标 + 每帧只读对应位置（本工程用前者）；
- 调试期日志会影响时序，但这次靠“临时打印 mono/feed/pos” 拿到决定性的数据（对比 mono 有语音、feed 恒定）即可结束过度猜测；
- 正式版把这款统计日志注释掉（已在 call_phone.c 注释，保留可恢复）。
