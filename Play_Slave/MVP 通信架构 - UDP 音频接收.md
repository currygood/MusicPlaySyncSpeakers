# MVP 通信架构：单播 UDP 音频接收与主从同步

> 正式文档 · 对应工程：`Play_Slave`（ESP32-S3 N16R8 从节点）+ `Play_Master`（ESP32 主节点）
>
> 建立于 2026-10-18，2026-10-22 按协议 v2 重写。
> 从节点工程里未使用的旧 `sync_protocol` 模块已于 2026-10-22 删除；主节点工程的
> `sync_protocol` 已改造成 v2 单播发送端（同名，但内容已不是旧实现）。

---

## 一、这一版要解决什么

**一句话：主从两台机器放同一份 PCM，听感上要像一台机器在放。**

具体拆成三件事：

1. **能出声** —— 从节点收到主节点的 PCM，从喇叭放出来；
2. **出声时刻对齐** —— 主机和从机同一时刻出声，残差控制在几毫秒；
3. **长期不漂** —— 两块板子的晶振差几十 ppm，连续放一小时也不能越差越远。

上一版（8 字节头、无时间戳）只做到了第 1 条：从机收到就写 I2S，出声时刻完全由
网络到达时刻决定，和主机差出一段随机的网络延迟。自己一个人听没问题，主从一起放
就是明显的回声。

### 1.1 为什么数据面必须是单播

| | 单播 | 组播 |
| --- | --- | --- |
| 802.11 ACK / 重传 | 有 | **没有**，丢了就丢了 |
| 发送速率 | 按每个客户端自适应（可达 65~300 Mbps） | **强制基本速率**（1/2/5.5/11 Mbps） |
| AP 缓存 | 不需要 | 按 **DTIM** 缓存，唤醒间隔越大延迟越大 |
| 加密/省电 | 正常 | 部分 AP 对组播有额外限制 |

组播唯一的优势是"一份包发给 N 台"，省带宽。但本项目单台从机只有
**705.6 kbps**（44.1kHz/16bit/单声道），8 台也才 5.6 Mbps —— 在一个 802.11n
的家里连零头都算不上。用组播省这点带宽，换来的是丢包、低速、延迟不可控，
完全不划算。

现实中多房间音频产品（AirPlay 2、Google Cast、Sonos、Spotify Connect、Snapcast）
的数据面**全部是单播**。只有 Dante/AES67、运营商 IPTV 这类跑在有线/专网上的
才会用组播。

**结论：音频数据永远点对点单播；组播只用来做发现和小控制帧。**

---

## 二、拓扑

```text
┌──────────────────────────┐                    ┌──────────────────────────┐
│ 主节点（ESP32）            │                    │ 从节点（ESP32-S3）         │
│ 192.168.31.x             │   UDP 单播          │ 192.168.31.45            │
│                          │ ─────────────────▶ │                          │
│ sync_protocol (v2)       │   :5680            │ udp_audio                │
│   PCM → 20B 头 + PCM      │  每帧一份，发给每台  │   ↓ jitter buffer (64槽) │
│   t_play = t_tx + D      │  从机              │   ↓ 按 t_play 对齐       │
│   ↓                      │                    │ Amplifier_Play_Buffer    │
│ Amplifier_Play_Buffer    │                    │   ↓                      │
│   ↓                      │                    │ I2S1 → NS4168 → 喇叭      │
│ I2S0 → 功放 → 喇叭         │                    │                          │
└──────────────────────────┘                    └──────────────────────────┘

控制通道（兼容保留）：239.0.0.1:5679 组播，START / HEARTBEAT / PING / PONG
v2 的音频同步**不再依赖**控制通道 —— 从节点一包都不用回。
```

- PC 也可以用 `tools/udp_audio_sender.py` 冒充主机，直接发给从节点。
- 从节点 IP 由 DHCP 分配，在串口日志里打印；建议在路由器上做静态绑定。
- 主节点侧的从机列表是**编译期默认值 + 运行时接口**：
  `sync_protocol.c` 的 `SYNC_DEFAULT_SLAVE_IPS[]`，或调用
  `sync_protocol_master_set_slaves()`（自动发现还没做）。

