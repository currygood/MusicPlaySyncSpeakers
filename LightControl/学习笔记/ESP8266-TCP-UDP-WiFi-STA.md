# ESP8266 的 TCP / UDP / WiFi-STA 笔记

> 环境：ESP-01S（**1MB** flash）· ESP8266_RTOS_SDK v3.4（`v3.4-115-g858c7c2e`）
>
> 这份笔记对着项目里的这些文件写：[tcp_client.c](../main/tcp_client.c)、
> [udp_client.c](../main/udp_client.c)、[wifi_sta.c](../main/wifi_sta.c)；
> §5 里还会用到 [cmd.c](../main/cmd.c)、[link.c](../main/link.c)、
> [relay.c](../main/relay.c)。
>
> SDK 的结论都回查了源码，文中标到 `文件 + 函数/宏名`（行号会随代码改动腐烂，符号名不会）；
> 项目自己的用法直接给文件名。
>
> 配套阅读：[ESP8266-NVS.md](ESP8266-NVS.md)（凭据存在哪）、ESP8266开发流程.md §9.2（拓扑与四值表）、§9.7（连不上怎么查）

## 1. TCP 和 UDP 的区别

### 1.1 一张表

| | TCP | UDP |
| --- | --- | --- |
| 全称 | Transmission Control Protocol | User Datagram Protocol |
| 中文 | 传输控制协议 | 用户数据报协议 |
| 连接 | **有**，必须先 `connect()` | **无**，socket 建好就能发 |
| 角色 | 客户端 / 服务端，不对称 | 没有角色，完全对称 |
| 谁先动 | **只能客户端先拨**，服务端等着 | 谁都可以先发 |
| 消息边界 | **没有** —— 是字节流 | **有** —— 一次 `sendto` 对应一次 `recvfrom` |
| 可靠性 | 确认、重传、排序、去重 | 什么都不保证 |
| 发出去之后 | 最终能知道对方收没收到 | 完全不知道，也不知道对方死活 |
| 对端地址 | `connect()` 时定死 | 每次 `sendto()` 都要重新填 |
| 对端怎么认识我们 | `accept()` 时就知道了 | 只能靠**我们先发一包** |
| 头部开销 | 20 字节起 | 8 字节 |
| 本项目端口 | `TCP_SERVER_PORT` = 8086 | `UDP_PORT` = 8087 |

### 1.2 三条会真正咬人的区别

#### ① 连接：决定了"谁知道谁"

TCP 是**我们拨出去**的，所以对端 `accept()` 的那一刻就知道我们的 IP 和端口。
这带来一个白拿的好处：**主节点不需要我们自报家门**。

UDP 没这回事。主节点那边**根本不知道我们存在** —— 没有连接可建，它手上没有任何
关于我们的信息。所以 `udp_client.c` 一上来就得先喊一包
（[udp_client.c](../main/udp_client.c) 的 `udp_announce()`），
主节点才能从**源地址**学到"该往哪儿回"。

> 这一条直接决定了回包规则：
>
> - TCP 模式：主节点对每个 `accept()` 出来的连接回包，天然一对一
> - UDP 模式：主节点得维护一张"谁在哪"的表，或者干脆约定"回源 IP:8087"
>   —— 本项目用的是后者，所以 `udp_client.c` 才要 `bind()` 一个**固定端口**
>   （[`udp_client.c` 里的 `udp_client_task()`](../main/udp_client.c)）

#### ② 消息边界：拆包与粘包

**TCP 是字节流，不是消息队列。** 你在网络调试助手里点一次"发送"，ESP 这边 `recv()`
收到的可能是（[`cmd.c` 里的 `CMD_BUF_SIZE` 宏](../main/cmd.c)）：

```text
① 一次收到完整的 "开灯"        ← 最理想
② 分两次收到 "开" 和 "灯"      ← 被网络拆开了
③ 一次收到 "开灯关灯"          ← 你连点了两次，粘在一起了
```

所以**绝对不能拿每次 `recv()` 到的内容直接去 `strcmp()`**。项目的做法是先把字节攒进
缓冲区、再在缓冲区里找关键字（[`cmd.c` 里的 `cmd_on_rx()`](../main/cmd.c)）。

**UDP 保留消息边界**：发一次 = 收一次，一包就是一包，② 那种情况不会发生
（[udp_client.h](../main/udp_client.h)）。

> ⚠️ 但 **③ 还是会发生** —— 发送方完全可以在一个数据报里塞两条命令。
> 所以那套"攒起来再找关键字"的写法**照旧必须留着**。
> UDP 只是帮你消掉了一半的坑，另一半还在。

#### ③ 可靠性：为什么 UDP 模式必须有心跳

| | TCP | UDP |
| --- | --- | --- |
| 丢包 | 自动重传 | 丢了就丢了，收不到拉倒 |
| 对端重启 | 连接会断，`recv()` 返回 0，立刻知道 | **毫无感知** |
| 本地怎么发现链路断了 | `send()`/`recv()` 报错 | 没辙，只能上层自己约心跳 |

第三种情况是本项目加心跳的真正原因（[`udp_client.c` 里的 `UDP_ANNOUNCE_MS` 宏](../main/udp_client.c)）：
**主节点一重启，它记的地址就没了**，而我们从节点收不到任何通知（UDP 无连接）。
没有周期心跳的话，我们会一直以为链路好好的，主节点却再也找不到我们 ——
直到我们自己也被重启一次。

所以 `UDP_ANNOUNCE_MS = 30000`（[udp_client.c](../main/udp_client.c)）：
30 秒是权衡，够短（主节点重启后半分钟内链路自动恢复），够长（不至于在空口上刷屏）。
TCP 没这个问题，因为 TCP 是我们拨出去的，主节点重启 → 连接断 → `recv()` 返回 0 → 重连。

### 1.3 本项目为什么两条都留着

不是"哪个更好"，是**各有各的用处**：

| 场景 | 用哪条 | 为什么 |
| --- | --- | --- |
| 日常控制（开灯 / 关灯 / 改 WiFi） | **TCP** | 命令不能丢，而且要立刻知道对方在不在 |
| 想做广播 / 组播 | UDP | TCP 根本做不了（没有连接就没法广播） |
| 想做极省资源的点对点 | UDP | 8 字节头、无连接状态；而且 ESP-NOW 那条路就是从这儿长出去的 |

运行期靠 `net tcp` / `net udp` 两条命令切（[`cmd.c` 里的 `s_cmds[]`](../main/cmd.c)），
同一时刻**只有一条在收** —— 两条都开的话，同一条命令会从两条路各到一次，
"开灯"被执行两次（[link.h](../main/link.h)）。

#### 两个端口为什么故意取不同的号

TCP 用 **8086**，UDP 用 **8087**。其实 UDP 端口和 TCP 端口是
**两套互不干扰的编号空间** —— 理论上都取 8086 也没事，内核不会搞混。

