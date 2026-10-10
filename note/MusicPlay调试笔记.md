# MusicPlay 第十一阶段调试笔记（内部 RAM 不足为主线）

> 日期：2026-10-10 ｜ 项目：Play_Master（App/MusicPlay 实现 + bt_audio / audio_decoder / sync_protocol / wifi_manager 联动）
> 关联文档：`docs/03-开发/主音频节点开发过程.md` 第十一阶段

## 1. 本阶段做了什么

- App 层新增 `components/App/MusicPlay/`：播放状态唯一主人（源切换、播放/暂停/上下曲、音量、播放模式）；UI 命令、源切换、bt_audio 事件、audio_decoder 事件统一进同一个命令队列，由 `MusicPlay_Task`（prio 9 / 栈 4096 / Core 1）串行处理；
- 本地源：扫描 `/sdcard/music/*.mp3` → `audio_decoder_open/play` → 解码 PCM 经 `on_pcm` 回调直送 `sync_protocol_master_push_pcm()`；READY 后 `sync_protocol_master_start(stream_id=2, 44100, 2)`；
- 蓝牙源：控制命令经 `bt_audio_send_ctrl_cmd()`，状态以 bt_audio 回流事件为准；A2DP PCM 由 bt_audio 的 `on_pcm` 直连 sync_protocol（不经 MusicPlay）；
- 音量三元同步：`Amplifier_Set_Volume` + `sync_protocol_master_broadcast_cmd(SYNC_CMD_VOLUME)` + （蓝牙源）`bt_audio_set_volume`，并持久化到 `node_role`；播放模式同样读写 `node_role`；
- `sync_protocol` 的 init/deinit 由 MusicPlay 的 create/destroy 负责，两条组播通道由 App 层经 `wifi_manager_mcast_open()` 传入；
- 测试入口：`app_main.c` 新增 `MusicPlay_Test()`，用 `xTaskCreatePinnedToCore` 起独立任务（栈 4096）——`main_task` 只有 3584B，装不下测试流程。

## 2. 现象总览

| 日志现象 | 表象 | 根因 | 处理 |
|---|---|---|---|
| `E MusicPlay: create: queue/sem create failed` | 模块创建失败 | 内部 DRAM 被 WiFi/BT 吃光 | 队列长度收紧 + 大缓冲移 PSRAM（见 3） |
| `E bt_audio: task create failed: internal free=7155 largest=4608` | bt_audio 起不来 | 内部最大连续块 < 4KB 栈 | 关 HFP AG + 大缓冲移 PSRAM |
| `E SYNC_PROTOCOL: SyncTx_Task / SyncLocalPlay_Task create failed` | 同步任务起不来 | 同上 | 组包缓冲移 PSRAM + 降栈 |
| `E AUDIO_DECODER: open: task create failed` | 本地曲打不开 | 同上 | scratch 移 PSRAM，栈 32768→8192 |
| `E BT_OSI: osi_alarm_new failed ... err 0x101` → `assert failed: hash_map_set` | 连手机时崩溃 | BT 定时器对象池耗尽 | `CONFIG_BT_ALARM_MAX_NUM=50` |
| `.iram0.text will not fit ... overflowed by 13180 bytes` | 链接失败 | IRAM 不够 | 关 `ESP_WIFI_IRAM_OPT` / `RX_IRAM_OPT` |
| `-Werror=format-truncation`（3 处） | 编译失败 | `snprintf` 拼接超出目标缓冲 | 改手工 `memcpy` 限长拼接 |
| `W wifi_manager: mcast sendto failed ret=-1 errno=12` | 组播发包失败 | WiFi 省电攒包 + 组播回环 | `WIFI_PS_NONE` + 关 `IP_MULTICAST_LOOP` |
| 本地曲只出一帧就哑（`pos` 卡 26ms） | 播放一帧后无声 | 同一曲被 open 两遍 | `SELECT_TRACK` 先落 view 再切源 |
| `SYNC_START sent` 出现在 `stop` 之前 | 从节点会抖 | stop 后补发旧流 SYNC_START | `sync_protocol` 加流代号 `stream_gen` |