---

## 三、线格式（协议 v2）

所有整数**小端**。一个 UDP 数据报 = 20 字节头 + 裸 PCM。

| 偏移 | 类型 | 字段 | 说明 |
| --- | --- | --- | --- |
| 0 | `uint16` | `magic` | 固定 `0xAA55`，不匹配直接丢 |
| 2 | `uint8` | `version` | 固定 `2`，不匹配直接丢 |
| 3 | `uint8` | `flags` | bit0 = 本流首帧；接收端用来识别"seq 归零但 stream_id 未变"的重启 |
| 4 | `uint16` | `stream_id` | 一次流唯一标识；变化 = 发送端重启/换源，清缓冲 |
| 6 | `uint16` | `seq` | 包序号，0~65535 回绕 |
| 8 | `uint64` | `t_play_us` | **该帧的基准播放时刻**（发送端时钟，µs） |
| 16 | `uint16` | `payload_len` | PCM 字节数（不含头）。接收端以实际报文长度为准 |
| 18 | `uint16` | `lead_ms` | 发送端提前量 D（ms），接收端换算时钟偏移要用 |
| 20 | `int16[]` | `pcm` | 44100 Hz / 单声道 / 16bit / 小端 |

`t_play_us` 特意排在偏移 8（8 字节对齐），避免 32 位平台上的非对齐访问。

**格式写死，没有协商。** 采样率必须等于 `AMPLIFIER_SAMPLE_RATE`
（`amplifier.h` = 44100），改一边必须改两边。

### 3.1 包长与码率

| 发送端 | 每包时长 | 采样数 | 头 + PCM | 包速率 | 码率 |
| --- | --- | --- | --- | --- | --- |
| ESP32 主节点 | 15 ms | 661 / 662 交替 | 20 + 1322/1324 | 66.7 /s | 88.2 KB/s（705.6 kbps） |
| PC 脚本默认 | 10 ms | 441 | 20 + 882 | 100 /s | 同上 |

661/662 交替是因为 44100 × 0.015 = 661.5 不是整数；交替才能保证平均采样率精确。
PC 脚本按累计采样数换算 `t_play_us`，尾片不是整帧也不会累积误差。

发送端限制单包 PCM ≤ 1400 字节（`MAX_PAYLOAD`），避免超过 MTU 1500 触发
IP 分片 —— WiFi 上分片代价很高。

### 3.2 扩容：单播的带宽账

| 从机数 | 主节点出口带宽 | 说明 |
| --- | --- | --- |
| 1 | 0.7 Mbps | 当前实测配置 |
| 4 | 2.8 Mbps | 普通家用 AP 毫无压力 |
| 8 | 5.6 Mbps | 单播上限（`SYNC_MAX_SLAVES`），还要算上 ACK 与重传开销 |
| > 16 | — | 再往上才值得重新考虑组播，或改用有线/多 AP |

---

## 四、时刻模型

### 4.1 每一帧只有一个"基准播放时刻"

主节点在发出第 i 帧时：

```text
t_tx(i)    = 现在（主节点 esp_timer，µs）
t_play(i)  = t_tx(i) + D            D = lead_ms，默认 200ms
```

`t_play(i)` 同时写进包头，并作为主节点**本地**播放的时刻基准。从节点收到包头后，
唯一要做的就是算出 `t_play(i)` 在自己时钟轴上对应几点。

### 4.2 从节点怎么换算（单向，不需要 PING/PONG）

设 θ = 从机时钟 − 主机时钟，L = 这一包的单程网络延迟。从节点收到第 i 帧时：

```text
d(i)   = 本地收到时刻 − t_play(i)  =  θ − D + L(i)
d_min  = 最近 64 个 d(i) 的最小值    ≈  θ − D + L_min
offset = d_min + D + UDIO_MARGIN_US ≈  θ + L_min + MARGIN
出声时刻 = t_play(i) + offset
```

- `d_min` 用滑动窗口取最小值，把抖动和排队延迟都滤掉，只留下最小单程延迟；
- `MARGIN = 60ms` 是抖动余量：保证抖动最大的包也能在出声前到齐；
- **从节点不需要向主节点发任何包**，主节点也不必维护 PING/PONG 状态机。

