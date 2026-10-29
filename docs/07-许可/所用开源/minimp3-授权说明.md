# minimp3 授权说明

> 本文档记录音频解码库 **minimp3** 在 `Play_Master` 中的来源、版本状态、本地修改与授权情况。
> 结论先行：**CC0-1.0（公有领域奉献），允许商用、修改、闭源再分发，不要求署名、不要求随附许可文本、无 copyleft**。
> 全项目对该库的唯一改动是新增一个"由调用方提供 scratch"的解码入口，属 CC0 允许的修改，无附加义务。

| 项目 | 内容 |
| :--- | :--- |
| 组件 | minimp3（单头文件 MP3 解码器） |
| 许可证 | CC0-1.0 |
| 上游 | <https://github.com/lieff/minimp3> |
| 本地位置 | `Play_Master/components/middlewares/audio_decoder/third_party/minimp3/` |
| 使用模块 | `components/middlewares/audio_decoder`（SD 卡 MP3 → PCM） |
| 编制日期 | 2026-10-29（工作区快照 git `fb48714`） |
| 关联文档 | `docs/03-开发/第三方依赖与许可证.md`、`docs/02-设计/蓝牙从手机获取音频.md` |

---

## 1. 为什么选它

主节点要把 SD 卡里的 MP3 流式解成 16-bit PCM，再送本地功放并转发给从节点。选型时考虑过的方向：

| 方案 | 结论 |
| :--- | :--- |
| **minimp3**（单头文件，C99） | **采用**：零依赖、基础接口无动态分配、约 70 KB 源码即可解一帧；对外只有 `mp3dec_t` ＋ 一次 `mp3dec_decode_frame()` 调用，便于自建 8 KB 滑动窗口按需续读 SD 文件 |
| Helix / libmad | 未采用：体积与集成成本高于收益，本板算力足够 |
| ESP-ADF 的 MP3 解码器 | 未采用：会连带引入整条 ADF 音频管线，与现有分层（App / middlewares / bsp）冲突 |

minimp3 的设计目标是"小、快、符合 ISO 一致性测试"——上游 README 中 11 组 conformance 向量 PSNR 均 > 96 dB；输出为 16-bit 交织 PCM，正好匹配下游 I2S 与组播包格式。

> 上表是**当前技术判断**，不是历史调研记录；本项目只实际集成过 minimp3。

---

## 2. 源信息与本地副本

| 项 | 值 |
| :--- | :--- |
| 上游仓库 | <https://github.com/lieff/minimp3> |
| 上游形态 | 单头文件 `minimp3.h`，`MINIMP3_IMPLEMENTATION` 宏在唯一编译单元展开 |
| 版本标识 | **无**。上游不发布版本号或 tag，`minimp3.h` 始终以单文件分发，只能靠 SHA256 或 diff 标识 |
| 本地路径 | `Play_Master/components/middlewares/audio_decoder/third_party/minimp3/minimp3.h` |
| 本地大小 / 行数 | 79,333 B / 1,876 行 |
| 本地 SHA256 | `d56283e9c59abef0ab124cc369ce34c40d9c07bad2b11547aad94377ea3795d5` |
| 上游对齐快照（2026-10-29 取 `master`） | 76,831 B，SHA256 `57e437c5c1f0e8b243885d3929c8973b5e6c778451e0100ab4251d19915cb3ad` |
| 与上游差异 | 10 个 hunk，新增 24 行 / 删除 13 行，全部集中在 §3 的补丁点 |
| 附带文件 | 上游 `README.md` 原样保留在同目录，作为出处凭证 |

体积差异（+2,502 B）中约 1.9 KB 来自 CRLF 行尾，其余即补丁本身。

---

## 3. 本地补丁：把 15.5 KB scratch 挪出任务栈

### 3.1 为什么必须改

minimp3 解一帧 MP3 需要一个约 **15.5 KB** 的暂存区 `mp3dec_scratch_t`（granules 缓冲 + 合成窗口）。上游的 `mp3dec_decode_frame()` 是**在函数内按值分配**这个结构体的，也就是压在调用者栈上。

