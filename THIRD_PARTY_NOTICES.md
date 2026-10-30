# 第三方组件与许可声明（Third-Party Notices）

本仓库自身以 **Apache-2.0** 发布（见 [LICENSE](LICENSE)）。
本文件汇总仓库内**随源码分发**的第三方内容、以及**构建时拉取、最终进入固件**的第三方组件，供分发、二次开发与产品化时合规使用。

> 本文件同时充当 Apache-2.0 第 4(d) 条所述之 NOTICE。分发本项目的源码或固件时，请一并保留本文件。
> 每个组件的详细调研、版本核实与本地修改记录，见 [`docs/07-许可/`](docs/07-许可/)。

---

## 1. 随仓库分发的第三方内容

这些文件直接躺在仓库里，克隆即得，因此声明是**必须**的。

| 组件 | 仓库内位置 | 版本 | 许可证 | 版权 | 本项目义务 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **minimp3** | `Play_Master/components/middlewares/audio_decoder/third_party/minimp3/` | 无版本号（单头文件） | **CC0-1.0** | lieff 及贡献者 | 无（已保留出处与修改标注） |
| **Noto Sans SC**（GB2312 子集，wght=400） | `Play_Master/storage/font/ui_font.ttf` | 由 NotoSansSC-VF 派生 | **OFL-1.1** | Adobe / Google Inc. | 保留版权与许可、修改版不得使用保留字体名（已改名） |
| **Material Icons**（17 码位，16/24 px 位图字体） | `Play_Master/components/App/UI/ui_icons_16.c`、`ui_icons_24.c` | `MaterialIcons-Regular.ttf` 转换 | **Apache-2.0** | Google Inc. | 附许可声明、标注"已修改" |

补充说明：

- minimp3 为 CC0（公有领域奉献），**不要求署名、不要求随附许可文本**；本项目仍保留上游 `README.md` 与版权头，便于追溯。
- Noto Sans SC 与 Material Icons 的完整授权分析见：
  - [`docs/07-许可/所用开源/NotoSansSC-授权说明.md`](docs/07-许可/所用开源/NotoSansSC-授权说明.md)
  - [`docs/07-许可/所用开源/minimp3-授权说明.md`](docs/07-许可/所用开源/minimp3-授权说明.md)

## 2. 构建时拉取、随固件分发的第三方

这些组件**不在仓库里**（`managed_components/` 与 ESP-IDF 均不入库），由 `idf.py` 按 `dependencies.lock` 自动拉取；但编译产物中包含它们，因此发布固件时必须声明。

| 组件 | 版本 | 许可证 | 用途 | 本项目义务 |
| :--- | :--- | :--- | :--- | :--- |
| **LVGL** | 9.6.0 | MIT | UI 框架（屏幕、控件、字体调度） | 保留版权与许可文本 |
| **FreeType** | 2.14.3 | **FTL**（双许可中选 FTL，非 GPL） | 运行时 TTF 字体渲染 | **在产品文档中致谢 FreeType** + 保留许可 |
| **cJSON** | 1.7.19 | MIT | 灯控协议 JSON 编解码 | 保留版权与许可文本 |
| **ESP-IDF** | v6.0.1 | Apache-2.0 | 框架、驱动、系统服务 | 附许可副本、保留声明 |
| ├ FreeRTOS Kernel | V10.5.1 | MIT | 任务与调度 | 保留版权声明 |
| ├ lwIP | 随 IDF | BSD-3-Clause | TCP/IP、UDP 组播 | 保留版权声明，不得以作者名义背书 |
| ├ mbedTLS | 随 IDF | Apache-2.0（或 GPL-2.0，本项目取前者） | OTA over HTTPS | 附许可副本 |
| ├ Bluedroid（含 SBC 解码） | 随 IDF | Apache-2.0 | 经典蓝牙 A2DP / AVRCP / HFP | 附许可副本 |
| ├ FatFs | R0.15 w/patch2 | ChaN FatFs 许可（BSD 式单条款） | SD 卡文件系统 | 保留版权与免责声明 |
| ├ SPIFFS | 随 IDF | MIT | `storage` 分区 | 保留版权声明 |
| └ Montserrat（LVGL 内置 14 px） | 随 LVGL | OFL-1.1 | 数字与西文回退字体 | 保留版权与许可 |

### FreeType 致谢（FTL 强制要求）

按 FreeType License 的广告条款，产品文档或"关于"页中需出现以下文字（年份取实际使用的版本）：

```
Portions of this software are copyright © 2026 The FreeType Project
(https://freetype.org). All rights reserved.
```

## 3. minimp3 专项

| 项 | 值 |
| :--- | :--- |
| 上游 | <https://github.com/lieff/minimp3> |
| 许可证 | CC0-1.0（公有领域奉献，允许商用、修改、闭源再分发） |
| 本地路径 | `Play_Master/components/middlewares/audio_decoder/third_party/minimp3/minimp3.h` |
| 本地 SHA256 | `d56283e9c59abef0ab124cc369ce34c40d9c07bad2b11547aad94377ea3795d5` |
| 本地修改 | 新增 `mp3dec_decode_frame_scratch()`（+24/−13 行，10 个 hunk），把约 15.5 KB 的 `mp3dec_scratch_t` 从任务栈移到 PSRAM 堆；上游 `mp3dec_decode_frame()` 原样保留 |
| 许可义务 | **无**。CC0 不要求署名、不要求随附许可、无 copyleft；不含明示专利授权（MP3 专利已于 2017 年到期） |

完整说明（为什么打补丁、栈占用对比、升级后如何重打补丁）：[`docs/07-许可/所用开源/minimp3-授权说明.md`](docs/07-许可/所用开源/minimp3-授权说明.md)

## 4. 各许可证对使用者的最小要求

| 许可证 | 涉及组件 | 你需要做什么 |
| :--- | :--- | :--- |
| CC0-1.0 | minimp3 | 无强制要求 |
| MIT | LVGL、cJSON、FreeRTOS、SPIFFS | 保留版权声明与许可文本 |
| BSD-3-Clause | lwIP | 保留版权声明，不得用作者名背书 |
| Apache-2.0 | 本项目、ESP-IDF、Material Icons、mbedTLS | 附许可副本、保留 NOTICE、标注对文件的修改 |
| FTL | FreeType | 文档致谢 FreeType + 保留许可文本 |
| OFL-1.1 | Noto Sans SC、Montserrat | 保留版权与许可，修改版须改名 |
| ChaN FatFs | FatFs | 保留版权声明与免责声明 |

本项目**未链接任何 GPL / AGPL / LGPL 组件**；FreeType 走 FTL 分支。

## 5. 维护约定

新增第三方依赖时：

1. 在 `docs/07-许可/所用开源/` 下新增一份 `<组件>-授权说明.md`，记录版本、许可、来源与修改；
2. 在本文件对应小节补一行；
3. 若修改了第三方源码，在文件内用 `[本项目修改]` 标注原因（Apache-2.0 / OFL 的硬性要求，其余许可证属良好实践）；
4. 若该组件随仓库分发（vendored），确认其目录内保留原始 `LICENSE` / 版权头。