**主节点本地播放必须加同一个 MARGIN**：

```text
主节点出声 = t_play(i) + SYNC_RX_MARGIN_US
```

否则从机整体比主机晚 60ms，同一间屋子能听出回声。两边的宏
（`UDIO_MARGIN_US` / `SYNC_RX_MARGIN_US`）必须保持一致。

**残差**：只有 `L_min`（局域网通常 1~3ms）。人耳对"两台音箱之间 3ms 的差"
几乎不可察觉；真正刺耳的是随网络波动的几十毫秒差，那个已经被 offset 和
jitter buffer 消掉了。

### 4.3 为什么不用 PING/PONG

旧方案的主从同步靠控制通道往返测 RTT。实测这条通道：

- 组播 + AP 省电机制，往返 120~211ms，还带 60→105ms 的锯齿；
- 主节点每 15ms 才轮询一次控制通道，PONG 有 0~15ms 的量化误差；
- 从节点要发 PING，主节点要回 PONG，双方都多一个状态机。

单项估计把这三个问题一次性绕开：**误差上限就是 L_min，而不是 RTT/2 的抖动**。

### 4.4 时钟偏移冻结，漂移用 sample slip

`offset` 在**开始播放的那一刻冻结**，播放过程中不再改：

- 改 offset = 改所有未播帧的出声时刻 = 一次可听见的跳变；
- 晶振漂移交给 sample slip（丢/插一个样本）慢慢补。

#### 控制量：抖动缓冲的"跑道"

```text
level = 最近收到的 t_play − 正在播放帧的 t_play
```

主节点的 `t_play` 按**主节点的晶振**推进，从节点的播放头按**从节点的晶振**
前进；两个速率差几十 ppm，跑道就会匀速变长或变短：

| `level` 相对基准 | 含义 | 动作 |
| --- | --- | --- |
| > 基准 + 4ms | 从机消耗慢，数据越积越多 | 丢 1 个样本（加快） |
| 在 ±4ms 死区内 | 正常 | 不动 |
| < 基准 − 4ms | 从机消耗快，缓冲正在被追空 | 插 1 个样本（放慢） |

> **为什么不用"写入时刻 − (t_play + offset)"**：写入时刻由 I2S DMA 的排空
> 节奏决定，而 DMA 和 `esp_timer` 共用同一个晶振。每写一帧，`now` 前进的
> 微秒数正好等于 `t_play` 前进的微秒数（都是 `1e6/44100 × 采样数`），所以
> 那个差值恒为 −DMA 深度（≈ −46ms），**从机快 100 ppm 也看不出来**。
> 要观测漂移，比较的量里必须同时跨越两个晶振 —— 跑道的一端是主节点推进的
> `t_play`，另一端是从节点推进的播放头，正好满足。

- **基准不是算出来的，是测出来的**：开播头几十毫秒 DMA 从空到满，跑道会有
  一段瞬态；等积累 `UA_SLIP_WARMUP = 200` 帧（2~3 秒）后，把当时的均值定为
  基准。这样不用去算 I2S DMA 的写提前量 —— 算不准会直接变成系统性的同步误差。
- `level` 先过一层 1/32 的滑动平均，滤掉 DMA 占用和到达时刻的量化抖动；
- 路径延迟的**阶跃**（AP 换速率、路由变化）会让跑道整体平移一次，控制器会把
  它当成一次漂移、用 slip 把跑道拉回基准。结果是从机时间轴整体平移该阶跃的
  大小，属可接受的已知限制（局域网内通常 1~3ms）；晶振漂移是**斜坡**，与之
  可区分，且远小于死区时不会动作；
- 死区 4ms 是能容忍的同步误差上限 —— 两路相同信号相差 10ms 以内，人耳会
  融合成一个声像（优先效应），听不出回声；
- 两次 slip 至少间隔 20ms ⇒ 最大校正速率 50 样本/秒 ≈ **1134 ppm**，
  远大于普通晶振的 ±30~50 ppm；