## 3. 核心问题：内部 RAM 不足

### 3.1 定量：内部 DRAM 到底被谁吃掉

关掉 HFP AG 后一条完整的启动内存曲线（`bt_audio` 打点 + App 打点）：

```
AppMain:  [heap] test start:             int free=163511 largest=110592, psram free=4192024
AppMain:  [heap] after wifi connect:     int free= 99099 largest= 98304
AppMain:  [heap] after mcast:            int free= 94443 largest= 94208
bt_audio: [heap] bt_audio_start enter:   int free= 94267 largest= 90112
bt_audio: [heap] after controller enable:int free= 80863 largest= 77824   ← BT 控制器 ≈ -13KB
bt_audio: [heap] after bluedroid enable: int free= 64383 largest= 63488   ← Bluedroid ≈ -16.5KB
bt_audio: [heap] after profile init:     int free= 64383 largest= 63488
bt_audio: [heap] before task create:     int free= 63675 largest= 62464
```

- 启动 163KB → WiFi 连上 99KB → 组播开好 94KB → BT 控制器 + Bluedroid 干完只剩 64KB；
- 这 64KB 还要放 amplifier I2S DMA 描述符、SD/SPI 驱动、LVGL 缓冲、各模块任务栈；
- HFP AG 还开着时更极端：`E bt_audio: task create failed: internal free=7155 largest=4608`——内部只剩 7KB、最大连续块 4.6KB，连一个 4KB 栈的任务都建不出来；
- 同期 `psram free` 一直是 **~4.1MB**。

**结论：不是内存总量不够，而是"内部 DRAM 不够 + 大块都落在内部 DRAM"。** PSRAM 空着 4MB，内部 DRAM 却精确到几百字节。

### 3.2 处理策略（按收益排序）

#### ① 大缓冲一律 PSRAM（收益最大）

统一写法（与项目既有风格一致，失败回退内部）：

```c
void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
if (p == NULL)
{
    p = heap_caps_malloc(n, MALLOC_CAP_8BIT);   /* 回退内部 */
}
```

本阶段搬到 PSRAM 的缓冲：

| 缓冲 | 大小 | 模块 |
|---|---|---|
| minimp3 帧解码 scratch（`mp3dec_scratch_t`） | ~15.5KB | audio_decoder.c |
| 解码输入窗缓冲 `in_buf` | 8KB | audio_decoder.c |
| sync 入口环 `ingress_buf` | 24KB | sync_protocol.c |
| sync 组包缓冲 `tx_pkt` | ~1.5KB | sync_protocol.c |
| sync 延迟队列 `delay_q` | 16 包 | sync_protocol.c |
| 功放音量衰减工作缓冲 `s_amp_scaled` | 8KB | amplifier.c |
| bt_audio 事件队列 `evt_q` | 4 × sizeof(bt_audio_event_t) | bt_audio.c |
| bt_audio PCM/HFP FIFO 与任务缓冲 | 若干 | bt_audio.c |

> 说明：PSRAM 指针 CPU 常规读写没问题（走 cache），只有 DMA 描述符 / 中断上下文 / `esp_timer` 回调里访问的缓冲必须留内部 RAM。

#### ② 任务栈能降就降（栈必须落内部 DRAM）

| 任务 | 原栈 | 现栈 | 为什么能降 |
|---|---|---|---|
| `audio_decoder` 解码任务 | 32768 | 8192 | 15.5KB scratch 从栈搬到堆，栈上只剩单帧 PCM 4.6KB |
| `SyncTx_Task` | 8192 | 4096 | 组包缓冲 `tx_pkt` 从栈搬到 PSRAM |
| `SyncLocalPlay_Task` | 10240 | 6144 | 出队缓冲不再落栈 |
| `MusicPlay_Task` | 12288（设计稿） | 4096 | 数据解码已移到 audio_decoder |
| `MusicPlay_Test`（测试） | 8192 | 4096 | 测试流程无大局部变量 |

