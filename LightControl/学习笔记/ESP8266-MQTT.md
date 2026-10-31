# ESP8266 的 MQTT 笔记 —— 以及它怎么和 TCP / UDP 同时用

> 环境：ESP-01S（**1MB** flash）· ESP8266_RTOS_SDK v3.4（`v3.4-115-g858c7c2e`）
>
> 这份笔记对着项目里的这些文件写：[mqtt_link.c](../main/mqtt_link.c)、
> [mqtt_link.h](../main/mqtt_link.h)、[link.c](../main/link.c)、
> [cmd.c](../main/cmd.c)、[main.c](../main/main.c)。
>
> SDK 的结论都回查了源码，文中标到 `文件 + 符号名`（行号会随代码改动腐烂，符号名不会）；
> 项目自己的用法直接给文件名。
>
> 本文只依据 `main/` 里的源码和 [README.md](../README.md) 写。

---

## 1. 先纠正一个前提：MQTT 和 TCP 不是二选一的关系

这个问题问的是"为什么 MQTT 和 TCP 可以一起用"，但严格说，里面藏着一个错误的假设。
先看 MQTT 站在哪一层：

```text
        ┌──────────────────────────────────────┐
        │  MQTT     应用层                      │  主题、QoS、心跳、遗嘱
        │  "往 kChtBO89U002/up 发一条"          │  它【不知道】字节是怎么送过去的
        ├──────────────────────────────────────┤
        │  TCP      传输层                      │  连接、重传、按序、流量控制
        │  "把这段字节可靠地送到对端"            │  它【不知道】里面装的是 MQTT 还是别的
        ├──────────────────────────────────────┤
        │  IP / lwIP                            │
        ├──────────────────────────────────────┤
        │  WiFi 驱动 + 空口（2.4GHz）            │
        └──────────────────────────────────────┘
```

**MQTT 本来就跑在 TCP 上。** 它不是 TCP 的替代品，而是 TCP 的**用户**：
`mqtt_link.c` 里那些 `esp_mqtt_client_publish()`，往下最终走的就是
`lwIP` 的 `send()`。所以"MQTT 和 TCP 能不能一起用"这个问法，
就像在问"快递和卡车能不能一起用"。

那"三选一"的印象是哪来的？来自**本项目自己的命令表**（[`cmd.c` 里的 `s_cmds`](../main/cmd.c)）：

| 命令 | 换的是哪一层 |
| --- | --- |
| `net tcp` / `net udp` | **传输层**：跟 ESP32 那段用 TCP 还是 UDP |
| `mqtt on` / `mqtt off` | **应用层**：要不要再挂一条到 broker 的 MQTT |

把它们并列成三条"链路"，是为了让操作命令好记，不是因为它们在同一层。
**真正互斥的是 TCP 和 UDP**，理由见 §3.1；MQTT 和这两条谁都不冲突。

> ### 为什么
>
> 一层协议能同时被多个上层用，也能同时用多个下层 —— 这是分层的全部意义。
> 一个进程可以同时开着一条 TCP 连接、一条 UDP 套接字、外加一个跑在第三条
> TCP 连接上的 MQTT 客户端，三者互不干扰，因为它们在 lwIP 里是**三个独立的
> `socket`**，各有各的收发缓冲区、各有各的任务。
>
> 所以"能不能同时用"从来不是协议层面的问题，**是这个项目自己的代码组织
> 得不得当的问题**。下一节就是这个。

---

## 2. 为什么改之前的代码做不到

改造之前，`link.c` 只有一条主链路，靠一个全局变量记着：

```c
static link_mode_t s_link = LINK_TCP;   /* ← 已经不存在了 */
```

所有链路**共用**这一个值。这在"同时只有一条链路开着"时是够用的，
要同时开两条就撞上三堵墙。

### 2.1 一个变量装不下两个答案