- 1 个样本 = 22.7µs。**丢样本时必须真的少写一个样本**：I2S DMA 是按从机
  晶振消耗"输出样本"的，写 `n-1` 个，下一帧就会提前 22.7µs 进入 DMA，
  播放头才真的往前追了一个样本。如果丢完又把长度补齐回 `n`，时间轴一点
  没变，等于白丢一个样本、还在拼接处留一个台阶 —— 跑道会一直偏，`slip`
  以最大速率空转，每秒几十次的台阶就是"破音"。插样本同理，要真的多写
  一个；
- 拼接处做 `UA_SPLICE_TAP = 8` 点的线性过渡（≈0.18ms），插样本时取前后
  两个样本的中点，把硬台阶摊成缓变，避免每秒几次的"咔哒"声；
- 每次（重新）开播的第一帧做 `UA_FADE_IN_SAMPLES = 128` 点淡入（≈2.9ms），
  压掉 DMA 从静音切换到音频时的直流台阶；
- 跳帧（`level too deep`）后基准重新测定，因为那次跳变把跑道整体砍短了。

**绝不用 `set_sample_rate` 做漂移校正**：它内部是 disable → reconfig → enable，
会把 DMA 里未播的音频整个丢掉，还会造成一个时钟空窗。

---

## 五、从节点接收路径

### 5.1 两个任务，一条单向依赖

```text
udp_audio_rx   （prio 11 / stack 4096）
  recvfrom → 校验 magic/version → 按 seq 落进环形槽位
           → 更新 d(i) 窗口 → 记录最新 t_play
                │
                ▼  槽位 + 互斥锁
udp_audio_play （prio 10 / stack 5120）
  取下一帧 → 缓冲过深就跳 → 算 slip → Amplifier_Play_Buffer()
  丢包补 PLC、断流清盘重来
```

**持锁期间绝不调用 `Amplifier_Play_Buffer()`**（可能阻塞十几毫秒）。取帧、
算参数在锁内，写 I2S 在锁外。

### 5.2 抖动缓冲

```c
typedef struct {
    uint64_t t_play_us;
    uint16_t seq;
    uint16_t nsamples;
    uint8_t  valid;           /* 先填数据、最后置 valid */
    int16_t  pcm[1024];
} ua_slot_t;                  /* ≈ 2KB/槽 */
```

- 64 个槽（必须是 2 的幂），`seq % 64` 定位；64 × 2KB ≈ **132KB，放 PSRAM**；
  10ms 包能存 640ms，比 `UA_LEVEL_MAX_US`（450ms）留足余量；
- 按 seq 顺序消费，天然处理乱序：先到的后播，迟到的补位；
- 已经播过的包直接丢（计入 `late`），重复包丢弃（计入 `dup`）；
- `stream_id` 变化 → 整盘清零，重新 preroll。

### 5.3 启动流程（preroll）

开始出声前要攒够"提前量的一部分"，避免一开始就欠载：

```text
有效帧 ≥ 8  且  d 样本 ≥ 10  且  缓冲深度 ≥ lead × 3/4
                                      或
有效帧 > 0  且  d 样本 ≥ 5   且  发送端已停手 > 100ms
```

第二条是兜底：发送端只发了一段就停，缓冲区里剩的也要放出来。

满足后冻结 `offset`，算出第一帧的出声时刻，等到点再开始（最后 20ms 忙等）。

> 为什么按"深度"而不是"包数"：发送端只提前 `lead_ms` 发，15ms 一帧时
> 缓冲区里最多只有 ~13 帧，按"20 包"判会永远凑不齐。

### 5.4 丢包与断流

| 情况 | 处理 | 计数 |
| --- | --- | --- |
| 当前 seq 缺失 | 上一帧按 **3/4 衰减**重复（PLC），比补零柔和 | `lost` |
| 还没有任何有效帧 | 补 10ms 静音 | `lost` |
| > 500ms 没有新包 | 清盘、重新 preroll | 日志 `no data for` |
| 缓冲深度 > 450ms | 跳到"最新 − 150ms"的位置，防止延迟无限累积 | 日志 `level too deep` |

### 5.5 关键参数

