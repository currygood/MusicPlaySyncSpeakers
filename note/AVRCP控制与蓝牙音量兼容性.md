# AVRCP 控制与蓝牙音量兼容性笔记

> 日期：2026-09-30 ｜ 项目：Play_Master（bt_audio）

## 1. 现象

- 播放/暂停/上一首/下一首等 AVRCP 控制命令全部返回 `ESP_ERR_INVALID_STATE`；
- 调音量（`bt_audio_set_volume()`）返回 `ESP_OK`，但手机音量条不变化、本机无反应；
- 日志中出现：

```
W (...) BT_BTC: A2DP Enable without AVRC
W (...) BT_BTC: AVRC Controller is expected to be initialized in advance of A2DP !!
W (...) BT_BTC: AVRC Target is expected to be initialized in advance of A2DP !!
...
W (BT_BTC): handle_rc_metamsg_rsp: code 10 error 0x0
```

## 2. 根因一：AVRC 初始化顺序必须在 A2DP 之前

ESP-IDF 的 AVRCP 控制通道由 `esp_avrc_ctrl_init()` 建立，且**必须在 A2DP 初始化之前**调用：
先 A2DP 后 AVRC 的话协议栈认为 “AVRC not Init”，控制指令找不到通道，全部返回 `ESP_ERR_INVALID_STATE`。

## 3. 根因二：安卓手机默认拒绝 “绝对音量”（Absolute Volume）

`SetAbsoluteVolume` 属于 AVRCP 绝对音量通道。多数安卓手机**默认关闭“蓝牙绝对音量”**，
指令到达手机后以 `code 10`（`REJECT`）回拒，音量不会反映到手机，也不会反馈到本机音箱。

## 4. 解决

### 4.1 初始化顺序修正

```c
esp_avrc_ctrl_init();          /* 先 AVRCP */
esp_a2dp_...();                /* 再 A2DP */
esp_bt_gap_set_scan_mode(...); /* GAP 可发现/可连接 */
```

修正后 play / pause / prev / next / toggle 均返回 `ESP_OK`。

### 4.2 音量策略（兼容两种手机）

- 本机音量加/减（UI、模块命令）→ **发 AVRCP 透传按键** `ESP_AVRC_PT_CMD_VOL_UP` / `VOL_DOWN`（不依赖绝对音量通道）；
- 手机开了“蓝牙绝对音量”→ 保留 `SetAbsoluteVolume` 路径做滑块联动；
- 本机扬声器音量始终由 amplifier 模块软件音量实现（见`功放软件音量实现.md`），不依赖手机是否支持绝对音量。

## 5. 验证

- 控制命令全部 `ESP_OK`，UI 播放/暂停、上一首/下一首正常；
- 手机不开绝对音量：本机音量仍有效（软件音量真实作用），手机滑块可能不动；
- 手机开绝对音量：滑块与本机同步。

## 6. 备注

- `get_track_info` 返回 `ESP_ERR_NOT_FOUND` 是部分手机不主动上报元数据，属正常，与音量问题无关；
- 音量变化一定要接到真实输出端（amplifier）而不是只存变量（见下个笔记）。