项目还是把它们分开了，理由是**给人看的**：重号之后，看日志、抓包、配防火墙，
都得先在脑子里换算一次"这是哪个协议的那个 8086"。没必要给自己挖这个坑
（[`udp_client.c` 里的 `UDP_PORT` 宏](../main/udp_client.c)）。

---

## 2. TCP 的 API 怎么用

### 2.1 六步骨架

全是 BSD socket 风格的 API，lwIP 提供的。顺序是死的：

```c
int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);   // ① 要一个 socket
setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, ...);    // ② 给 recv 设超时
connect(sock, (struct sockaddr *)&dest, sizeof(dest));  // ③ 拨号（只有客户端能做）
send(sock, data, len, 0);                               // ④ 发
recv(sock, buf, sizeof(buf), 0);                        // ⑤ 收
close(sock);                                            // ⑥ 挂断
```

对应项目里的位置：[tcp_client.c 里的 `tcp_client_task()`](../main/tcp_client.c)
一个函数里，六步挨着写下来。

### 2.2 逐个说

#### `socket()` —— 参数就是"要哪种协议"

```c
int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
/*                     ↑          ↑            ↑
                   IPv4     流式（有连接）   具体协议 */
```

对比 UDP 那句（[`udp_client.c` 里的 `udp_client_task()`](../main/udp_client.c)）就一眼看出区别了：
`SOCK_DGRAM` + `IPPROTO_UDP`。**这三元组决定了后面能用哪些 API。**

> ⚠️ 但别把"能调"和"按你想的跑"混为一谈，这里有个 lwIP 的实现细节值得知道：
> **在 TCP socket 上调 `sendto()` 不会报错**，它只是把地址参数**丢掉**，
> 转成 `send()`（`lwip/src/api/sockets.c` 里的 `lwip_sendto()`）：
>
> ```c
> if (NETCONNTYPE_GROUP(netconn_type(sock->conn)) == NETCONN_TCP) {
>     return lwip_send(s, data, size, flags);   /* ← to / tolen 直接被忽略 */
> }
> ```
>
> 也就是说"在 TCP 上误用 `sendto()` 并填了个错地址"是**静默生效**的 ——
> 数据发去了 `connect()` 时定的那个对端，而不是你填的那个。
> 同理，UDP socket 上 `connect()` 也不握手，它只是"记住默认对端"，
> 让后续的 `send()` 不用每次填地址。

返回的是一个 **fd（文件描述符）**，`int`。失败返回负数，`errno` 里有原因。

#### `setsockopt(SO_RCVTIMEO)` —— 为什么非有不可

```c
struct timeval tv = { .tv_sec = TCP_RECV_TIMEOUT_S, .tv_usec = 0 };
setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
```

没有它，`recv()` 默认是**永远阻塞**的。后果不是"等得久一点"，而是：
**连接半死不活的时候，这条任务再也醒不过来了**（[`tcp_client.c` 里的 `tcp_client_task()`](../main/tcp_client.c)）。

而且这个超时在项目里是**双重身份**：
[`udp_client.c` 里的 `UDP_RECV_TIMEOUT_S` 宏](../main/udp_client.c) 说得更直白 —— 它既让任务有机会
看到 `s_running` 被置成 false，又顺便充当心跳的定时点。

> 超时**不算错误**。`recv()` 超时会返回 -1 并设 `errno = EWOULDBLOCK`，
> 必须和真错误分开处理（见下面 `recv()` 那段）。

#### `connect()` —— 只有客户端能做

```c
memset(&dest, 0, sizeof(dest));
dest.sin_family      = AF_INET;
dest.sin_port        = htons(TCP_SERVER_PORT);        // ← 必须 htons
dest.sin_addr.s_addr = inet_addr(TCP_SERVER_IP);      // ← 字符串转 32 位整数
connect(sock, (struct sockaddr *)&dest, sizeof(dest));
```

三个细节：

- **`htons()` 不能省**。`sockaddr_in` 里的东西都是**网络字节序**（大端），
  而 ESP8266 是小端。端口号是 16 位，直接赋值就是高低字节颠倒。
  这让"端口明明写对了却连不上"变成一类经典事故。（地址用 `inet_addr()` 转，
  它自己会处理字节序，不用你再转。）
- **`memset` 不能省**。`sockaddr_in` 里还有填充字段，不清零就是栈上的随机值。
- **它失败的原因分两种**，看 `errno`（见 §2.3 的表）：包没出去（113）和
  对方拒绝（111）是**完全不同**的问题，不要混着查。

#### `send()` —— 返回值是"交给协议栈了多少"

```c
return send(s_sock, data, len, 0);
```

⚠️ 返回成功**只说明字节进了协议栈的发送缓冲区**，不代表对方收到了。
TCP 保证的是"最终有序可靠送达，否则报错"，不是"`send()` 返回时对方已经收到"。

#### `recv()` —— 三种返回值，三种意思

这是最需要记清楚的一个。项目里的处理（[`tcp_client.c` 里的 `tcp_client_task()`](../main/tcp_client.c)）：

| 返回值 | 含义 | 项目怎么处理 |
| --- | --- | --- |
| `> 0` | 收到了这么多字节 | 交给业务回调 |
| `== 0` | **对端主动关闭了连接** | `break` 出去重连 |
| `< 0` | 出错，**但要看 errno** | 见下 |

`< 0` 还要再分：

```c
if (errno == EWOULDBLOCK || errno == EAGAIN) {
    continue;      /* 只是这 5 秒没数据 —— 正常情况，不是错误 */
}
printf("[tcp] 接收出错 errno=%d\n", errno);   /* 真错误 */
break;
```

> `EWOULDBLOCK` 和 `EAGAIN` 在 lwIP 里是**同一个值**（`lwip/errno.h`：
> `#define EWOULDBLOCK EAGAIN`），所以这两个判断其实是重复的。
> 留着两个是为了可读性 —— POSIX 允许它们不同，代码将来挪到别的栈上就不用改。

⚠️ **`recv()` 交出来的是裸字节流，不是 C 字符串** —— 它不会替你补 `'\0'`，
也不保证是一条完整消息。所以 `tcp_client.c` 里 `recv()` 的长度参数写的是
`sizeof(rxbuf) - 1`，留一个字节的余量，再由上层自己收尾。

#### `close()` —— 别忘了，也别在错的地方关

```c
s_sock = -1;
close(sock);
```

两条纪律：

- **`s_sock = -1` 要在 `close()` 之前**。`s_sock` 是对外公开的句柄，
  `tcp_client_send()` 靠它判断"能不能发"。先关再置 -1 的话，中间那一瞬间
  别的任务可能拿着一个已经关掉的 fd 去 `send()`。
- **关 socket 的动作只由任务自己做**（[`tcp_client.c` 里的 `s_running`](../main/tcp_client.c)）。
  原因见 §5.4，是一条死锁链。

### 2.3 连接失败时，errno 在说什么

这是查问题时的第一手证据。数字全部来自 `lwip/errno.h`：