在 ESP32 上这不可接受：给一个音频任务开 20 KB 以上内部 DRAM 栈，会直接挤掉 WiFi/蓝牙协议栈的空间。

### 3.2 改了什么

只增加一个入口，不动算法：

```c
/* [Play_Master 本地补丁] 与 mp3dec_decode_frame() 语义完全一致，区别是
 * mp3dec_scratch_t（约 15.5KB）由调用方提供：默认入口把它分配在自身栈上，
 * 栈受限场景（ESP32 内部 DRAM 任务栈紧张）可改用本入口把大缓冲挪到
 * 堆/PSRAM。除暂存位置外无任何行为差异。 */
int mp3dec_decode_frame_scratch(mp3dec_t *dec, const uint8_t *mp3, int mp3_bytes,
                                mp3d_sample_t *pcm, mp3dec_frame_info_t *info,
                                mp3dec_scratch_t *scratch);
```

要点：

- 上游 `mp3dec_decode_frame()` **原样保留**（本地文件第 1813 行），行为未变，别的工程可直接用；
- 新入口只是把 `scratch` 从"函数内局部变量"变成"调用方指针"，解码流程、状态机、算法一行未改；
- `audio_decoder.c` 只调用新入口，把 scratch 预分配到 **PSRAM 优先** 的堆上。

### 3.3 收益（实测，见 `audio_decoder.c` 头注释）

| 项 | 上游默认入口 | 本项目（补丁入口） |
| :--- | :--- | :--- |
| 帧解码 scratch（`mp3dec_scratch_t`） | ~15.5 KB，占任务栈 | ~15.5 KB，堆 / PSRAM |
| 单帧 PCM（1152 帧 × 2ch × 2 B） | 4.6 KB，占任务栈 | 4.6 KB，堆 / PSRAM |
| 解码任务栈 | ≈ 21 KB（15.5 + 4.6 + 调用深度） | **4 KB**（只剩调用深度） |

---

## 4. 在本项目中的用法

`audio_decoder` 只做两件事：把 MP3 帧解成 PCM，通过回调抛出去。播放/暂停/切歌的决策权在 App 层 `MusicPlayer`。

| 参数 | 值 | 说明 |
| :--- | :--- | :--- |
| 使用接口 | `mp3dec_t` + `mp3dec_decode_frame_scratch()` | 不使用 `mp3dec_ex` 流式接口 |
| 输入缓冲 | 8 KB，PSRAM 优先（失败回退内部 DRAM） | 约容纳 6 帧 320 kbps / 44.1 kHz；剩余 < 512 B 时从 SD 续读 |
| 读文件方式 | 自建滑动窗口 + `memmove` | `mp3dec_ex` 内部 128 KB IO 缓冲会 malloc 进 PSRAM，在本板触发过 cache 相关看门狗复位，故弃用 |
| 首帧探测 | 解码任务内完成，不整文件扫描 | `open()` 只等一个信号量；时长按 `文件大小 × 8 / 码率` 估算，避免大文件打开卡顿 |
| 支持格式 | 仅 MP3 | WAV/FLAC 等未引入，避免额外解码库 |
| 输出 | 16-bit 交织 PCM + 采样率/声道 | 采样率与 44.1 kHz 全链路是否一致由上层判断 |
| 任务 | Core 1 / 优先级 10 / 栈 4 KB | 大缓冲全部在堆上，见 §3.3 |

数据流：`SD 卡 → FatFs → audio_decoder（minimp3）→ PCM → sync_protocol（UDP 组播）/ I2S 功放`。

从节点（`Play_Slave`）不链接 minimp3：它只收 PCM，不做解码。

---

## 5. 许可与义务

### 5.1 许可原文

`minimp3.h` 文件头即是完整的授权声明：

