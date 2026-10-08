# HFP 下行（语音助手应答）无声 bug 排查笔记

> 日期：2026-10-08 ｜ 项目：Play_Master（audio_bus 删除后重构回归） ｜ 状态：已修复，待重新编译验证

## 1. 现象

- 唤醒词命中、HFP SCO 打开（mSBC 16k）后，手机语音助手的应答**功放完全听不到**；
- 同一块硬件在重构前（第九阶段 A2DP 等）播放正常，功放本身没问题；
- 关键日志：

```
I (106654) bt_audio: HFP SCO open (mSBC, 16000 Hz, frame=0 B)
I (107256) bt_audio: HFP volume set to 1
I (107257) AMPLIFIER: volume set: 1%
I (109898) bt_audio: HFP SCO closed   （约 3 秒后手机结束应答）
```

## 2. 根因（两处叠加）

### 2.1 声道不匹配：HFP 下行是单声道，功放总线是立体声

- bt_audio 的 HFP 下行（SCO 下行上采样到 44.1kHz）输出**单声道**，经 `on_pcm(HFP_DOWNLINK)` 给到 App；
- 主节点功放总线是**立体声**（`AMPLIFIER_CHANNEL_NUM=2`，L/R 槽位写同一份数据）；
- `App_Bt_Pcm_Cb` 把单声道原样交给 `Amplifier_Play_Buffer()`：
  - 字节数 = 样本数×2，通常不是 4 的倍数 → `i2s_channel_write()` 拒绝写入（错误被回调忽略）→ 静音；
  - 就算能写，单声道也会被当作双声道帧解析，变调/丢数据；
- A2DP 与本地 MP3 播放本来就是立体声，所以之前正常，HFP 下行是第一次真正踩到这个场景。

### 2.2 HFP +VGS 音量刻度 0~15 被当 0~100 用

- HFP 规范里 `+VGS` 音量范围是 0~15，AVRCP 绝对音量是 0~100（bt_audio 已归一为 0~100）；
- 这次手机报了 `VGS=1`，旧代码直接 `Amplifier_Set_Volume(1)` → 功放 1%；
- 功放音量用平方曲线 `gain=(vol/100)^2`，1% 约等于 -80dB，基本不可闻——即使声道问题修好也照样听不见。

## 3. 修复（已落地）

### 3.1 单声道 → 立体声扩展

- `Play_Master/main/app_main.c` → `App_Bt_Pcm_Cb()`：
  - 新增静态缓冲 `s_hfpDnStereoBuf[2048]`；
  - `BT_AUDIO_PCM_SOURCE_HFP_DOWNLINK` 先复制为 L/R 相同（字节数 = 样本数×4 的倍数），再调 `Amplifier_Play_Buffer()`;
  - 超大块打 WARN 保护（最坏一帧 882×4=3528B < 4096B）。

### 3.2 HFP +VGS 归一到 0~100

- `Play_Master/components/middlewares/bt_audio/bt_audio.c` → `VOLUME_CONTROL_EVT` 分支：
  - `v100 = min(v, 15) * 100 / 15`，再发 `BT_AUDIO_EVT_VOLUME_CHANGED`，与 AVRCP 同刻度；
- 音量策略：0~100% 直接是用户设置，0% 静音属产品正常行为，**不做下限**（曾临时加过 20% 下限，已按产品语义移除）。

## 4. 验证

- 手机通话/媒体音量拉大（VGS≈15 → 本地 100%）再唤醒，应能听到语音助手应答；
- 串口应出现：
  - `bt_audio: HFP volume set to N (VGS 0~15 -> 0~100)`
  - `AppMain: volume change: phone=X%`

## 5. 遗留

- 若手机音量拉满仍无声，需怀疑 SCO 下行数据根本没进 bt_audio（`bt_hfp_data_recv_cb` 未收到包），下轮验证时确认，重点看协议栈数据回调。