实测 `stack=1784`（AppMain 打印的 MusicPlay 任务高水位），4096 有余量。

#### ③ WiFi/LWIP 也赶到 PSRAM（sdkconfig）

```
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096
CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y
CONFIG_WIFI_LWIP_ALLOCATION_FROM_SPIRAM_FIRST=y
```

启动日志出现 `wifi_init: WiFi/LWIP prefer SPIRAM`，WiFi 的动态 rx/tx 缓冲就落 PSRAM。

#### ④ 砍功能省内部 RAM

- `# CONFIG_BT_HFP_AG_ENABLE is not set`：本项目只做 HFP **HF 端**（音箱当免提），AG（音频网关）是手机侧角色的功能，白占约 16KB；
- `# CONFIG_ESP_WIFI_IRAM_OPT / CONFIG_ESP_WIFI_RX_IRAM_OPT is not set`：这两个是把 WiFi 代码搬进 IRAM 换吞吐的选项，关掉省 IRAM（解决链接期 IRAM 溢出）。

#### ⑤ 定时器池：`CONFIG_BT_ALARM_MAX_NUM=50`

`E BT_OSI: osi_alarm_new failed to create timer, err 0x101` 的下一步就是 `assert failed: hash_map_set` 崩溃（`bta_sys_start_timer` 拿不到 alarm）。这是 BT 协议栈定时器对象池耗尽，加大池子即可。

#### ⑥ 测试代码不要占 main 栈

`CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584`（省内部 RAM），测试流程（I2S/功放/SD/组播/BT 初始化）必须 `xTaskCreatePinnedToCore` 单独起任务。

### 3.3 红线：PSRAM 不是万能

- **PSRAM 不能放 `mp3dec_ex` 的大缓冲**：第五阶段踩过——`mp3dec_ex` 内部约 128KB 缓冲落 PSRAM 后，解码热循环触发 cache-livelock → WDT 复位。所以只用 minimp3 **基础接口** `mp3dec_decode_frame`（scratch 约 15.5KB），并把 scratch 显式分配到 PSRAM 才安全；
- 中断上下文 / DMA 描述符 / `esp_timer` 回调里访问的缓冲必须是内部 RAM（PSRAM 在 cache 关闭或高负载时不可访问）；
- PSRAM 有 cache 开销，热路径上每帧级的小块访问要评估。

### 3.4 本阶段内存相关改动清单

| 文件 | 改动 |
|---|---|
| `music_play.c` | 新建；`MP_CMD_QUEUE_LEN=4`（`music_msg_t` 含 `bt_audio_event_t` 约 536B，队列 8 条要 ~4.3KB 内部 RAM 会失败）；`MP_TASK_STACK=4096` |
| `audio_decoder.c` | `scratch` / `in_buf` 移 PSRAM；任务栈 32768→8192；调用新的 `mp3dec_decode_frame_scratch()` |
| `minimp3.h` | 本地补丁：把 `mp3dec_decode_frame()` 里的 `mp3dec_scratch_t scratch;` 提成入参，调用方决定放哪 |
| `sync_protocol.c` | `ingress_buf` / `tx_pkt` / `delay_q` 移 PSRAM；栈 8192→4096、10240→6144；init 失败回滚并返回 `ESP_ERR_NO_MEM` |
| `amplifier.c` | 音量衰减缓冲 8KB 移 PSRAM |
| `bt_audio.c` | 事件队列 / PCM FIFO / 任务缓冲移 PSRAM；新增 `bt_audio_heap_dump()` 打点 |
| `sdkconfig` | 见 3.2 ③④⑤ |
| `app_main.c` | MusicPlay 测试任务独立 4096B 栈 |

## 4. 其他问题

### 4.1 IRAM 溢出（链接失败）

```
ld.exe: Play_Master.elf section `.iram0.text' will not fit in region `iram0_0_seg'
ld.exe: region `iram0_0_seg' overflowed by 13180 bytes
```