| 宏 | 值 | 作用 |
| --- | --- | --- |
| `UDIO_DEFAULT_PORT` | 5680 | 监听端口 |
| `UA_RX_BUF_BYTES` | 4096 | 单次 `recvfrom` 缓冲 |
| `UA_SOCK_RCVBUF` | 64 KB | socket 接收缓冲（≈740ms 音频） |
| `UA_RECV_TIMEOUT_MS` | 200 | 只用来定期醒来，不参与播放节奏 |
| `UA_SLOTS` | 64 | 抖动缓冲槽数（2 的幂） |
| `UA_MAX_SAMPLES` | 1024 | 单帧采样上限 |
| `UA_OFFSET_WIN` | 64 | `d(i)` 滑动窗口长度 |
| `UA_PREROLL_PKTS` | 8 | preroll 最少帧数 |
| `UA_UNDERRUN_RESET_US` | 500 ms | 断流判定 |
| `UA_LEVEL_MAX_US` | 450 ms | 缓冲过深阈值 |
| `UA_LEVEL_TARGET_US` | 150 ms | 跳帧后保留的深度 |
| `UA_LEVEL_EMA_SHIFT` | 5 | 跑道观测值滑动平均（1/32） |
| `UA_SLIP_WARMUP` | 200 帧 | 开播后多久测定漂移基准 |
| `UA_SLIP_DEADBAND_US` | 4 ms | sample slip 死区 |
| `UA_SLIP_MIN_GAP_US` | 20 ms | 两次 slip 最小间隔 |
| `UA_SPLICE_TAP` | 8 点 | slip 拼接处的线性过渡点数（≈0.18ms） |
| `UA_FADE_IN_SAMPLES` | 128 点 | 每次开播首帧的淡入点数（≈2.9ms） |
| `UDIO_MARGIN_US` | 60 ms | 抖动余量，必须与主节点一致 |

### 5.6 输出侧

`Amplifier_Play_Buffer()`（`components/middlewares/amplifier/amplifier.c`）：

- I2S_NUM_1，TX，44100 Hz，单声道 16bit；BCLK=GPIO8，WS=GPIO9，DOUT=GPIO10
- 软件音量：平方曲线 `gain = (vol/100)²`，定点实现，音量 100 时零开销直通
- `timeout` 传 `UINT32_MAX` = 一直等到 DMA 有空位；**这条阻塞就是从机晶振的节拍来源**

> **功放必须独立供电。** 从开发板排针取电会在放音瞬间掉压，触发 NS4168 的
> UVLO 欠压保护 —— 静默关闭输出且不报任何错，看起来就像"收到了但没声音"。
> 排查见 `学习笔记/功放模块独立供电 - 为什么开发板电源带不动 NS4168.md`。

### 5.7 自检开关

`main/app_main.c` 的 `APP_BOOT_BEEP`（默认 0）：打开后开机响 800 Hz 方波，
**绕开网络直接调 `Amplifier_Play_Buffer()`**。

- 响 → 功放 / I2S / 喇叭 / 供电正常，问题在收包侧
- 不响 → 问题在功放供电或接线，和 UDP 代码无关

---

## 六、主节点侧改动

### 6.1 发送：组播 → 逐台单播

```c
t_play = t_tx + D;
hdr->version     = SYNC_PROTO_VERSION;     /* 2 */
hdr->t_play_us   = t_play;
hdr->lead_ms     = D;
for (i = 0; i < slave_count; i++) {
    wifi_manager_unicast_send(audio_chan, slave_ips[i],
                              SYNC_UNICAST_PORT_AUDIO, pkt, pkt_len, 0);
}
```

- 最后一个参数 **timeout = 0**：发一次就走，不等待。掉线的从机不会把
  15ms 的发送节奏拖慢（sockets 是 `SO_SNDTIMEO`，等的话每台最坏 20ms）；
- 从机表在模块内是 `char slave_ips[8][16]`，锁内拷贝到栈上、锁外再发
  （`sendto` 会进 lwIP 锁，不应持着模块锁去等）；
- 一台都配不上时只在本机播放，并在第一帧打一次 `no slave configured` 警告；
- 帧节奏、入环、背压逻辑保持不变。

### 6.2 本地播放：t_play + MARGIN

