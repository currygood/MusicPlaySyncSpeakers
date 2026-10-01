# HFP 语音链路调试笔记（已解决，2026-10-01 更新）

> 日期：2026-09-30（2026-10-01 更新为已解决） ｜ 项目：Play_Master（bt_audio HFP）

## 1. 现状

- HFP SLC 连接成功、SCO 可打开（mSBC 16kHz）；
- **麦克风数据无法送到手机**（手机端听不到音响侧语音），原因未明；
- 该问题已在《主音频节点开发过程.md》保留，**计划在 CallPhone 模块完成后回头测试**。

## 2. 已观察到的现象

### 2.1 内存/定时器失败

```
E (36963) BT_OSI: osi_alarm_new failed to create timer, err 0x101 assert failed ...
```

手机允许“蓝牙唤醒”后，HFP + A2DP + 语音识别同时工作，**内部 RAM 不足**导致协议栈断言重启。

### 2.2 SCO 发送队列溢出（上行丢包）

```
W (BT_BTM): SCO xmit Q overflow, pkt dropped   (大量重复)
...
W (...): bt_audio: HFP voice recognition DISABLED
W (...): bt_audio: HFP SCO closed
I (...): bt_audio: A2DP stream started
```

### 2.3 手机侧表现

- 手机语音助手页面显示“点击说话或打字了”，一直收不到音响麦克风上的声音；
- 唤醒词检测有时能触发（HFP voice recognition 事件），但音频数据没有真正送出去。

## 3. 可能原因（待验证）

1. **内部 RAM 不足**：A2DP + HFP/SCO 双链路 + mSBC 编解码，内部堆/IRAM 紧张，定时器与队列分配失败；
2. **上行发送节奏问题**：麦克风数据大块突发送入，SCO 控制器每 7.5/10ms 一个包，发送队列跟不上 → `SCO xmit Q overflow` 直接丢包；
3. **语音识别通道竞争**：`voice recognition` 被自动 DISABLED、SCO 关闭、A2DP 恢复，链路状态机没衔接好；
4. 上行重采样（44100 → mSBC 16k）在中断/定时器里做时被阻塞导致饥饿。

## 4. 解决方向（CallPhone 阶段实施）

- **内存**：任务栈、FIFO 收紧，大的 PCM 缓冲放 PSRAM（内部 RAM 只留必要协议栈）；
- **发送节流**：按 SCO 帧率定时拆包喂（mSBC 每 10ms 320B），不要一次性塞大块；
- **链路状态机**：SCO open 后才启动上行，SCO closed 立即停发并复位 FIFO；
- **集成**：麦克风 → audio_bus → CallPhone（唤醒检测）→ HFP 上行，统一在 CallPhone 里调度，不再用临时测试代码。

## 5. 备注

- 目前 HFP 仅验证了连接/下行收包、“上行听不到”属于遗留问题；
- 后续若调通，记得回填本文件：根因 + 解决方案 + 验证日志。


## 6. 已定位并解决（2026-10-01 更新）

### 6.1 根因

bt_audio.c 的 bt_hfp_data_send_cb()（HFP 协议栈每 7.5ms 回调要一帧 SCO 数据）：

- 每帧从 hfp_up_fifo 取 in_need（约 331 个）新来的 44.1k 单声道样本；
- 经内部 bt_audio_hfp_resample_16_16() 降采样到 mSBC 16k（120 样本/帧）送出；
- 但重采样游标 s_bt_audio.hfp_rs_pos 是跨回调累计的绝对游标，而缓冲每帧只装最新样本：
  第 2 次回调起 idx = pos>>16 越界，被钳到“窗口最后一个样本”上——整帧输出恒为同一数值（约等于静音）。

所以手机端其实一直在收“恒定电平”，语音识别 VAD 判定无语音，约 4.8s 后自动
HFP voice recognition DISABLED 并关闭 SCO。这正是本文件 1、2.3 节遗留的“麦克风上行送不到手机”。

### 6.2 修复（bt_audio.c，send 回调内）

在每个回调的重采样完成后，把游标折回本帧窗口：

```c
s_bt_audio.hfp_rs_pos -= (uint64_t)in_need << 16;
if ((int64_t)s_bt_audio.hfp_rs_pos < 0)
{
    s_bt_audio.hfp_rs_pos = 0;
}
```

（与 CallPhone 监听侧 44.1k → 16k 唤醒重采样是同一类 bug、同一修法。）

### 6.3 验证

- 唤醒词正常（wn9s_nihaoxiaozhi，sdkconfig 配置）；
- SCO 打开后说话，手机语音助手能正常识别并回复（“现在几点”场景验证通过）；
- SCO 不再被自动关闭；第四阶段联调日志见《主音频节点开发过程.md》第四阶段。

### 6.4 备注

- 原 2.1 内存问题（osi_alarm 失败断言）：CallPhone + bt_audio 运行时 internal heap 仍有约 33KB 余量，本次验证未再复现；
- 原 2.2 的 SCO xmit Q overflow 本次验证未出现。