`s_link` 只能等于 `LINK_TCP`、`LINK_UDP` **之一**。问它"MQTT 那一路开着吗"，
它答不上来 —— 它压根没有那个字段。

连"回执该发给谁"都答不上来：`link_send()` 当时是
`link_send(const char *data, int len)`，没有"发给哪条"这个参数，
它只能自己去看 `s_link`。

```text
现在的 link_send：  link_send(LINK_MQTT, "LED ON\r\n", 0)   ← 发送方写在参数里
以前的 link_send：  link_send("LED ON\r\n", 0)              ← 函数自己猜
```

**猜错的后果**：命令从 MQTT 进来、回执却发去了 TCP。发命令的人
（手机上那个小程序）什么都收不到，而 ESP32 那边莫名其妙多出一句
`LED ON` —— 两边都会以为固件坏了。

### 2.2 三条回调跑在三条任务上

这是最硬的一堵墙。三条链路的接收回调不是一个任务里排队进来的，
而是**三条任务各跑各的**：

| 任务 | 优先级 | 栈 | 谁建的 |
| --- | --- | --- | --- |
| `sys_evt` | 10 | — | SDK 的 `esp_event_loop_create_default()` |
| `tcp_client` | 5 | 4096 | [`tcp_client_start()`](../main/tcp_client.c) |
| `udp_client` | 5 | 4096 | [`udp_client_start()`](../main/udp_client.c) |
| `mqtt_task` | 5 | 6×1024 | esp-mqtt 自己建的（`MQTT_TASK_PRIORITY` / `MQTT_TASK_STACK`，见 §6.5） |
| `wifi_mgr` | 4 | 6144 | [`wifi_sta_init()`](../main/wifi_sta.c) |

三个 prio 5 的任务是**平级**的，FreeRTOS 让它们按时间片轮转。
也就是说：`cmd_on_tcp_rx()` 和 `cmd_on_mqtt_rx()` 可能在任何一刻被对方打断。

> **为什么这件事是致命的**
>
> 假设还是用全局 `s_link` 记"当前链路"，而两条同时在收：
>
> ```text
> tcp_client 任务：读到 s_link == LINK_TCP，准备把回执发去 TCP
>        ↓ 被时间片打断
> mqtt_task  任务：执行命令，把 s_link 改成 LINK_MQTT
>        ↓ 切回来
> tcp_client 任务：发出去了 —— 但用的是【刚被改过的】s_link，发去了 MQTT
> ```
>
> 这叫**竞态**。它不会每次都发生，只在恰好的时机发生 —— 也就是
> 最难查的那种 bug。修法不是加锁，是**根本不要有这个全局变量**：
> 让"谁问的"跟着参数一路传下去，每个任务手里的答案都是自己的，谁也改不了谁的。

### 2.3 一份命令缓冲区会串味

`cmd.c` 当时只有一份攒字节的缓冲区。两条链路各收半条，会拼成一条
谁都没发过的命令：

```text
MQTT 上来个 "开"    →  缓冲区里躺着 "开"，还没拼成完整命令
        ↓  与此同时
TCP  上来个 "灯"    →  缓冲区里凑成了 "开灯"
        ↓
→ 执行了一次开灯，而【没有任何一方发过这两个字】
```

这和 §3.2 里说的"换链路时缓冲区串味"是同一类问题，
只是这里连"换"都不用 —— 两条链路本来就在同时收。

---

## 3. 改造：三处，各管一件事

### 3.1 `link.c` —— 一个主链路 + 一个独立开关

```c
static link_mode_t s_master = LINK_TCP;   /* 主链路：TCP 或 UDP，二选一 */
static bool        s_mqtt_on = false;     /* MQTT 那一路：独立的一个开关 */
```

两个变量，而不是一个 —— 这就是"能同时开两条"的全部秘密。
它俩的语义完全不同：