第十一阶段新增 MusicPlay + 改动模块把 IRAM 顶爆。处理：关掉 `CONFIG_ESP_WIFI_IRAM_OPT` / `CONFIG_ESP_WIFI_RX_IRAM_OPT`（把 WiFi 代码从 IRAM 挪回 flash，牺牲一点 WiFi 吞吐换 IRAM）。注意这是 **IRAM** 问题，不是 DRAM。

### 4.2 `osi_alarm_new` 失败 → `hash_map_set` 断言

现象：连接手机瞬间 `E BT_OSI: osi_alarm_new failed to create timer, err 0x101`，随后 `assert failed: hash_map_set hash_map.c:129 (data != NULL)`，调用栈 `bta_hf_client_send_at_brsf → bta_sys_start_timer`。

根因：BT 协议栈定时器对象池（alarm pool）耗尽，`bta_sys_start_timer` 拿不到 alarm，`hash_map_set` 收到 NULL 直接断言；突发负载（HFP SLC 建链 + AVRCP 注册通知）时最容易触发。

处理：`CONFIG_BT_ALARM_MAX_NUM=50`。

### 4.3 `-Werror=format-truncation`（3 处）

```
music_play.c:643 '%s' directive output may be truncated writing up to 255 bytes into a region of size 64
music_play.c:649 '%s' directive output may be truncated writing up to 127 bytes into a region of size 64
music_play.c:929 '%s' directive output may be truncated writing up to 255 bytes into a region of size 114
```

根因：`snprintf(dst, 64, "%s - %s", title, artist)` 这类拼接，源字段（AVRCP 元数据 255B、文件名 255B）比目标缓冲长，GCC 静态分析报必然截断。

处理：不再用 `snprintf` 拼，改成手工 `memcpy` + 显式限长（`part = strlen(src); if (part > room) part = room;`），三处分别对应蓝牙曲目 `display_name`（标题 - 歌手）、扫描时 `/sdcard/music/xxx.mp3` 路径拼接、扫描时 `display_name`（去扩展名）。

### 4.4 本地曲"只出一帧就哑"

现象：`AUDIO_DECODER: diag: frames=1 pos=26/364721ms` 之后 pos 永远停在 26ms，功放只响一帧。

日志证据（同一次 select 里 open 了两遍）：

```
8076 AUDIO_DECODER: open /sdcard/music/xxx.mp3 ...
8081 AUDIO_DECODER: closed handle
8131 AUDIO_DECODER: open /sdcard/music/xxx.mp3 ...
8135 MusicPlay: READY ...
8163 MusicPlay: READY ...          ← 两个 READY
8159 / 8174 两次 TRACK_CHANGED + PLAY_STATE_CHANGED
```

根因：`MUSIC_MSG_SELECT_TRACK` 在非本地源时先调 `Music_Switch_Source_Msg()`（切源内部会按 `view.track_index` 起播一次），返回后又调 `Music_Start_Local_Track()` 再起一次——同一个文件 open 两遍 → 两个 `READY` → 对同一个解码器调用两次 `audio_decoder_play()`，解码任务把多出来的 `PLAY` 当成打断命令退出解码循环。

修复：切源前先把 `view.track_index` / `view.track` 落好，让切源自己完成起播，不再额外起一次（`music_play.c:1062-1084`）。修复后单次 open、pos 正常推进（`frames=39 pos=1018 → frames=77 pos=2011 → ...`）。

### 4.5 `mcast sendto failed ret=-1 errno=12`

现象：音频组播开始发之后刷屏 `W wifi_manager: mcast sendto failed ret=-1 errno=12`（ENOMEM）。

根因两条叠加：
1. **WiFi 省电**：STA 默认 `WIFI_PS_MIN_MODEM`，驱动把包攒到 DTIM 窗口才发，突发时 socket 发送队列打满 → `sendto` 返回 ENOMEM；
2. **组播回环**：`IP_MULTICAST_LOOP` 默认开，主节点把自己发的音频包再收一份（第九阶段单机自环需要，现在从节点才是接收方），白白多占 RX 队列。