| errno | 宏 | 原文注释 | 实际含义 |
| --- | --- | --- | --- |
| 101 | `ENETUNREACH` | Network is unreachable | 本机压根没网（没拿到 IP / 没连上 AP） |
| **113** | `EHOSTUNREACH` | **No route to host** | **有网，但没有去那个地址的路由** |
| 111 | `ECONNREFUSED` | Connection refused | 路由通了，对方回了 RST —— 端口没人监听 |
| 110 | `ETIMEDOUT` | Connection timed out | SYN 发出去了，没有任何回应（对方不在 / 被防火墙丢） |
| 104 | `ECONNRESET` | Connection reset by peer | 连接中途被对方强行掐断 |
| 98 | `EADDRINUSE` | Address already in use | 端口被占（UDP 的 `bind()` 常见） |

这些宏都定义在 `lwip/errno.h` 里，括号里的数字是 lwIP 给它们编的值。

> **113 和 111 的区别是最常被搞混的一对**，而且它们的排查方向相反：
>
> - **113** = 包在**路由那一步**就没出去。写法是**错网段**了 ——
>   节点在 `192.168.137.x`，却去连 `192.168.4.1`，中间没有路由能过去。
>   这是"两套地址只切了一半"的典型症状（[`tcp_client.c` 里的 `TCP_SERVER_IP` 宏](../main/tcp_client.c)）。
> - **111** = 包到了对方机器，对方明确拒绝了。说明**网络是通的**，
>   问题在"那个端口上没人监听" —— 网络调试助手没开监听，或者监听的端口不对。
>
> 看到 113 先去查地址，看到 111 先去查对方。

---

## 3. UDP 的 API 怎么用

### 3.1 和 TCP 的对应关系

| 步骤 | TCP | UDP |
| --- | --- | --- |
| 建 socket | `socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)` | `socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)` |
| 设超时 | `setsockopt(SO_RCVTIMEO)` | **一样** |
| 绑定本机端口 | 不用（`connect` 时自动分配） | **`bind()`，必须** |
| 建立连接 | `connect()` | **没有这一步** |
| 发送 | `send(sock, data, len, 0)` | `sendto(sock, data, len, 0, &dest, sizeof(dest))` |
| 接收 | `recv(sock, buf, size, 0)` | `recvfrom(sock, buf, size, 0, &from, &fromlen)` |
| 关闭 | `close()` | **一样** |

一句话：**UDP 少了 `connect()`，多了 `bind()`，收发各多一个地址参数。**

### 3.2 `bind()` —— 为什么 UDP 这边非有不可

```c
memset(&local, 0, sizeof(local));
local.sin_family      = AF_INET;
local.sin_port        = htons(UDP_PORT);
local.sin_addr.s_addr = INADDR_ANY;

if (bind(sock, (struct sockaddr *)&local, sizeof(local)) != 0) { ... }
```

**不 bind 也能跑** —— lwIP 会在第一次 `sendto()` 时随便挑一个临时端口
（`lwip/src/core/udp.c` 里的 `udp_bind()`：端口传 0 就走 `udp_new_port()` 自动分配）。
但那意味着：**我们每次的源端口都可能变**，而主节点的回包规则是
"回源 IP:端口" —— 端口一变，回包就发错地方了。

所以 `bind()` 在这里的作用是：**把本机端口钉死，让主节点的回包规则没有歧义**
（[`udp_client.c` 里的 `udp_client_task()`](../main/udp_client.c)）。

`INADDR_ANY` 的含义是"收本机所有网卡上的这个端口"（不是"随便挑一个地址"）。

> ⚠️ 本端 `bind` 的端口和目的端口用的是**同一个值**（都是 8087），
> 这是有意设计的：主节点收到包的源地址就是"从节点IP:8087"，回包直接发给它，
> 不需要额外协商，也不需要维护一张"谁在哪"的表。

### 3.3 `sendto()` 和 `recvfrom()`

#### `sendto()` —— 每次都要重新填地址

```c
memset(&dest, 0, sizeof(dest));
dest.sin_family      = AF_INET;
dest.sin_port        = htons(UDP_PORT);
dest.sin_addr.s_addr = inet_addr(UDP_SERVER_IP);

return sendto(s_sock, data, len, 0, (struct sockaddr *)&dest, sizeof(dest));
```

最后两个参数就是"发给谁"，**每次调用都得填** —— 因为 UDP socket 不记这个。

> 项目**故意每次现算**，而不是建 socket 时存一份全局的
> （[`udp_client.c` 里的 `udp_client_send()`](../main/udp_client.c)）：
> 反正就十几个字节的填表，换来的是不必操心"上次存的地址会不会是过期的"。

⚠️ **`sendto()` 返回成功不代表对方收到了。** UDP 没有确认、没有重传、
没有连接状态 —— 对方是死是活，本地完全看不出来（[`udp_client.c` 里的 `udp_client_send()`](../main/udp_client.c)）。

#### `recvfrom()` —— 那个 `from` 参数，项目故意不用

```c
struct sockaddr_in from;
socklen_t          fromlen = sizeof(from);

int len = recvfrom(sock, rxbuf, sizeof(rxbuf), 0,
                   (struct sockaddr *)&from, &fromlen);
```

`recvfrom()` 会把**发送方的地址**填进 `from`。于是很容易想到一个"聪明"的做法：
**从收到的包里学习主节点的地址**，这样就再也不怕主节点换 IP 了。

**项目故意不这么做**（[udp_client.h](../main/udp_client.h)）：

> 那样等于让热点里**任何一台设备**，只要往本机 8087 端口丢一包，
> 就能把自己变成"主节点"。
>
> 写死地址虽然死板，但它把**"谁能指挥这个节点"钉死在编译期**。

这是本项目里唯一一处**为了安全而主动放弃灵活性**的地方，值得记住。
`from` 变量仍然要留着 —— 只是 `recvfrom()` 的参数不能传 NULL。

### 3.4 一个 UDP 特有的坑：`bind()` 失败会一直失败

```c
if (bind(sock, (struct sockaddr *)&local, sizeof(local)) != 0) {
    printf("[udp] bind 端口 %d 失败 errno=%d，2 秒后重试\n", UDP_PORT, errno);
    close(sock);
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    continue;
}
```

注意这里**先 `close(sock)` 再重试**，而且**回到循环开头会重新 `socket()`**。
顺序不能反：端口被占（`EADDRINUSE`）时，如果拿着同一个 socket 反复 `bind()`，
会一直失败 —— 必须换一个新的 socket 再试。

---

## 4. STA 模式的 API 怎么用

STA = **Station**，也就是"客户端模式"：**ESP8266 去连别人的热点**。

对照的另一半是 AP 模式（自己当热点），本项目**从节点**用的是 STA，
**主节点 ESP32** 用的是 AP。ESP8266 两种都支持，还能同时开（`WIFI_MODE_APSTA`）。

### 4.1 初始化顺序

顺序是死的，每一行的理由都不一样（全部在
[`wifi_sta.c` 里的 `wifi_sta_init()`](../main/wifi_sta.c)）：