| | `s_master` | `s_mqtt_on` |
| --- | --- | --- |
| 取值 | `LINK_TCP` / `LINK_UDP` | `true` / `false` |
| 为什么不这样 | **互斥** —— 两条都是跟同一个 ESP32 说话的路 | **独立** —— 对端是 broker，不是那个 ESP32 |
| 谁能改 | `link_init()`、`link_switch()` | `link_init()`、`link_set_mqtt()` |

**为什么 TCP 和 UDP 必须互斥？** 因为它们连的是同一台 ESP32、同一个端口。
两条都开的话，主节点发一条"开灯"会同时从两条路到达，
`cmd_try_one()` 执行两次、回执也发两条。继电器是幂等的，
执行两次看不出问题 —— 但 `wifi DESKTOP,12345678` 这种就不一样了：
两次写入会把同一份凭据写两遍 NVS（无害，但白写），
而配网失败的那次重试会翻倍。**没有收益，只有风险，所以写死互斥。**

**MQTT 为什么不在这条约束里？** 它的对端是巴法云的 broker，
跟 ESP32 是两回事。两边各说各的，谁也不碍谁 —— 这正是加这一路的目的：

```text
                    ┌──────────────────────────────┐
                    │        本节点（ESP-01S）      │
                    │                              │
   ESP32  ──TCP───▶ │  主链路：tcp_client.c        │
    或      或      │  （或 udp_client.c，二选一）  │
   ESP32  ──UDP───▶ │                              │
                    │  第二路：mqtt_link.c          │ ──TCP──▶ 巴法云 broker
                    │  （独立开关）                 │
                    └──────────────────────────────┘
```

### 3.2 `cmd.c` —— `src` 一路传下去，每条链路一份缓冲区

**第一处**：缓冲区从一份变成三份（[`cmd.c` 里的 `cmd_rx_t`](../main/cmd.c)）：

```c
typedef struct {
    char buf[CMD_BUF_SIZE + 1];   /* 攒字节的地方，+1 留给结尾的 '\0' */
    int  len;                     /* 里面当前有多少个有效字节 */
    volatile bool stale;          /* 这条链路被关过，里面可能留着半条命令 */
} cmd_rx_t;

static cmd_rx_t s_rx[LINK_COUNT];   /* 下标就是 link_mode_t */
```

> **为什么这样就免锁了**
>
> `s_rx[0]`、`s_rx[1]`、`s_rx[2]` 是三块**不同的内存**。tcp 任务只碰
> `s_rx[LINK_TCP]`，mqtt 任务只碰 `s_rx[LINK_MQTT]` —— 两个人从头到尾
> 没碰过同一块地方，那就不需要锁。
>
> ⚠️ 反过来说，**"只碰自己那一份"是个必须守住的纪律**。改造时差点
> 在这里写错一次：`cmd_try_one()` 换完链路后想顺手把另一条链路的缓冲区
> 清掉，那是一句 `memcpy` 对着别人正在写的内存 —— 典型的跨任务撞车。
> 最后的做法是**只挂标志**（`stale = true`），由那条链路自己的任务
> 在下次收数据时清（[`cmd_on_rx_from()`](../main/cmd.c)）。

**第二处**：每个函数多一个 `src` 参数，一路传到 `link_send()`：

```text
cmd_on_tcp_rx  ─┐
cmd_on_udp_rx  ─┼→ cmd_on_rx_from(src, …) → cmd_on_rx(src, …) → while (cmd_try_one(src))
cmd_on_mqtt_rx ─┘      关掉的丢掉/清缓冲        攒字节+找命令      挑最靠前那条执行
```

三个 `cmd_on_*_rx()` 每个只有一行，就是把 `src` 填死之后转给
`cmd_on_rx_from()`：

```c
void cmd_on_mqtt_rx(const char *data, int len)
{
    cmd_on_rx_from(LINK_MQTT, data, len);
}
```

**入口认 `src`，出口也认 `src`**，全程当参数传 —— 全局变量就这么消掉了。
`link_send()` 现在长这样（[`link.c` 里的 `link_send()`](../main/link.c)）：