```c
int64_t play_at = ent.t_play_us + SYNC_RX_MARGIN_US;   /* 60ms */
```

`sync_delay_entry_t` 里新增 `t_play_us` 字段（原来只有 `t_tx_us`，播放时现算）。
本地延迟队列默认容量从 16 包提到 **24 包**：队列里压着的帧数 = D + MARGIN
（260ms）/ 15ms ≈ 18 帧，16 包会在稳态下溢。

### 6.3 音频 socket 改为"纯发送"

`wifi_manager_mcast_socket_create()` 现在只在 `rx_enable == true` 时才
`IP_ADD_MEMBERSHIP` + `bind()`。音频通道 `rx_enable = false`，于是：

- 不加组 → 不会收到别人发到 239.0.0.1:5680 的包；
- 不绑端口 → 源端口由协议栈临时分配，`sendto` 照常工作。

这一条是必须的：**加了组却没人读，收进来的包会一直占着 lwIP 的 pbuf 不释放**，
最终 `sendto` 返回 `ENOMEM`，控制包发不出去（旧架构里 PONG 丢失就是这么来的）。

### 6.4 控制通道的现状

控制组播 239.0.0.1:5679 仍然保留：`SYNC_START` / `SYNC_HEARTBEAT` /
`PING→PONG` 的代码都还在，但 **v2 的音频同步完全不用它**。
后续做音量、暂停、切歌下发时继续走这条通道（小帧，组播很合适）。

---

## 七、PC 发送脚本

脚本：`tools/udp_audio_sender.py`（Python 3，numpy 可选）

> 这是**代替**主节点的测试工具，不要和主节点同时往同一台从机发 ——
> 两个不同的 `stream_id` 会让从机反复清缓冲。

```bash
# 正弦自检，不依赖任何文件
python udp_audio_sender.py --host 192.168.31.45 --tone

# 放 WAV（自动降混单声道、重采样到 44.1k）
python udp_audio_sender.py --host 192.168.31.45 --file song.wav

# 循环 + 小声
python udp_audio_sender.py --host 192.168.31.45 --file song.wav --loop --volume 40

# 裸 PCM（s16le，默认 44.1k 单声道；其他格式用 --raw-rate/--raw-channels）
python udp_audio_sender.py --host 192.168.31.45 --raw test.pcm

# 完全复刻主节点帧节奏（15ms 一帧，661/662 采样交替）
python udp_audio_sender.py --host 192.168.31.45 --file song.wav --master

# 丢 2% 的包 + 0~15ms 抖动，验证从机的 PLC 和 jitter buffer
python udp_audio_sender.py --host 192.168.31.45 --tone --loop \
    --drop-pct 2 --jitter-ms 15
```

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `--host` | 必填 | 从机 IP |
| `--port` | 5680 | 从机监听端口 |
| `--file` / `--tone` / `--raw` | — | 三选一：WAV / 正弦音 / 裸 PCM |
| `--raw-rate` / `--raw-channels` | 44100 / 1 | 裸 PCM 的采样率和声道数（自动降混、重采样） |
| `--packet-ms` | 10 | 每包音频时长；太大触发 IP 分片 |
| `--lead-ms` | 200 | **必须与主节点的 D 一致**，否则 offset 换算会错 |
| `--volume` | 100 | 发送前线性衰减 0~100 |
| `--loop` | 关 | 循环 |
| `--master` | 关 | 复刻主节点：15ms 帧，661/662 采样交替（覆盖 `--packet-ms`） |
| `--drop-pct` | 0 | 随机丢包百分比，测 PLC |
| `--jitter-ms` | 0 | 每包额外 0~N ms 随机发送延迟，测 jitter buffer |

节奏控制：

```python
t_play_us = t0_play_us + samples_done * 1_000_000 // RATE
send_at   = t_play_us - lead_us
sleep_until(send_at)          # 绝对时刻，误差不累积
sendto(HDR.pack(...) + chunk)
```