```
https://github.com/lieff/minimp3
To the extent possible under law, the author(s) have dedicated all copyright and
related and neighboring rights to this software to the public domain worldwide.
This software is distributed without any warranty.
See <http://creativecommons.org/publicdomain/zero/1.0/>.
```

### 5.2 CC0-1.0 意味着什么

| 问题 | 结论 |
| :--- | :--- |
| 可以商用吗 | 可以，无任何费用或数量限制 |
| 可以修改吗 | 可以，无需公开修改内容 |
| 可以闭源分发吗 | 可以，CC0 无 copyleft |
| 必须署名吗 | **不必**（本项目仍保留版权头与 README，属工程习惯而非义务） |
| 必须随产品附许可文本吗 | **不必** |
| 有专利授权吗 | **没有**。CC0 只放弃著作权，不含明示专利许可（这一点与 Apache-2.0 不同） |

MP3 相关专利池已于 2017 年到期（Fraunhofer 于 2017 年 4 月终止 MP3 专利许可项目），因此当前不构成额外许可成本。若后续引入 AAC / LC3 等仍在专利期内的编码，需要重新评估——这也是本项目只做 MP3 的原因之一。

### 5.3 本项目的实际做法

- 保留上游版权头与 `README.md`，便于追溯出处；
- 补丁处标注 `[Play_Master 本地补丁]` 并说明动机，升级时可快速定位；
- 不把 minimp3 作为独立商品出售，也不对外宣称其为自研。

以上三点都不是 CC0 的要求，是维护性约定。

---

## 6. 维护与升级

### 6.1 校验当前副本

```bash
cd F:/EmbeddedProject/MusicPlaySyncSpeakers
sha256sum Play_Master/components/middlewares/audio_decoder/third_party/minimp3/minimp3.h
# 期望：d56283e9c59abef0ab124cc369ce34c40d9c07bad2b11547aad94377ea3795d5

curl -sSL --ssl-no-revoke -o upstream_minimp3.h \
     https://raw.githubusercontent.com/lieff/minimp3/master/minimp3.h

diff <(tr -d '\r' < upstream_minimp3.h) \
     <(tr -d '\r' < Play_Master/components/middlewares/audio_decoder/third_party/minimp3/minimp3.h)
```

预期差异只有 10 个 hunk、新增 24 行、删除 13 行，且全部落在两个函数签名与 `scratch` 的取用方式上。

### 6.2 升级后重打补丁

minimp3 没有版本号，升级就是整体替换文件，替换后**必须重打补丁**：

1. 用上游新文件覆盖本地 `minimp3.h`；
2. 在 `mp3dec_decode_frame()` 之前插入 `mp3dec_decode_frame_scratch()`：把上游函数体整体复制一份，删掉函数内的 `mp3dec_scratch_t scratch;`，把所有 `scratch.` 改成 `scratch->`；
3. 上游 `mp3dec_decode_frame()` 保持不动（继续用栈上 scratch）；
4. 编译；若报 `undefined reference to mp3dec_decode_frame_scratch`，说明第 2 步没做或签名不一致；
5. 更新本文 §2 的大小、SHA256 与差异行数。

漏打补丁会直接链接失败，不会静默跑错——这是刻意保留的"安全网"。

---

## 7. 引用位置

| 位置 | 内容 |
| :--- | :--- |
| `components/middlewares/audio_decoder/third_party/minimp3/minimp3.h` | 库本体（含本地补丁） |
| `components/middlewares/audio_decoder/third_party/minimp3/README.md` | 上游说明原样保留 |
| `components/middlewares/audio_decoder/audio_decoder.c` | 唯一编译单元（定义 `MINIMP3_IMPLEMENTATION`）与调用方 |
| `components/middlewares/audio_decoder/audio_decoder.h` | 模块接口，头注释注明"仅支持 MP3（CC0-1.0）" |
| `docs/03-开发/第三方依赖与许可证.md` | 依赖清单入口 |
| `note/audio_decoder模块开发遇到的问题.md` | 开发期问题记录（栈溢出、`mp3dec_ex` 看门狗等） |