```c
int link_send(link_mode_t to, const char *data, int len)
{
    switch (to) {
    case LINK_UDP:  return udp_client_send(data, len);
    case LINK_MQTT: return mqtt_link_send(data, len);
    default:        return tcp_client_send(data, len);
    }
}
```

### 3.3 `main.c` —— 三个回调，外加"先等 IP"

```c
tcp_client_set_rx_handler(cmd_on_tcp_rx);
udp_client_set_rx_handler(cmd_on_udp_rx);
mqtt_link_set_rx_handler(cmd_on_mqtt_rx);

wifi_sta_wait_ip(UINT32_MAX);   /* ← 等 WiFi 真的拿到 IP */

link_init(LINK_TCP);
```

**三条都要注册**：现在最多两条同时在收，每条都得有自己的入口。

**为什么 `link_init()` 之前要等 IP**：MQTT 的 broker 是**域名**
（`mqtt.bemfa.com`），没有 IP 就没有 DNS。不等也能跑（esp-mqtt 会自己
每 10 秒重试一遍），但那不是"能用"，只是"没崩"。`wifi_sta_wait_ip()`
不清事件位，所以同时等在同一个位上的 tcp / udp 两条任务不受影响；
而它们本来就在自己任务里等着同一个 IP，主链路因此**没有任何可观察的变化**。

> 顺带的好处：连 30 次失败转去 `ap_prov.c` 配网时，`app_main` 就停在
> 这一行等着；新凭据提交成功、WiFi 真起来之后才继续往下走。
> 也就是说不管走哪条路（首次上电 / 日常重连 / 配网救回来），
> MQTT 都是**先有 IP 才启**，不用额外加判断。

---

## 4. 一条命令的完整旅程

从 MQTT 收到 `on` 开始，到回执推回云端为止：

```text
mqtt_task（prio 5，栈 6KB）                      ← esp-mqtt 自己的任务
  └─ broker 推来一条消息 → MQTT_EVENT_DATA
       └─ s_rx_handler(data, len)               ← 纯传输层到此为止，它不知道"开灯"
            └─ cmd_on_mqtt_rx(data, len)        ← main.c 里注册的
                 └─ cmd_on_rx_from(LINK_MQTT, …)
                      ├─ link_is_open(LINK_MQTT)？ 否 → 丢掉，到此为止（第一道闸）
                      ├─ s_rx[LINK_MQTT].stale？  是 → 先把自己这份清干净
                      └─ cmd_on_rx(LINK_MQTT, …)  ← 攒进【自己那份】缓冲区
                           └─ while (cmd_try_one(LINK_MQTT))
                                ├─ 扫 s_cmds，挑【位置最靠前】的那条
                                ├─ relay_on()                        ← 唯一真正动硬件的地方
                                └─ link_send(LINK_MQTT, "LED ON (GPIO0 = HIGH)\r\n", 0)
                                     └─ mqtt_link_send()
                                          └─ esp_mqtt_client_publish(…, "kChtBO89U002/up", …)
```

同一时刻，ESP32 从 TCP 发来的 `off` 走的是**另一条完全平行的路**
（`tcp_client` 任务 → `s_rx[LINK_TCP]` → `relay_off()`），
两条路除了最后都去碰一下 `relay.c` 的 GPIO 之外，没有任何共享状态。

> `relay.c` 那边要不要加锁？不用。`relay_on()` / `relay_off()` 就是一次
> `gpio_set_level()`，写的是同一个寄存器的一位。两条命令同时到，
> 结果不是"开"就是"关"，取决于谁后写 —— 而这恰好就是"后到的命令赢"，
> 正是想要的行为。中间不会出现一个既不是开也不是关的状态。

---

## 5. 巴法云的两个发布口：`/up` 和 `/set`

订的是 `kChtBO89U002`，发的时候在它后面接一个后缀，语义完全不同：

