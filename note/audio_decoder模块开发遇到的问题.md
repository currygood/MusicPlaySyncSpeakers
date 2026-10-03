# audio_decoder 模块开发遇到的问题

## 一、先写结论（经验教训）

1. **小容量 SD 卡格式化选 FAT（FAT16），不要用 FAT32。**
   本项目的 SD 卡是 121MB 的老卡（249344 扇区），Windows 下默认格式成 FAT32 后，
   反复出现“能挂载、能 stat，但 fopen/fseek/fread 出问题（读取 0 字节 / ESP_FAIL）”；
   重新格式化为 FAT16 后读写恢复正常。SD_Card 测试阶段也确认过原卡“大概率坏了”，
   换卡 + FAT16 格式化后稳定。

2. **遇到这种“播放瞬间掉电/复位”问题，先测硬件，再查软件。**
   先用示波器测 3.3V 电源波形，看复位瞬间电源有没有跌落、纹波是否异常；
   再用逻辑分析仪抓复位信号/串口。本次实测：3.3V 波形干净、无跌落 -> 排除硬件供电问题。

3. 本次就是按“先硬件后软件”的流程定位的：
   我通过逻辑分析仪和示波器判断了不是硬件问题，所以定位到软件上；
   而且根据现象（SD-only 正常、BtA2dp 正常、只有 audio_decoder 解码播放触发）
   把目标锁定在 audio_decoder，结果真的是。

## 二、现象回顾

阶段一：用 `Audio_Decoder_Test()` 从 `/sdcard/music` 读取 MP3 解码，并经功放直接播放（测试阶段不经 sync_protocol）。

1) 中文文件名打不开：
   ```
   I (1403) AppMain: test track: /sdcard/music/�Ն�-~1.MP3
   E (1408) SD_CARD: fopen failed: /sdcard/music/�Ն�-~1.MP3 (mode=rb)
   E (1412) AUDIO_DECODER: open: cannot open /sdcard/music/�Ն�-~1.MP3
   ```

2) 改成能打开的文件名后，出现真正的“自动掉电”：
   ```
   I (1827) AppMain: audio_decoder_play: ESP_OK
   --- Error: GetOverlappedResult failed (PermissionError(13, ...))
   --- Waiting for the device to reconnect...
   ```
   设备端然后重启（或完全没有日志），现象上像断电一样。

## 三、原因与解决

### 3.1 小容量卡格式化问题（FAT16 而不是 FAT32）

- 现象：文件能打开（stat 正常），但 `fread` 返回 0、`fseek/tell` 返回 `ESP_FAIL`，
  典型的小容量卡 + FAT32 兼容问题；MEM-ONLY 加载时反复“读到 0 字节”。
  还有首次读取协议错误日志：
  ```
  E (1435) SD_CARD: CSD data token error: err=ESP_ERR_TIMEOUT tok=0xFF
  ```
- 处理：在 Windows 上把 121MB 小卡重新格式化，格式选 **FAT（FAT16）**，不要选 FAT32；
  另外把测试音频文件名先改成 ASCII（避开 FATFS 8.3 短文件名中文乱码），降低干扰源。

### 3.2 播放开始即“掉电”-> 实际是代码侧 WDT 复位

现象特征：
- 只有解码+播放路径触发（SD 只读测试、BtA2dp、Mic->功放回放都不触发）；
- 电源按键轮询定时器已注释、充电器常亮，仍可复现；
- 把整个 LCD 模组（含 SD 卡槽）拔掉后不再触发 —— 这里差点被判定成硬件，
  实际只是把 SD/解码链路一并排掉了；
- 复位原因抓取：rst:0x8 (TG1WDT_SYS_RESET)，bootloader 打印
  PRO CPU/APP CPU 都被 WDT 复位。

排查步骤：

1. 先在 sdkconfig 打开 `CONFIG_ESP_TASK_WDT_PANIC` 与 coredump 到 flash，
   让复位现场落盘，重启后 `idf.py coredump-info` 读回来；
2. 用示波器测 3.3V：复位瞬间无跌落、无毛刺 -> 排除硬件供电；
3. 加 MEM-ONLY 模式（整段读入 PSRAM 后纯内存解码、不读 SD）-> 纯解码也会复位 ->
   证明问题在解码本身（不是 SPI2/SD 并发）。

### 3.3 根因一：mp3dec_ex 流式接口把内部大缓冲分配到 PSRAM -> cache-livelock

- 第一版用了 minimp3 的 `mp3dec_ex`（流式封转接口），内部约 128KB 读缓冲；
- 该缓冲 malloc 超过内部 DRAM 后自动落到 PSRAM；
- 解码热循环（Flash 取指 + 频繁访问 PSRAM）在 ESP32 上与 PSRAM/Flash cache 并发
  触发 cache contention/livelock -> TG1WDT 复位，现象就像“掉电”。

处理：

- 弃用 `mp3dec_ex`，改用 minimp3 基础接口：`mp3dec_t + mp3dec_decode_frame()`；
- 自行实现 8KB 输入缓冲（单个 malloc，内部 DRAM，不落到 PSRAM）+ `memmove` 滑动窗口续读，
  由 sd_card 句柄 API 读文件；
- PCM 输出缓冲放在解码任务栈上（`pcm[MINIMP3_MAX_SAMPLES_PER_FRAME]`，2304x2 字节）；
- 每帧按帧时长软节流（约 26ms @44.1k），避免解码满速占满 CPU。

### 3.4 根因二：minimp3 栈占用（收尾关键一步）

- `mp3dec_decode_frame()` 内部把 `mp3dec_scratch_t`（约 15.5KB）以及 PCM 缓冲（4.6KB）
  声明在调用者的栈上，单帧峰值约 20KB；
- 首帧探测/解码如果直接在 `app_main()` 主任务栈（默认仅 3584B）里执行，会栈溢出，
  实测会 Guru Meditation（LoadStoreError + Double Exception）或直接 TG1WDT 复位；
- 处理：把首帧探测也放到**解码任务（32KB 栈，Core1/prio10）**里执行，
  `audio_decoder_open()` 创建任务后通过二值信号量（probe_done）等待探测结果，
  探测成功后再组装 info、发 READY。

验证：
- 两个根因都修完后：`open ... 44100Hz/2ch bitrate=... duration=...`、
  `audio_decoder_play: ESP_OK`、`position ...` 心跳正常、解码到 EOF 打日志，全程无复位。

## 四、可复用的经验清单

- 小容量 SD 卡用 FAT16，不要 FAT32（大卡 >2GB 才用 FAT32/ exFAT）；
- 复位类问题先测电源（示波器 3.3V）/ 逻辑分析仪抓复位，先排除硬件再查代码；
- 调用 mp3 解码库（minimp3）时注意栈需求：不要在小任务栈/主任务（3584B）里直接解，
  放到 32KB 解码任务里，并做首帧探测；
- 解码器的大读缓冲/中间缓冲避免一次 malloc 超配额落到 PSRAM，以免带不进 cache 的
  livelock/WDT 复位；
- rst:8 (TG1WDT_SYS_RESET) 不一定是时间问题，先开 coredump 抓 PC 再下结论。