| # | 调用 | 为什么在这个位置 |
| --- | --- | --- |
| ① | `nvs_flash_init()` | WiFi 驱动要把 PHY 校准数据存进 NVS，**不初始化会直接启动失败** |
| ② | `tcpip_adapter_init()` | 把 lwIP 协议栈拉起来 —— 后面所有 socket API 的地基 |
| ③ | `esp_event_loop_create_default()` | 起默认事件循环，"连上了 / 断开了"靠它派发 |
| ④ | `esp_wifi_init(&cfg)` | 初始化 WiFi 驱动本身 |
| ⑤ | `esp_wifi_set_ps(WIFI_PS_NONE)` | 关省电（**本 SDK 上冗余**，见下） |
| ⑥ | `esp_event_handler_register(...)` ×2 | **先注册回调，再启动** |
| ⑦ | `esp_wifi_set_mode(WIFI_MODE_STA)` | 声明"我是客户端" |
| ⑧ | `esp_wifi_set_config(ESP_IF_WIFI_STA, &cfg)` | 把 SSID / 密码灌进去 |
| ⑨ | `esp_wifi_start()` | 启动。**它是异步的，返回时还没连上** |

**⑥ ⑦ ⑧ ⑨ 的顺序值得单独说**：`esp_wifi_start()` 会触发
`WIFI_EVENT_STA_START` 事件，而**连接动作是在那个事件的处理函数里发起的**
（[`wifi_sta.c` 里的 `wifi_event_handler()`](../main/wifi_sta.c)）：

```c
if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    printf("[wifi] STA 已启动，开始连接 \"%s\" ...\n", s_ssid);
    esp_wifi_connect();      /* ← 真正的连接动作在这里 */
}
```

所以**回调和配置都必须在 `esp_wifi_start()` 之前就位**，否则事件来了没人接，
或者接到的时候配置还是空的。

> **`esp_wifi_start()` 只是把驱动拉起来，它不会去连 AP。**
> 这是初学者最容易误会的一点 —— "我 start 了怎么不连？"
> 连接得你自己在 `STA_START` 事件里 `esp_wifi_connect()`。

#### ⑤ 那句冗余的 `esp_wifi_set_ps()`

[`wifi_sta.c` 里的 `wifi_sta_init()`](../main/wifi_sta.c) 把它标成了"冗余"，
依据是 `esp_wifi.h 里的 esp_wifi_set_ps()` 的原文：

```text
@attention Default power save type is WIFI_PS_NONE.
```

默认本来就不省电，所以这一句**不是任何问题的修复**。留着它的理由写在代码里：
把前提写死，而且**将来会真的有用** —— SDK 自带的 ESP-NOW 示例
（`examples/wifi/espnow/README.md`）明确写了"接收方是 station 且连着 AP 时，
必须关掉 modem sleep"。哪天从节点改用 ESP-NOW，省电开着就会收不到包。

> 这是一个值得学的做法：**明知当前冗余，但为了让前提显式存在而保留**，
> 同时在注释里说清楚"它现在是冗余的、为什么还留着"。
> 不写清楚的话，下一个人会以为是某个 bug 的修复而不敢删。

#### ⑧ 灌进去的 `wifi_config_t`

`esp_wifi_set_config()` 收的是一个结构体，两个地方必须小心
（[`wifi_sta.c` 里的 `apply_credentials()`](../main/wifi_sta.c)）：

```c
wifi_config_t cfg;
memset(&cfg, 0, sizeof(cfg));                       // ← ① 必须清零

copy_str((char *)cfg.sta.ssid,     sizeof(cfg.sta.ssid),     s_ssid);
copy_str((char *)cfg.sta.password, sizeof(cfg.sta.password), s_pass);

cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;    // ← ② 最弱可接受的加密方式
esp_wifi_set_config(ESP_IF_WIFI_STA, &cfg);
```

**① 必须 `memset`。** 这个结构体里还有很多字段（信道、BSSID、各种阈值），
不清零它们就是栈上的随机值 —— 会变成"偶尔连得上偶尔连不上"那种最难查的毛病。

**② `threshold.authmode` 是"下限"，不是"要求"。** 设成 `WIFI_AUTH_WPA2_PSK`
的意思是"**比 WPA2 弱的（WEP、WPA-TKIP）我都不连**"：

| 你的热点是 | 设了 WPA2_PSK 之后 |
| --- | --- |
| WPA2-Personal（Windows 热点默认） | 连得上 |
| 开放（无密码） | **连不上** |
| WPA/WPA2 混合、WPA3 | 可能连不上 —— 把这行去掉再试 |
| 不设这一行 | 等于接受任何加密方式 |

> ⚠️ 这一条对"主节点自建 SoftAP"同样适用：**ESP32 那边 `ap.authmode`
> 也必须选 WPA2-PSK**，否则从节点会把它挡在外面。这就是 ESP8266开发流程.md §9.2
> 那张"四值表"里 **authmode** 占一格的原因 —— 它和 SSID / 密码 / 端口一样，
> 是**两边必须手工对齐**的值之一。

**③ 这里是运行期赋值，不能再用初始化器写法。**
`cfg.sta.ssid` 的类型是 `uint8_t[32]`，而 `s_ssid` 是 `char *`，类型对不上。
C 标准专门允许用字符串字面量**初始化**字符数组
（`.ssid = "ESP32-S3-host"` 那种写法），但**赋值**不行，必须老老实实拷贝。

#### 谁记得住 `ssid` / `password` 的字节数上限？

`wifi_config_t` 里的 `ssid[32]` / `password[64]` 是 **802.11 协议**规定的上限
（`SSID_MAX_LEN` / `PASS_MAX_LEN`，[wifi_sta.c](../main/wifi_sta.c)）。
注意**单位为字节**，不是字符 —— 一个中文 SSID 一个字 3 字节。

> ⚠️ 长度**必须在存进 NVS 之前**检查（[`wifi_sta.c` 里的 `wifi_sta_set_credentials()`](../main/wifi_sta.c)）：
> 超长的 SSID 被截断后照样能写进 NVS，但永远连不上 ——
> 那种"命令说成功了、就是连不上"的毛病最难查。

#### 接口名的新旧写法

代码里写的是 `esp_wifi_set_config(ESP_IF_WIFI_STA, &cfg)`
（[`wifi_sta.c` 里的 `apply_credentials()`](../main/wifi_sta.c)）。
你在别的文档里会看到 `WIFI_IF_STA` —— 两者**是同一个东西**：

```c
/* esp_wifi_types.h */
#define WIFI_IF_STA ESP_IF_WIFI_STA
```

只是个 alias，用哪个都行。

### 4.2 事件驱动：不轮询，等回调

WiFi 连接**不能轮询**（没有"连上了吗"这种 API 值得去查），
而是**注册一个回调，让系统在状态变化时叫你**：

```c
esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);
```

| 参数 | 意思 |
| --- | --- |
| `WIFI_EVENT` / `IP_EVENT` | **事件大类**（event_base）。WiFi 状态变化是一类，网络层事件是另一类 |
| `ESP_EVENT_ANY_ID` | 这个大类下的**所有**事件我都要 |
| `IP_EVENT_STA_GOT_IP` | 只要这一个事件，别的不用叫我 |
| `&wifi_event_handler` | 事件来了调谁 |
| `NULL` | 传给回调的自定义参数（本模块用不上） |