| 发布到 | 谁收得到 | 用途 | 命令表里对应 |
| --- | --- | --- | --- |
| `kChtBO89U002/up` | **所有订阅了这个主题的设备**，但**发布者自己收不到** | 通知同伴：事儿办了 | 所有回执都走它 |
| `kChtBO89U002/set` | **谁都不推**，只更新云端记着的值 | 让云端的值跟上，不惊动别人 | `cloud on` / `cloud off` |

"发布者自己收不到"是 broker 的规矩，不是本模块的 —— 所以手机上点一下
`/up`，手机自己看不到那条消息，板子却收到了。

### 5.1 ⚠️ 回执文案必须**全大写**

这是改造过程中真找出来的一个坑，值得单独说。

既然回执现在会推到 `/up` 上**广播出去**，那么对面要是也跑这套固件，
它就会把我的回执**当成一条命令**去解析。而命令表里有裸的 `on` / `off`
（ASCII 别名，小写）：

```text
我发出去的回执：  "WIFI OK (reconnecting)\r\n"
                          ↑↑
对面节点解析：    命中 "on" → relay_on() → 【对面的继电器吸合了】
```

一句人畜无害的状态回执，让隔壁那台设备把灯打开了。

**规矩**：`link_send()` 发出去的每一个字符串，都不许含有命令表里的关键字，
**一律全大写**（`ON` 不会命中 `on`，因为匹配是区分大小写的按字节比较）。
上面那句已经改成了 `"WIFI OK (RECONNECTING)\r\n"`。

> 这条规矩的根本原因：**`/up` 是一个回环**。我推上去的东西会回来的
> —— 回到别的设备上，而那台设备的行为跟这台一模一样。
> 只要系统里有"广播 + 把收到的都当命令"这两件事凑在一起，
> 就一定有这种自激回路。

### 5.2 `cloud on` / `cloud off` 为什么必须带前缀

因为裸的 `on` / `off` 是**真拉继电器**的，而 `cloud on` 只是"把云端那个值
改成 on"，板子一动不动。两者必须能分清。

分开靠的是"**挑缓冲区里位置最靠前的那条**"这条规则（[`cmd_try_one()`](../main/cmd.c)）：

```text
缓冲区 = "cloud on"
           ↑        ↑
           第 0 位    第 6 位也命中 "on"

→ 两个都命中，但 "cloud on" 的起始位置更靠前 → 执行 ACT_CLOUD_ON
```

`mqtt on` / `mqtt off` 用的是同一条规则。**这也是为什么命令表必须整张扫完
再决定，而不是"找到第一条就返回"** —— 后者会让 `cloud on` 执行成开灯。

---

## 6. MQTT 这一路特有的坑

### 6.1 域名 → 要 DNS

`mqtt.bemfa.com` 是域名，要先解析成 IP。所以这一路需要一个**真的能上外网、
会下发 DNS 的热点**：

| 上联是什么 | MQTT 能通吗 |
| --- | --- |
| 手机热点 / 路由器（能上外网） | ✅ |
| ESP32 自己开的 SoftAP | ❌ 它只发 IP，没有 DNS 服务器，解析不出域名 |
| 能连上但外网断了的路由器 | ❌ DNS 可能通但连不上 broker |

所以在"只有 ESP32、没有外网"的现场，串口上会看到它一直在重连。
**不想要就发一条 `mqtt off` 让它安静** —— 主链路一点不受影响。

### 6.2 `mqtt_link_stop()` 为什么**不断开**连接

```c
void mqtt_link_stop(void)
{
    s_running = false;    /* 就这一行 */
}
```

刻意**不**调用 `esp_mqtt_client_stop()`。那个函数有两个坑，踩中任何一个
都会让整条 MQTT 链路废掉：

