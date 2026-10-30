# MusicPlaySyncSpeakers

一套跑在 ESP32 上的**多设备无线音频同步系统**：主节点接收手机蓝牙音频（或读 SD 卡本地 MP3）并解码，通过 WiFi 组播把 PCM 同步分发给多个从节点，实现"手机放歌，全屋音箱同时响"。

> 目标不是做一个蓝牙音箱，而是验证一条**低成本、可扩展的全屋音频**路径：单主多从、主从各自供放、多台音箱同时出声不错位。

## 架构

```
            手机（A2DP Source）
                    │ Bluetooth Classic（SBC，协议栈内解码）
                    ▼
        ┌───────────────────────────────┐
        │  Play_Master  (ESP32-WROVER-E)│
        │  ┌─────────────────────────┐  │
   SD ──┼──│ audio_decoder (minimp3) │  │
        │  └───────────┬─────────────┘  │
        │              ▼                │
        │        PCM 44.1k/16bit        │
        │      ┌───────┴────────┐       │
        │      ▼                ▼       │
        │   本地 I2S 功放   UDP 组播 239.0.0.1:5678
        └──────────────────────┬────────┘
                               ▼
                    ┌──────────────────────┐
                    │ Play_Slave (ESP32-S3)│ × N
                    │ UDP 收包 → 抖动缓冲   │
                    │ → I2S 功放           │
                    └──────────────────────┘
```

- **主节点**：蓝牙 A2DP Sink / SD 卡 MP3 播放、UI（2.8" 触摸屏 + 中文字体）、灯控、OTA；
- **从节点**：只收 PCM 并播放，无 UI、无解码，成本极低；
- **同步**：组播单包 15 ms PCM，从节点按序对齐播放时刻。









## 许可

本项目以 **Apache License 2.0** 发布，见 [LICENSE](LICENSE)。

第三方组件的许可、版本与本地修改记录见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)：

- **minimp3**（MP3 解码）— CC0-1.0，本项目对其打了一处补丁（把 15.5 KB 解码 scratch 移出任务栈），详见 [`docs/07-许可/所用开源/minimp3-授权说明.md`](docs/07-许可/所用开源/minimp3-授权说明.md)
- **LVGL** 9.6.0 — MIT；**FreeType** 2.14.3 — FTL；**cJSON** 1.7.19 — MIT；**ESP-IDF** 6.0.1 — Apache-2.0
- **Noto Sans SC**（中文子集）— OFL-1.1；**Material Icons**（图标位图）— Apache-2.0

未链接任何 GPL / AGPL 组件。

## 致谢

- [lieff/minimp3](https://github.com/lieff/minimp3) — 极简 MP3 解码器
- [LVGL](https://lvgl.io/) — 嵌入式 GUI
- [FreeType](https://freetype.org/) — 字体渲染
- [Noto Sans SC](https://fonts.google.com/noto) / [Material Icons](https://github.com/google/material-design-icons) — 字体与图标
- [ESP-IDF](https://github.com/espressif/esp-idf) — 框架与协议栈

> 本项目为个人学习与验证性质，硬件与音质未做产品级认证；自行复刻请注意功放供电与散热。