- 每帧在 `t_play − lead` 时刻发出，数据总是提前 `lead` 毫秒到从机；
- 用**绝对时刻**调度而不是"每帧 sleep 10ms"：某次慢了后续帧自动追回；
  最后约 1ms 忙等，Windows 上先 `timeBeginPeriod(1)`，否则 sleep 粒度
  是 15.6ms，节奏会抖；
- 落后到这一帧已经赶不上从机出声时刻（超过 `lead + 60ms` 余量）时，
  脚本换一个 `stream_id` 重新锚定，从机看到新流会清缓冲重新对齐；
- 每秒打印一行进度（发包数、音频时长、实际耗时、漂移、丢包数）。

WAV 只支持 16-bit PCM；非 16-bit 会退出并打印可用的 ffmpeg 转换命令。

---

## 八、与旧架构的对照

| 维度 | 旧（组播 + PING/PONG） | 新（v2 单播 + 播放时刻戳） |
| --- | --- | --- |
| 数据面传输 | 组播 `239.0.0.1:5678` | **单播**，逐台发到 `:5680` |
| 包头 | 8 字节，`timestamp_us` = 发送时刻 | **20 字节**，`t_play_us` = 播放时刻 |
| 时钟同步 | PING/PONG 测 RTT，误差受往返抖动影响 | **单向最小延迟估计**，从机不回包 |
| 同步精度 | 设计目标 ±15ms，实测锁不上 | 残差 ≈ `L_min`（1~3ms） |
| 抖动缓冲 | 24 槽 × 15ms | 64 槽，按 seq |
| 丢包 | 补静音 | 上一帧 3/4 衰减重复（PLC） |
| 漂移 | 相位环 + `set_sample_rate`（会清 DMA） | **sample slip**，不动采样率 |
| 从节点任务数 | 接收 / 心跳 / 播放 三个 | **两个** |
| 从节点回包 | 每秒 PING | **零** |
| 控制面 | 全部走 5679 | 保留，但不再参与同步 |

旧实现已于 2026-10-22 从从节点工程删除（`Play_Slave/components/middlewares/
sync_protocol/` 已不存在），需要参考时查 git 历史。主节点工程里保留的同名模块是
v2 单播发送端，不是上面的旧实现。

---

## 九、故障对照表

### 9.1 串口日志的正常序列

```text
I (xxx) UDP_AUDIO: listening on UDP 5680 (unicast, proto v2, margin=60ms)
I (xxx) UDP_AUDIO: first packet from 192.168.31.x:xxxxx
I (xxx) UDP_AUDIO: stream 12345 start (lead=200ms)
I (xxx) UDP_AUDIO: playing: seq=0 nsamples=441 offset=260ms
I (xxx) UDP_AUDIO: first frame: seq=0 n=441 write=0ms level=260ms   ← 首帧准时写入
I (xxx) UDP_AUDIO: drift reference: level=214ms           ← 开播 2~3 秒后
I (xxx) UDP_AUDIO: rx=1000 lost=0 late=0 bad=0 dup=0 slip=0 level=214ms dev=0ms offset=260ms   ← 每 5 秒
```

- `level` 是缓冲跑道（已平滑），稳态值 ≈ `lead_ms` + 余量 − DMA 提前量，
  不需要和某个固定数字比对；首帧那行显示的是 DMA 还空着时的 260ms，等 DMA
  灌满（≈46ms）后会落到 214ms 附近，所以基准取在 2~3 秒后；
- `write` 是首帧实际写入 I2S 的时刻相对 `start_at` 的偏差，正常是 0；
- `dev` 是 `level` 相对基准的偏差，应该长期在 ±4ms 内小幅波动；越过死区时
  `slip` 才会动作；
- `slip` 长期平均 ≈ 晶振差 × 44100 个/秒（50ppm ≈ 2 个/秒）；`dev` 在死区
  内时它完全不动，越界期间每个 slip 校正 22.7µs，按漂移速率均匀补；
- `lost` 应为 0 或偶发。

### 9.2 现象 → 查什么