回调签名是固定的，四个参数（[`wifi_sta.c` 里的 `wifi_event_handler()`](../main/wifi_sta.c)）：

```c
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
```

⚠️ **`event_data` 的类型由 `event_id` 决定，得自己转**：

```c
wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)event_data;
/* d->reason 就是 802.11 断开原因码 */
```

#### 这个回调跑在哪条任务上？

**不在你自己的任务里**，在系统的事件循环任务上。这个任务的参数是写死的：

```c
/* default_event_loop.c 里的 esp_event_loop_create_default() */
esp_event_loop_args_t loop_args = {
    .queue_size = CONFIG_ESP_SYSTEM_EVENT_QUEUE_SIZE,   /* CONFIG_ESP_SYSTEM_EVENT_QUEUE_SIZE = 32 */
    .task_name = "sys_evt",
    .task_stack_size = ESP_TASKD_EVENT_STACK,           /* 2048 + 余量（CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE） */
    .task_priority = ESP_TASKD_EVENT_PRIO,
    .task_core_id = 0
};
```

算一下优先级：`ESP_TASK_PRIO_MAX - 5`（`esp_task.h` 里的 `ESP_TASKD_EVENT_PRIO` 宏），而
`ESP_TASK_PRIO_MAX = configMAX_PRIORITIES = 15`（`esp_task.h`、
`FreeRTOSConfig.h`）——**`sys_evt` 的优先级是 10**。

对比一下项目里自己起的任务：

| 任务 | 优先级 | 来源 |
| --- | --- | --- |
| `sys_evt`（跑事件回调） | **10** | SDK 内部 |
| `tcp_client` / `udp_client` | 5 | [`tcp_client.c` 里的 `tcp_client_start()`](../main/tcp_client.c) / [`udp_client.c` 里的 `udp_client_start()`](../main/udp_client.c) |
| `wifi_mgr`（状态上报） | 4 | [`wifi_sta.c` 里的 `wifi_sta_init()`](../main/wifi_sta.c) |

**事件回调跑在优先级 10 的任务上，比传输任务高一倍。** 这就是为什么
[`wifi_sta.c` 里的 `wifi_event_handler()`](../main/wifi_sta.c) 那条警告这么重：

> 绝对不能在这里做阻塞操作（比如 `vTaskDelay`），否则整个 WiFi 状态机都会停摆。

一个优先级 10 的任务睡 1 秒，优先级 5 的传输任务才轮得上 —— 而且**在事件回调里
阻塞，等于把整个事件循环堵住**：所有 WiFi 状态变化、所有 IP 事件都会排在后面没人处理。

所以"重连"这件事在项目里的写法是（[`wifi_sta.c` 里的 `wifi_event_handler()`](../main/wifi_sta.c)）：

```c
/* 直接重连、不在这里 sleep：esp_wifi_connect() 内部要先扫一遍信道、
   再等认证超时，本身就要好几秒，不会把事件循环转成死循环。 */
esp_wifi_connect();
```

**"等一会儿"这个活儿本身是阻塞的，所以它必须挪到别的地方去** ——
这就是 `wifi_mgr_task` 存在的唯一理由（[`wifi_sta.c` 里的 `wifi_mgr_task()`](../main/wifi_sta.c)）。

### 4.3 事件位：回调和任务之间怎么传话

事件回调里不能阻塞，所以它**只能置一个标志就走**。项目用的是 FreeRTOS 的
**事件组**（EventGroup）：

```c
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_GOT_IP_BIT   BIT0
#define WIFI_DOWN_BIT     BIT1
```

| 谁 | 干什么 | 位置 |
| --- | --- | --- |
| 回调（`sys_evt` 任务） | 拿到 IP → `SetBits(GOT_IP)` | [`wifi_sta.c` 里的 `wifi_event_handler()`](../main/wifi_sta.c) |
| 回调（`sys_evt` 任务） | 断开 → `ClearBits(GOT_IP)` + `SetBits(DOWN)` | [`wifi_sta.c` 里的 `wifi_event_handler()`](../main/wifi_sta.c) |
| 传输任务 | `xEventGroupWaitBits(GOT_IP, 无限等)` | [`wifi_sta.c` 里的 `wifi_sta_wait_ip()`](../main/wifi_sta.c) |
| `wifi_mgr` | 等到 `DOWN` 或被超时唤醒 | [`wifi_sta.c` 里的 `wifi_mgr_task()`](../main/wifi_sta.c) |

两个容易写错的地方，代码里都标了：

**① `wifi_sta_wait_ip()` 用的是 `pdFALSE` —— 不清除事件位**
（[wifi_sta.c](../main/wifi_sta.c)）：

```c
EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_GOT_IP_BIT,
                                       pdFALSE,   /* ← 不清除这个 bit */
                                       pdTRUE,
                                       ticks);
```

因为**多个任务可能同时在这里等**。清除的话，先醒的那个会把位擦掉，
后面的永远等不到。

**② `wifi_mgr_task` 里两个 `xEventGroupWaitBits` 的 `pdFALSE` 含义不同**
（[`wifi_sta.c` 里的 `wifi_mgr_task()`](../main/wifi_sta.c)）：

```c
if (xEventGroupWaitBits(s_wifi_event_group, WIFI_GOT_IP_BIT,
                        pdFALSE, pdTRUE,
                        pdMS_TO_TICKS(WIFI_DOWN_REPORT_MS)) & WIFI_GOT_IP_BIT) {
```

这里也必须是 `pdFALSE` —— 它只是在**看一眼**有没有连上，不能把位清掉，
否则 `wifi_sta_wait_ip()` 那边永远等不到。

> 事件组就是"位"的集合，可以理解成一个**能被阻塞等待的全局标志变量**。
> 它比"回调里直接改一个 `bool` + 任务里 `vTaskDelay` 轮询"好的地方在于：
> 等待是**不占 CPU 的**（`portMAX_DELAY` 就是"睡到有人叫我"）。

### 4.4 "关联上" ≠ "连上了"

这是 STA 模式里最重要的一个区分，项目在 [`wifi_sta.c` 里的 `wifi_event_handler()`](../main/wifi_sta.c)
专门标了出来：

| 事件 | 含义 | 此时能通信吗 |
| --- | --- | --- |
| `WIFI_EVENT_STA_CONNECTED` | **关联**上了 AP（802.11 层通了） | **不能** —— 还没有 IP |
| `IP_EVENT_STA_GOT_IP` | DHCP 拿到 IP 了 | **能** |

所以：

- **"真的连上了"的标志是 `GOT_IP`**，不是 `CONNECTED`
- `tcp_client` / `udp_client` 一进去就卡在 `wifi_sta_wait_ip()` 上
  （[`tcp_client.c` 里的 `tcp_client_task()`](../main/tcp_client.c)）：
  **没有 IP 的时候 `connect()` 必然失败，与其盲目重试，不如在这里阻塞等待**
