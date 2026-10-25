# 蓝牙 A2DP 只有"吱吱吱"噪声 —— 字节流错位笔记

> 日期：2026-10-24 ｜ 项目：Play_Master（bt_audio）
> 结论：**不是蓝牙音质/编码问题，是 A2DP PCM FIFO 里写进了半个 int16，整条字节流永久错位。**

---

## 1. 现象

- 主从同步播放修好之后，主节点连手机放蓝牙：喇叭里只有满量程"吱吱吱 / 蹦蹦蹦"，听不到任何音乐；
- 手机侧显示正常播放，进度条在走；
- 日志里采样率正确：`A2DP audio cfg 44100Hz 2ch 16bit`；
- 断开重连后**依然如此**，不是"偶尔一次"。

## 2. 为什么先怀疑数据而不是链路

采样率/声道都对、音量链路也没变，说明数据"到了但内容是乱的"。满量程随机噪声是典型的**采样拼接错位**特征：相邻两字节被当成一个 `int16` 拼，波形完全随机。于是直接去看 `bt_a2dp_data_cb` 怎么把数据写进 FIFO。

## 3. 根因

```c
/* 旧实现 */
sent = xStreamBufferSend(fifo, data, len, 0);
```

FreeRTOS 的 `xStreamBufferSend()` 在 `xTicksToWait = 0` 且空间不足时，是**部分写入** —— 写满剩余空间就返回，返回实际写入字节数。

而 stream buffer 的空闲字节数是：

```text
xSpace = xLength - used - 1        // 永远留 1 字节区分满/空
```

`xLength` 是偶数（64K / 24K），读写游标每次移动整帧 4 字节 ⇒ **xSpace 恒为奇数**。

于是：

1. FIFO 只要溢出过一次（手机突发、任务被抢占都会），这次写入的字节数必然是奇数；
2. 奇数个字节 = 半个 `int16` 进环 → 之后所有采样边界永久偏移 1 字节；
3. 消费端按 `int16_t` 拼 → 满量程随机噪声；
4. 只有 FIFO 被重建（断开重连）才会消失。

**关键点：一次就够，之后永远错位。** 这也解释了为什么"一连上就一直吱"而不是断续。

## 4. 修复

### 4.1 生产端：按整帧裁剪（根治）

`bt_a2dp_data_cb` 不再把 `len` 直接交给 `send()`，而是**要写的量和能写的量都向下取整到 4 字节**：

```c
#define BT_A2DP_FRAME_BYTES 4u   /* 16bit × 2ch */

whole = len   - (len   % BT_A2DP_FRAME_BYTES);   /* 要写 */
space = xStreamBufferSpacesAvailable(fifo);
room  = space - (space % BT_A2DP_FRAME_BYTES);   /* 能写 */
n     = (whole < room) ? whole : room;
if (n == 0) { dropped_pcm += len; return; }      /* 宁可丢整帧 */
xStreamBufferSend(fifo, data, n, 0);
```

原则：**宁可丢整帧，绝不让 FIFO 里出现非整帧字节数。** 丢几毫秒音频听不出来，错一位就整条流废掉。

### 4.2 消费端：错位自愈（兜底）

`bt_audio_task` 收到 `bytes % 4 != 0`，说明环里已经有半帧，边界无法恢复 —— 直接 `xStreamBufferReset()` 丢一段重来，并打 `BT_PCM: A2DP FIFO misaligned`。代价是一次微小断音，远好过"一直吱到用户重连"。

## 5. 顺带修的三件事（不然能出声但会卡）

只修错位后已经能听见音乐，但会"卡、断续"。三个独立原因：

| 问题 | 原因 | 改动 |
| --- | --- | --- |
| 实时性不够 | 任务优先级 5，全系统倒数第二（MusicPlay 9 / decoder 10 / SyncTx 11 / SyncPlay 12） | 5 → **11**，只低于 SyncPlay(12) |
| 和射频抢 CPU | 原来和 WiFi(23)/BT 控制器(23)/lwIP(18) 挤在 core 0 | `xTaskCreatePinnedToCore(..., core 1)` |
| 缓冲余量为零 | 无预灌，FIFO 24KB 且一有数据就往下走 | FIFO → **64KB**（≈371ms）+ 开流预灌 **24KB** |

另外两条容易忽略的：

- **射频共存**：主节点同时在用 WiFi 每 15ms 给从机单播音频，和经典蓝牙抢同一个 2.4G 天线。开流要 `esp_coex_status_bit_set(ESP_COEX_ST_TYPE_BT, ESP_COEX_BT_ST_A2DP_STREAMING)`，停流/断连要清除，否则 WiFi 长帧会把蓝牙接收窗口挤掉 → 80ms 级空洞。
- **别把"FIFO 读空"当欠载**：`xStreamBufferReceive` 返回 0 只代表这 20ms 窗口里空，而手机 SBC 的包间隔本来就是 15~30ms，读空是**稳态**。曾经在 `bytes == 0` 时重新预灌 8KB(46ms)，等于每 20ms 人为停供 46ms → 周期性露底 → "偶尔卡一下"。现在只统计，连续 4 轮（≈80ms）才计一次真欠载。

## 6. 验证

`[btpcm]` 每秒一行：

```text
[btpcm] 88200 samp/s peak=8300 | drop=0 resync=0 un=0 idle=37 gap=28ms
```

| 字段 | 期望 |
| --- | --- |
| `samp/s` | 88200（44100 帧 × 2 声道）；≈96000 说明手机实际发 48k |
| `peak` | 2000~26000 为真实音乐；≈32000 且跳变 = 错位；≈0 = 没数据 |
| `drop` | 恒 0（涨说明消费端被拖住） |
| `resync` | 恒 0（涨说明还有别的非整帧写入路径） |
| `gap` | 稳定在 15~30ms 正常；忽大忽小到 80ms 以上是射频丢包 |

## 7. 通用教训

> **凡是"字节流"（Stream Buffer / 环形缓冲）承载定长采样的地方，生产端必须自己按帧对齐，不能依赖 `xStreamBufferSend` 的语义。**

因为 `xStreamBufferSend` 的部分写入 + 空闲字节恒为奇数，是个**必然**会踩的坑，不是概率问题。项目里已经踩了两次：`sync_protocol` 的入口环、`bt_audio` 的 A2DP FIFO。以后新增任何 PCM 字节流，先问一句"我写进去的字节数是整帧的整数倍吗"。

## 8. 关联笔记

- `docs/04-测试/主节点bug解决.md` —— 「嗡嗡声二次复发：入口环写进半个采样」是同一个坑的第一次；
- 《MusicPlay调试笔记》；
- 《AVRCP控制与蓝牙音量兼容性》。