| 坑 | 后果 |
| --- | --- |
| ① 它发现自己是跑在 MQTT 自己的任务里时，直接返回错误，**而返回前没有把 API 锁放掉** | 那是一把递归锁，漏掉一次 take，之后任何任务再调 MQTT 的 API 都会**永久卡死** |
| ② 就算不在 MQTT 任务里，它也会阻塞等待那条任务退出（`portMAX_DELAY`，无限等） | 和另外两个传输模块"只置标志、立刻返回"的约定正好相反 |

**坑 ① 是必踩的**：`mqtt off` 这条命令从 MQTT 进来 → `cmd_on_mqtt_rx()`
→ `cmd_try_one()` → `link_set_mqtt()` → `mqtt_link_stop()`，
整条链**都跑在 `mqtt_task` 自己的栈上**。这跟
`ESP8266-TCP-UDP-WiFi-STA.md` §5.4 里 `tcp_client_stop()` 那条死锁链
是同一回事，只是这次不是一个死循环，而是**一把没放掉的锁**。

**代价**：关掉之后连接、socket、那条 6KB 栈的任务都还留着。
真正让数据停下的是 `link_is_open(LINK_MQTT)` 那道判断（第一道闸）。
好处是再打开是**立刻**的 —— 不用重连、不用重新订阅。

> 如果将来真发现内存不够，正确的做法是让 esp-mqtt 别自己重连
> （`disable_auto_reconnect`）再另起一条任务管生命周期，
> **而不是**把 `esp_mqtt_client_stop()` 塞进 `mqtt_link_stop()` 里。

### 6.3 一次给一条完整消息，但仍然要攒

TCP 给的是**字节流**，一次 `recv()` 可能只有半条命令；MQTT 给的是
**一条条完整的消息**。但 `mqtt_link.c` 还是把它丢进了 `cmd.c` 那套
"攒起来再找关键字"的逻辑 —— 因为这样三条链路才能共用同一张命令表。

有一处仍然要攒：**报文超过收发缓冲区时，`MQTT_EVENT_DATA` 会分几次来**
（`event->data_len` 是这一段的长度，`event->total_data_len` 是整条的）。
所以 `mqtt_link.c` 用一个 `MQTT_RX_BUF_SIZE`（128 字节）的小缓冲区
把片段拼起来，**拼够 `total_data_len` 才交给上层**。
超过 128 字节的整条丢掉 —— 命令只有几个字节，攒到 128 还没完，
说明对方发来的根本不是命令。

### 6.4 QoS 0：发出去了 ≠ 对方收到了

订阅和发布都用 QoS 0（最多一次，巴法云自己的例程也是这个）。
`mqtt_link_send()` 返回成功，**只说明字节进了协议栈**，
不代表 broker 收到了、更不代表别的设备收到了。

升到 QoS 1 的话 broker 会重发，我们这边就得多处理"同一条命令到两次"。
开关灯是幂等的，多一次也无所谓 —— **收益不值这个复杂度**，所以没升。

### 6.5 那个 6KB 的任务

esp-mqtt 自己建一条任务，名字叫 `mqtt_task`
（SDK 的 `mqtt_client.c` 里 `xTaskCreate(esp_mqtt_task, "mqtt_task", …)`）：

| 参数 | 值 | 哪来的 |
| --- | --- | --- |
| 优先级 | 5 | `MQTT_TASK_PRIORITY`（`mqtt_config.h`，sdkconfig 没覆盖，用默认值） |
| 栈 | 6×1024 = 6144 字节 | `MQTT_TASK_STACK`（同上） |

优先级 5 和 `tcp_client` / `udp_client` **平级** —— §2.2 里那个竞态
就是这么来的。栈 6KB 是这条链路最实在的代价：1MB flash 的板子上，
这条任务不关就一直占着。

### 6.6 每次重连都要重新订阅

`clean session` 是开着的，broker **不会**替我们记着订阅关系。
所以 `MQTT_EVENT_CONNECTED` 里第一件事就是
`esp_mqtt_client_subscribe(s_client, MQTT_TOPIC, MQTT_QOS)`。