- 项目**根本没注册** `WIFI_EVENT_STA_CONNECTED` —— 它只关心"能通信了没"

### 4.5 断开原因码 `reason`

断开事件带着一个 802.11 的原因码，这是查"连不上"的第一手证据
（[`wifi_sta.c` 里的 `wifi_event_handler()`](../main/wifi_sta.c)）。
SDK 把这些码定义成了枚举，在 `esp_wifi_types.h` 里的 `wifi_err_reason_t`：

| reason | 宏 | 实际含义 | 去查什么 |
| --- | --- | --- | --- |
| 200 | `BEACON_TIMEOUT` | 收不到 AP 的信标了 | 信号太差 / AP 关了 / 距离太远 |
| **201** | `NO_AP_FOUND` | 扫不到这个 SSID | 名字写错（**空格和大小写都算**）、或热点在 5GHz 上 |
| 202 | `AUTH_FAIL` | 认证失败 | 密码不对 |
| 203 | `ASSOC_FAIL` | 关联失败 | AP 侧拒绝（MAC 过滤、超出上限） |
| **15** | `4WAY_HANDSHAKE_TIMEOUT` | 四次握手超时 | 密码不对 |
| 204 | `HANDSHAKE_TIMEOUT` | 握手超时 | 密码不对 / 信号差 |
| 205 | `CONNECTION_FAIL` | 连接失败 | 综合性的失败，看串口上下文 |
| 5 | `ASSOC_TOOMANY` | AP 上连的设备太多了 | 踢掉几个，或者换个 AP |
| 2 / 3 | `AUTH_EXPIRE` / `AUTH_LEAVE` | 之前的认证失效 / 对端要求退出 | 多为正常掉线，项目会自动重连 |

> 最常见的两个是 **201** 和 **15/202/204**：
> 前者是"根本没找到这个网"，后者是"找到了但密码过不去"。
>
> **ESP8266 只支持 2.4GHz。** 热点开在 5GHz 上芯片根本扫不到，
> 现象和"SSID 写错"**一模一样**，都是 reason=201
> （[`wifi_sta.c` 里的 `WIFI_SSID_DEFAULT` 宏](../main/wifi_sta.c)）。

### 4.6 换热点的两个 API

```c
esp_wifi_set_config(ESP_IF_WIFI_STA, &cfg);   /* 改配置 */
esp_wifi_disconnect();                        /* 断开 → 触发 DISCONNECTED 事件 */
```

顺序**不能反**（[`wifi_sta.c` 里的 `wifi_sta_set_credentials()`](../main/wifi_sta.c)）：
先改配置，再断开重连。反过来的话，断开事件里那个 `esp_wifi_connect()`
会用**旧配置**去连，然后你会看到"命令说成功了，就是连不上"。

还有一处讲究（[`wifi_sta.c` 里的 `wifi_sta_set_credentials()`](../main/wifi_sta.c)）：

```c
if (esp_wifi_disconnect() != ESP_OK) {
    esp_wifi_connect();
}
```

正常情况下不用手动连 —— `disconnect()` 触发 `DISCONNECTED` 事件，
回调里会自己 `esp_wifi_connect()`。**但如果当时本来就没连上**，
`disconnect()` 返回错误、也不会有事件，那就得手动补一脚。

---

## 5. 模块的架构

### 5.1 分层

```text
        ┌──────────────────────────────────────────┐
        │  main.c                                  │
        │  粘合层 / 启动器                          │
        │  只把下面这些按顺序接起来，不含业务逻辑     │
        │  它【不知道】"开灯"是什么                  │
        └───────────────┬──────────────────────────┘
                        │ 注册回调 cmd_on_tcp_rx() / cmd_on_udp_rx()
                        ▼
        ┌──────────────────────────────────────────┐
        │  cmd.c                                   │
        │  业务层：攒字节 → 认关键字 → 派发动作       │
        │  它知道"开灯"是什么                        │
        └────┬───────────────────────────────┬─────┘
             │ relay_on() / relay_off()      │ link_send() / link_switch()
             ▼                               ▼
      ┌──────────────┐              ┌──────────────┐
      │ relay.c      │              │ link.c       │  ← 当前走 TCP 还是 UDP
      │ GPIO0 拉高/低 │              │ 所有回执的出口 │     只有这一处知道
      └──────────────┘              └──────┬───────┘
                                           │ 选路
                             ┌─────────────┴──────────────┐
                             ▼                            ▼
                      ┌──────────────┐            ┌──────────────┐
                      │ tcp_client.c │            │ udp_client.c │   ← 传输层
                      │ 纯传输：      │            │ 纯传输：      │     不知道"开灯"
                      │ 搬字节        │            │ 搬字节        │     是什么
                      └──────┬───────┘            └──────┬───────┘
                             └─────────────┬─────────────┘
                                           ▼
                                 ┌───────────────────┐
                                 │ lwIP socket API   │   socket/connect/send/recv/sendto/recvfrom
                                 │ （BSD 风格）       │
                                 └─────────┬─────────┘
                                           ▼
                                 ┌───────────────────┐
                                 │ WiFi 驱动 + 硬件   │
                                 └─────────┬─────────┘
                                           ▼
                                        空口（2.4GHz）
```

`wifi_sta.c` 在**旁边**，不在这个栈里 —— 它管的是"**怎么连上网**"，
不参与"网上跑什么"。传输层在它之上，而且**依赖它**：

```text
tcp_client.c / udp_client.c  ──调用──▶  wifi_sta_wait_ip()   （等网通）
                             ──调用──▶  wifi_sta.h 的其它接口
```

**反向的依赖一条都没有**：`wifi_sta.c` 不知道有 TCP 和 UDP 这两个模块存在。
这就是为什么 `cmd.c` 里那条 `wifi` 命令能工作 —— 它调的是
`wifi_sta_set_credentials()`，跟当前走的是哪条链路完全无关。

### 5.2 往下调用，往上回调

这是这套架构的核心，一句话概括：

> **上层往下调函数（API），下层往上调回调（callback）。**

| 方向 | 机制 | 例子 |
| --- | --- | --- |
| 往下 | 直接函数调用 | `link.c` → `tcp_client_send()` |
| 往上 | 注册回调函数指针 | `tcp_client` → `cmd_on_tcp_rx()` |

**为什么往上不能也直接用函数调用？** 因为传输层**不知道业务层是谁**，
它的任务在 `app_main()` 返回之后还在跑。如果不注册回调，
传输层要么得 `#include "cmd.h"` 反向依赖（那就成了面条），
要么得知道"开灯"是什么（那就不是纯传输层了）。

注册这个动作在 `app_main()` 里（[main.c](../main/main.c)）：

```c
/* ③ 注册回调 —— 必须在 link_init() 之前：后者会立刻把链路启动起来，
      注册晚了第一段到达的数据会因为回调还是 NULL 而被悄悄丢掉。

      两个都注册：当前只有一条在收，但切换之后另一条需要自己的入口。 */
tcp_client_set_rx_handler(cmd_on_tcp_rx);
udp_client_set_rx_handler(cmd_on_udp_rx);
```