处理：
- `wifi_manager_start()` 里加 `esp_wifi_set_ps(WIFI_PS_NONE)`（主节点做低延迟同步本来就不该省电，即收即发）；启动日志确认 `wifi:Set ps type: 0`；
- `wifi_mcast_socket_create()` 增加 `rx_enable` 形参：纯发送通道（音频 5678）关 `IP_MULTICAST_LOOP`，控制通道（5679，要收 PING）保留回环。

### 4.6 `SYNC_START` 竞态（stop 后补发旧流）

现象（日志）：`12051 SYNC_START sent: stream=2 t0=10350887` 出现在 `12061 SYNC_PROTOCOL: stop` **之前**——stop 已经复位本流，发送任务还把手上那帧发出，并补发一条 t0 属于旧流的 `SYNC_START`。

根因：`SyncTx_Task` 在锁外读 `active`，`sync_reset_stream()` 清零 `start_sent` 后，任务又走了一次"首帧补发 SYNC_START"的分支。主节点听不出来（`master_start` 会再清延迟队列），但从节点会跟着旧 t0 抖。

修复（`sync_protocol.c`）：
- `sync_master_t` 增加 `uint32_t stream_gen`，`sync_reset_stream()` 自增；
- `SyncTx_Task` 循环顶部把 `stream_gen` 一起快照，发送前后用 `sync_stream_valid_locked(gen)`（须持 `s_lock`）校验：`started && (state == PLAYING || PREPARING) && stream_gen == gen`，失效即丢帧；
- 音频组播发送仍留在锁外（20ms 阻塞不能占着锁饿死播放任务），但发送后再次锁内校验；`start_sent` / `tx_seq++` / `frame_target` 等提交全部在锁内，与 `stop` 串行。

### 4.7 SD 卡"播 1-2s 停 / 重播 / 彻底停"

先怀疑是 bug，实际是 **测试脚本的固定流程**：`MusicPlay_Test()` 顺序执行 select → NEXT → PAUSE → RESUME → set_mode → 音量加减 → 切 BT 源。日志里 `after NEXT` / `after PAUSE` / `after RESUME` 一一对应，链路本身正常。真正的 bug 只有 4.4 的"open 两遍"。

### 4.8 烧录 `A fatal error occurred: The chip stopped responding`

esptool 写 flash 中途 `StopIteration`。非代码问题：串口/USB 链路抖动，或芯片在写 flash 时被复位。重插 USB / 复位后重烧即可（本次一次即恢复）。

## 5. 经验

- **内部 DRAM 是这个项目最稀缺的资源，不是 PSRAM**。加功能前先问三句：这块缓冲多大？能不能放 PSRAM？能不能不占任务栈？
- 每个模块的 create/start 都要能失败并**回滚**（本阶段补了 `sync_protocol` init 回滚、`bt_audio` task 创建失败回滚 FIFO/队列），否则 RAM 不够时表现为"半死不活的模块 + 玄学崩溃"；
- 内存不够的**第一手证据是打点**：在各阶段打印 `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` + `heap_caps_get_largest_free_block()`（`bt_audio_heap_dump()`），比猜代码快得多；
- **任务栈是内部 RAM 的硬开销**，大局部变量（>1KB）优先搬堆；
- 看到 `errno=12 / ESP_ERR_NO_MEM / task create failed` 先看"最大连续块 largest"——内部 RAM 碎片化会让 7KB free 也建不出 4KB 栈；
- 编解码/协议栈里"突发的定时器/对象池"是隐藏地雷（`BT_ALARM_MAX_NUM`），报错在 A 处、崩溃在 B 处，要顺着调用栈找；
- 别把 PSRAM 当"随便用"——`mp3dec_ex` 那次 cache-livelock 是硬教训（见第五阶段）。