漏了这一步的现象很有欺骗性：**日志说连上了，但发消息过来没反应**。
"连上"和"订上"是两件事。

---

## 7. 代价与已知取舍

| 取舍 | 说明 |
| --- | --- |
| 同一条命令两条链路都来 | 会**执行两次**。继电器幂等，无所谓；`wifi` 不是 —— 会往 NVS 写两遍 |
| 6KB 任务常驻 | `mqtt off` 之后连接和任务都留着（理由见 §6.2） |
| `link_switch()` / `link_set_mqtt()` 没加锁 | 两条链路同一瞬间各发一条换向命令，是"谁后写谁赢"。不崩、也不会错到哪去，但**不是严格互斥** |
| 回执文案受约束 | 必须全大写，理由见 §5.1 |

> 最后一条（没加锁）是**有意留着**的：要真撞上，得两条链路在同一毫秒里
> 各收到一条换向命令。为它加一把锁，代价是每个命令路径都要过一遍锁，
> 外加一处可能的死锁 —— 不值。

---

## 8. 一页速查

```text
分层：
  MQTT 是应用层，跑在 TCP 上 —— 它和 TCP 【不是】二选一
  命令表把它们并列成三条"链路"，是为了好记，不是因为同层
  真正互斥的是 TCP 和 UDP（同一个对端，两条都开会被执行两次）

同时用的机制（三处改动）：
  link.c    s_master（TCP/UDP 二选一）+ s_mqtt_on（独立 bool）
  cmd.c     s_rx[LINK_COUNT] 每条链路一份缓冲区（免锁）+ src 一路传下去
  main.c    三个 set_rx_handler + wifi_sta_wait_ip 之后才 link_init

为什么必须这么改：
  三条回调跑在三条 prio 5 的任务上，全局变量会被互相覆盖（竞态）
  一份缓冲区会让两条链路的半条命令拼成一条谁都没发过的命令
  → 所以"谁问的"只能靠【参数】传，不能靠全局状态

巴法云：
  <主题>/up    推给所有订阅者，发布者自己收不到 —— 所有回执走它
  <主题>/set   谁都不推，只更新云端的值 —— cloud on / cloud off 走它
  ⚠ 回执必须全大写："(reconnecting)" 里那个小写 "on" 会让对面节点吸合继电器
  ⚠ cloud on / mqtt on 靠"位置最靠前"规则区分于裸的 on

MQTT 特有的坑：
  域名要 DNS → ESP32 的 SoftAP 没有 DNS，这一路走不通
  stop() 只置标志，【不能】调 esp_mqtt_client_stop()（漏放递归锁 → 永久卡死）
  报文超过缓冲区会分几段来，要按 total_data_len 拼齐
  QoS 0：返回成功只说明进了协议栈
  clean session → 每次重连都要重新订阅，漏了的现象是"连上了但收不到"
  mqtt_task：prio 5、栈 6KB，和 tcp/udp 平级

任务优先级：sys_evt 10 · tcp_client / udp_client / mqtt_task 5 · wifi_mgr 4
```

---

## 相关文档

- [README.md](../README.md) —— 项目定位与拓扑
- [ESP8266-TCP-UDP-WiFi-STA.md](ESP8266-TCP-UDP-WiFi-STA.md) —— TCP / UDP / WiFi-STA
- [ESP8266-NVS.md](ESP8266-NVS.md) —— 热点凭据存在哪
- [ESP8266-AP配网架构.md](ESP8266-AP配网架构.md) —— 连不上之后的配网兜底
- [ESP8266开发流程.md](ESP8266开发流程.md) —— 工具链、编译烧录、启动日志
- [mqtt_link.h](../main/mqtt_link.h)、[link.h](../main/link.h)、[cmd.h](../main/cmd.h)
  —— 各模块的接口契约