⚠️ **顺序是有约束的**：`set_rx_handler()` 必须在 `start()` **之前**。
反过来的话，任务已经开始收了，而回调还是 `NULL` —— 那段时间的数据
会被静默丢弃（看看 [`tcp_client.c` 里的 `tcp_client_task()`](../main/tcp_client.c)：
`if (s_rx_handler != NULL)`，是 NULL 就什么都不做，**连日志都不打**）。

> 这是一个**接口契约**，不是能靠代码强制的约束。两个模块的头文件里都写了
> "必须在 start() 之前调用"（[`tcp_client.h` 里的 `tcp_client_set_rx_handler()`](../main/tcp_client.h)），
> 但编译器不会帮你检查 —— 遵守它靠的是读注释。

### 5.3 四条任务

`app_main()` 本身不是任务的主体，它只是**把线接起来**
（[main.c](../main/main.c)），接完就进一个空的死循环
（`while(1) vTaskDelay(1000)`）保持存活。

真正干活的是这四条：

| 任务 | 优先级 | 谁起的 | 干什么 |
| --- | --- | --- | --- |
| `sys_evt` | 10 | SDK 的 `esp_event_loop_create_default()` | 跑 WiFi / IP 事件回调 |
| `tcp_client` | 5 | [`tcp_client.c` 里的 `tcp_client_start()`](../main/tcp_client.c) | 连接、收发、断了重连 |
| `udp_client` | 5 | [`udp_client.c` 里的 `udp_client_start()`](../main/udp_client.c) | bind、收发、心跳 |
| `wifi_mgr` | 4 | [`wifi_sta.c` 里的 `wifi_sta_init()`](../main/wifi_sta.c) | 没连上时每 30 秒吭一声 |

几个设计上的共同点：

**① 传输任务永不退出**（[`tcp_client.c` 里的 `tcp_client_task()`](../main/tcp_client.c)）。
`s_running` 为 false 时它只是**空转**（500ms 一轮），不是退出。
这样切模式就不用反复创建/删除任务，也不会出现两条任务抢同一个 socket。

**② `start()` 是幂等的**（[`tcp_client.c` 里的 `tcp_client_start()`](../main/tcp_client.c)）：

```c
if (!s_task_started) {
    s_task_started = true;
    xTaskCreate(tcp_client_task, "tcp_client", 4096, NULL, 5, NULL);
}
s_running = true;
```

任务**只创建一次**，之后每次 `start()` 只是把 `s_running` 置回 true。
所以 `net tcp` / `net udp` 来回切多少次，都不会多出任务来。

**③ UDP 的任务是"懒创建"的**：`app_main()` 里只调 `link_init(LINK_TCP)`
（[main.c](../main/main.c)），而 `link_init()` 按传进来的参数
**只启动一条** —— 传 `LINK_TCP` 就只起 TCP。UDP 那条任务要等到第一次
`net udp` 才会被创建，在那之前一条任务都不多占。

### 5.4 `stop()` 为什么只置一个标志 —— 一条死锁链

这是整个架构里最"绕"的一处，但理由非常硬。

看调用链：

```text
link.c 的 link_switch()
  └─ tcp_client_stop()
       └─ 【本函数正跑在 tcp_client_task 自己的栈上】 ← 关键
```

为什么会这样？因为 `link_switch()` 是被**接收回调**调用的：

```text
tcp_client_task → recv() → s_rx_handler()  → cmd_on_tcp_rx()
                                            → cmd_on_rx_from()
                                            → cmd_on_rx()
                                            → cmd_try_one()
                                            → link_switch()      ← switch 命中 ACT_NET_*
                                            → tcp_client_stop()
```

**这条链从头到尾都在 `tcp_client_task` 的栈上。**

所以如果 `stop()` 写成"发个信号然后**等任务退出**"：

> 等任务退出 = 等自己退出 = **死锁**。

于是 `stop()` 只能这样写（[`tcp_client.c` 里的 `tcp_client_stop()`](../main/tcp_client.c)）：

```c
void tcp_client_stop(void)
{
    s_running = false;    /* 只置标志，立刻返回 */
}
```

关闭 socket 的活儿由任务自己在下一轮循环里收尾。

**代价**：真正断开要等 `recv()` 那一轮超时（最多 5 秒），或者重连等待那一轮
（最多 2 秒）。这段窗口里**旧链路还在收** —— 这就是为什么 §5.5 需要那几道闸。

### 5.5 三道闸：链路和命令的护栏

这三道闸原来都在 `main.c` 里，现在跟着代码分居 `link.c` 和 `cmd.c` ——
**位置变了，职责没变**。

#### 第一道：`link_send()` —— 所有回执只走一个出口

```c
int link_send(const char *data, int len)
{
    return (s_link == LINK_UDP) ? udp_client_send(data, len)
                                : tcp_client_send(data, len);
}
```

[`link.h` 里的 `link_send()`](../main/link.h) 的注释说得很清楚：
**所有回执一律调它，不再直接调 `tcp_client_send()`。**
漏掉哪一处，那条回执在 UDP 模式下就会往 TCP 发 —— 命令从 UDP 进来了，
回复却跑去了另一条路，发送方会觉得"命令生效了但没回音"。

#### 第二道：`cmd_on_rx_from()` —— 挡掉旧链路的包

```c
static void cmd_on_rx_from(link_mode_t src, const char *data, int len)
{
    if (src != link_current()) {
        printf("[cmd] 忽略 %d 字节：来自已停用的链路\n", len);
        return;
    }
    cmd_on_rx(data, len);
}
```

[`cmd.c` 里的 `cmd_on_rx_from()`](../main/cmd.c)：因为 `stop()` 只是置标志，
旧链路最多还能收 5 秒。**不管它的话**，切走之后发过来的命令照样会被执行，
而回执走的是新链路 —— 发送方会觉得"我明明切走了，怎么还被遥控"。

加上这一层，切换就是**干脆**的：`s_link` 一改，旧链路的包立刻失效。

#### 顺带：`link_switch()` 里"先回执后切换"的顺序

```c
/* ① 先回执。此刻 s_link 还是旧值，所以走的是旧链路 —— 正是我们要的。 */
link_send(mode == LINK_UDP ? "NET -> UDP\r\n" : "NET -> TCP\r\n");

/* ② 再换向，然后停一条、起另一条。 */
s_link = mode;
```

[`link.h` 里的 `link_switch()`](../main/link.h)：**反过来的话，这条回执会走新链路，
而发命令的人正在旧链路上等着看结果 —— 永远等不到。**
现象就是"发了 `net udp` 之后就再没动静了"，很容易误判成板子死了。

#### 第三道：换链路时把命令缓冲区整个清空

这一道挡的不是"旧链路的包"，而是**跨链路拼出来的命令**
（[`cmd.c` 里的 `cmd_try_one()`](../main/cmd.c)）：