| 串口/现象 | 含义 | 查什么 |
| --- | --- | --- |
| 一直停在 `listening`，没有 `first packet` | 一个包都没到 | 从机 IP、PC/主机是否同网段、防火墙、端口 |
| `bad` 在涨 | 收到了但 magic/version 不对 | 端口被占用、发送端还在用 v1 头 |
| `rx` 在涨、`bad=0`、但没声音 | 包和处理都正常 | **功放供电**（打开 `APP_BOOT_BEEP` 确认）、音量是否为 0 |
| 没有 `playing:` 日志 | preroll 没满足 | 发送端是不是只发了很少几包就停；`lead_ms` 是否太小 |
| `lost` 持续增长 | 真的丢包 | WiFi 信号、`SO_RCVBUF`、发送端发包过快 |
| `late` 增长 | 包到得比播放点还晚 | MARGIN 太小，或网络延迟抖动大 |
| `dup` 增长 | 重复包 | 发送端重复发送 / 多台主机发同一流 |
| `slip` 持续爆发、`dev` 总在同一侧 | 两端晶振差大 | 换一块板子对比；普通晶振 ±30~50ppm 对应约 2 个 slip/秒 |
| `slip` 一直涨但 `dev` 不回落 | 丢样本没有真的改变输出长度（时间轴没变） | 检查 `ua_write_frame`：丢样本必须输出 `n-1` 个，不能补回 `n` |
| `level` 周期性撞到 450ms | 漂移控制失效，缓冲被一路撑大或掏空 | 看 `dev` 和 `slip`；也可能是 CPU 被别的任务长期抢占 |
| 声音断续、周期性空洞 | 缓冲欠载 | `lost`/`late`；发送端打印的漂移 |
| 有杂音/爆音 | 包边界断裂、数据错位，或 slip 拼接台阶 | 采样率是否 44100、`payload_len` 与实际长度；`slip` 是否在涨（拼接平滑是否生效） |
| `no data for 500ms, re-preroll` | 发送端停了或链路断了 | 正常现象，恢复后会自动重新对齐 |

### 9.3 已知限制

1. **无重传**：丢一个包就是一个 PLC 帧，听感上是轻微的"糊"一下。
2. **单向估计分不开"时钟差"和"最小路径延迟"**：`L_min` 的那 1~3ms 会被算进
   offset，表现为从机整体晚 1~3ms —— 听不出来，但如果做声学测量能测到。
3. **多从机没有实测**：协议支持 8 台，每台独立估计自己的 offset，互不影响；
   但回环、多 AP、多设备并发没有验证过。
4. **从机地址是手动配置**：自动发现（从机上线广播 / mDNS）还没做。
5. **控制命令没接**：音量/暂停/切歌仍走旧控制通道，从节点当前不处理；
   切歌表现为 `stream_id` 变化 → 清缓冲重来，行为正确但少一次淡入。

---

## 十、文件位置

| 文件 | 作用 |
| --- | --- |
| `Play_Slave/components/middlewares/udp_audio/udp_audio.h` | 线格式、常量、API |
| `Play_Slave/components/middlewares/udp_audio/udp_audio.c` | 接收任务 + 播放任务 |
| `Play_Slave/main/app_main.c` | 装配：WiFi → 功放 → `udp_audio_start()` |
| `Play_Master/components/middlewares/sync_protocol/sync_protocol.{h,c}` | 主节点 v2 发送端 |
| `Play_Master/components/msystem/wifi_manager/wifi_manager.{h,c}` | 单播发送 API + 纯发送 socket |
| `Play_Master/main/app_main.c` | 打开音频通道（`SYNC_UNICAST_PORT_AUDIO`，只发） |
| `Play_Slave/tools/udp_audio_sender.py` | PC 端发送脚本 |

---

## 十一、下一步

按性价比排序：

1. **从机自动发现** —— 从机上线时往 5679 发一条"我上线了"，主节点收集；
   省掉手动配 IP 和 DHCP 静态绑定。
2. **控制命令接回从节点** —— 音量、暂停、恢复不需要重新对齐时钟，
   直接复用 5679 的小控制帧。
3. **丢包重传（NACK）** —— 在 `lead_ms` 的 200ms 窗口内还有机会补，
   比 PLC 好得多。代价是从节点要发 NACK，主节点要留一份最近发送的帧。
4. **多从机实测** —— 先验证 4 台，重点看 AP 的 airtime 和每台的残差。