```text
旧链路上来了个 "开"   →  缓冲区里躺着 "开"，还没拼成完整命令
        ↓  net udp 切换
新链路上来了个 "灯"   →  缓冲区里凑成了 "开灯"

→ 执行了一次开灯，而【没有任何一方发过这两个字】
```

所以 `cmd_try_one()` 里只要这一轮换了链路，就把 `s_cmd_len` 归零。
**换链路 = 把解析器也复位**，和 `cmd_on_rx_from()` 是同一个道理。

> ⚠️ 清空必须放在**抠掉命令之后**。放前面的话 `s_cmd_len` 先归零，
> 上面那句长度相减会变成负数，`memmove` 的长度会大得离谱，直接崩。

#### 附：带参数的 `"wifi "` 命令为什么必须等到整行

`cmd_do_wifi()` 的返回值和别的动作**不一样**（[cmd.c](../main/cmd.c)）：

| 返回 | 含义 |
| --- | --- |
| `true` | 这一行处理完了（不管成功失败），已经从缓冲区抠掉 |
| `false` | **行还没收全**，什么都没做，等下一批字节 |

因为 TCP 是字节流，一次 `recv()` 可能只到 `"wifi DESKTOP"` 就断了，
后面的密码还没来。**硬猜的话会把半截 SSID 当成真的存进 NVS。**
所以规矩是"以换行结尾"—— 用网络调试助手发的时候记得勾"发送新行"。

> 这里还有个容易忽略的规矩：如果 `"wifi "` 靠前但**整行还没到齐**，
> 必须原样返回 `false` 继续等，**不能退回去执行表里那条**
> —— 那等于把后面的命令提前执行了（[`cmd.c` 里的 `cmd_try_one()`](../main/cmd.c)）。

---

## 6. 附：这两个模块里反复出现的两个 C 坑

### 6.1 `strncpy()` 不保证结尾有 `'\0'`

`wifi_sta.c` 里自己写了一个 `copy_str()` 而不用 `strncpy()`
（[wifi_sta.c](../main/wifi_sta.c)）：

```c
static void copy_str(char *dst, size_t dst_size, const char *src)
{
    size_t n = strlen(src);
    if (n > dst_size - 1) {
        n = dst_size - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';          /* ← 这一步 strncpy 在截断时【不做】 */
}
```

**为什么不用 `strncpy()`？** 因为它在**源串长度 >= 目标缓冲区**时
**不会补 `'\0'`**，结果是一个没有结尾的字符串 —— 后面
`strlen()` / `printf("%s")` 会一路读下去越界。

这是 C 里最经典的坑之一。`strncpy` 的名字听起来"安全版 strcpy"，
实际上它是个"**定长填充**"函数，语义完全不是大家以为的那样。
干脆自己写一个语义明确的，比每次都在调用点想想"这次会不会截断"要可靠。

### 6.2 打印字节时 `char` 必须转 `unsigned char`

`cmd.c` 的 `dump_hex()` 里（[cmd.c](../main/cmd.c)）：

```c
printf(" %02X", (unsigned char)data[i]);
```

**不转的话**，`char` 在 Xtensa 上默认是**有符号**的 —— `0xBF` 会被符号扩展成
`0xFFFFFFBF`，`%02X` 打印出来就是 **8 位**。而"编码不对"这类毛病
恰恰要靠这些字节去分辨（GBK 的 `"开灯"` 是 `BF AA B5 C6`，头一个字节就是 `BF`），
打成 `FFFFFFBF` 就完全没法对照了。

> 这两个坑的共同点：**都跟"字节 vs 字符"有关**，而且都**不会报错** ——
> 一个悄悄越界，一个悄悄打错。看代码时不容易发现，跑起来才见鬼。

---

## 7. 一页速查

```text
TCP vs UDP：
  TCP 有连接、字节流、可靠；UDP 无连接、有消息边界、不可靠
  只有 TCP 分客户端/服务端，也【只有客户端能 connect】
  UDP 的对端只能靠"我们先发一包"才知道我们在哪 → 所以必须有心跳

TCP 六步：
  socket(AF_INET, SOCK_STREAM, IPPROTO_TCP) → setsockopt(SO_RCVTIMEO)
  → connect() → send() / recv() → close()
  recv() 的三种返回：>0 收到 / ==0 对端关了 / <0 看 errno
  errno: 101 本机没网 · 113 没路由(地址错网段) · 111 对方拒绝(端口没人听)
         110 超时(对方不在) · 104 被掐断

UDP 五步：
  socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP) → setsockopt(SO_RCVTIMEO)
  → bind() → sendto() / recvfrom() → close()
  比 TCP 少了 connect，多了 bind；bind 是为了让主节点的回包规则没有歧义
  ⚠ recvfrom 给出的源地址【故意不用】—— 用了就等于谁能发包谁就是主节点

STA 初始化顺序：
  nvs_flash_init → tcpip_adapter_init → esp_event_loop_create_default
  → esp_wifi_init → set_ps → 注册回调 → set_mode(WIFI_MODE_STA)
  → set_config(ESP_IF_WIFI_STA, &cfg) → esp_wifi_start()
  esp_wifi_start() 不连接！连接在 WIFI_EVENT_STA_START 回调里 esp_wifi_connect()
  "连上了" = IP_EVENT_STA_GOT_IP，不是 WIFI_EVENT_STA_CONNECTED
  reason 201 = 找不到 SSID（含 5GHz 热点）· 15/205 = 密码不对

架构：
  main.c（粘合）→ cmd.c（业务）→ link.c（选路）→ tcp_client / udp_client（纯传输）
  → lwIP → WiFi 驱动
  wifi_sta.c 在旁边，只管"怎么连上网"，被传输层依赖，反向不依赖
  上层往下调函数，下层往上调回调（set_rx_handler 必须在 start 之前）
  stop() 只能置标志 —— 它跑在要停的那条任务自己的栈上，等退出就是等自己

任务优先级：sys_evt 10 · tcp_client / udp_client 5 · wifi_mgr 4
  ⚠ 事件回调跑在 sys_evt(10) 上，绝对不能阻塞
```

---

## 相关文档

- [ESP8266-NVS.md](ESP8266-NVS.md) —— 凭据存在哪、`nvs_*` 的用法与坑
- [ESP8266分区表.md](ESP8266分区表.md) —— flash 布局
- [ESP8266配网踩坑(SmartConfig).md](ESP8266配网踩坑(SmartConfig).md) —— 那条被拆掉的配网路
- `ESP8266开发流程.md` §9.1（模块分工 —— 本文 §5 的简短版）
- `ESP8266开发流程.md` §9.2（拓扑与四值表）、§9.4/§9.5（命令解析的两个坑）、
  §9.6（换热点的操作纪律）、§9.7（连不上怎么查）、§10（连不上 30 次后的 AP 配网兜底）
- `main/tcp_client.h`、`main/udp_client.h`、`main/wifi_sta.h`、
  `main/relay.h`、`main/link.h`、`main/cmd.h` —— 各模块的接口契约
