# ESP8266 AP 配网的架构

> 范围：ESP-01S（**1MB** flash）· ESP8266_RTOS_SDK v3.4（`v3.4-115-g858c7c2e`）
>
> 这份笔记只讲**代码怎么组织**：谁负责什么、边界切在哪、数据怎么流、为什么这么切。
> 操作步骤、触发条件、和 SmartConfig 的对比在 [ESP8266开发流程.md](ESP8266开发流程.md) §10；
> 踩过的两个坑（431 请求头、favicon 404）在 §10.8。
>
> 涉及的文件：[ap_prov.c](../main/ap_prov.c) / [ap_prov.h](../main/ap_prov.h)（配网本体）、
> [wifi_sta.c](../main/wifi_sta.c)（只涉及它"放弃重连"那一段）、[main.c](../main/main.c)（只涉及那行接线）。
> 凭据收上来之后存哪：[ESP8266-NVS.md](ESP8266-NVS.md)。

## 1. 它在整条链路里的位置

**它不是"配网功能"，是"重连失败之后的兜底"。** 开机时没有人调它，也没有哪条命令能喊它起来 ——
只有 `wifi_sta.c` 数到「连了 30 次都没成功」，它才会被叫出来。

```text
上电
 └─ wifi_sta_init()                    从 NVS 读凭据 → esp_wifi_start()（STA 模式）
     │
     └─ 连上了？ ── 是 ──► 正常干活（TCP 连主节点、收命令）
          │
          否：断开事件 → 立刻重连，如此反复
          │   每次断开 s_retry++                              wifi_sta.c 里的 wifi_event_handler()
          │   管理任务每 30 秒醒来查一次够不够                 wifi_sta.c 里的 wifi_mgr_task()
          ▼
     s_retry >= 30 且注册过回调
          │   串口打印「已经失败 30 次了，转配网模式」          wifi_sta.c 里的 wifi_mgr_task()
          ▼
     ap_prov_run(ssid, 33, pass, 65)   ← 兜底从这里开始       wifi_sta.c 里的 wifi_mgr_task()
          │   （阻塞：开热点、起网页，等人掏手机 —— 可能几分钟，也可能永远）
          ▼
     拿到了 → sta_restart_from_prov() → 存 NVS → 重连         wifi_sta.c 里的 wifi_mgr_task()
     没拿到 → s_retry = 0，回 STA 模式再试 30 次              wifi_sta.c 里的 wifi_mgr_task()
```

30 这个数是 `WIFI_RETRY_BEFORE_AP`（[wifi_sta.h](../main/wifi_sta.h)）。
**计数由"断开"事件驱动，检查由管理任务每 30 秒做一次**（`WIFI_DOWN_REPORT_MS`，[wifi_sta.c](../main/wifi_sta.c)）。
判断用的是 `>=`，所以第 30 次失败不会被漏掉 —— 代价是最多再晚 30 秒才进配网。
改这两个值的后果不一样：改 `WIFI_RETRY_BEFORE_AP` 是改"失败几次"，改 `WIFI_DOWN_REPORT_MS` 是改"多久检查一次"。

> **为什么这一整段要放在管理任务里等？**
> 因为配网的本质是"等一个人走过来掏手机"，是长时间阻塞。而**事件回调跑在系统的事件循环任务上**
> —— 在那里阻塞，整个 WiFi 状态机就停摆了。管理任务本来就是干"等"这件事的
> （[`wifi_sta.c` 里的 `wifi_mgr_task()`](../main/wifi_sta.c)）。
>
> 顺带解释了它为什么是 6144 字节栈（[`wifi_sta.c` 里的 `wifi_sta_init()`](../main/wifi_sta.c)）：
> 这个数是从 4096 加上去的，加它的那次提交就叫「适配新增的配网功能」（`5650c5e6`）——
> 配网那一整段（开热点、起网页、解析请求）都跑在这条任务的栈上。

## 2. 分工：三个文件，依赖是单向的

| 文件 | 它知道什么 | 它**不知道**什么 |
| --- | --- | --- |
| `ap_prov.c` | 热点怎么开、网页长什么样、表单字段叫什么、怎么还原转义 | 凭据要交给谁、要不要存、主节点是谁 |
| `wifi_sta.c` | 凭据在 NVS 里、怎么连、失败了几次 | 有"网页"这回事，也不知道界面上长什么样 |
| `main.c` | 两边都存在，把回调接起来 | 配网内部怎么实现、WiFi 内部怎么重连 |

```text
                    main.c
                   /      \
                  ▼        ▼
            wifi_sta.c   ap_prov.c
                  \        /
                   ▼      ▼
                 wifi_sta.h   ← ap_prov.c 只用了这里的一个只读函数
```

`ap_prov.c` 只调 `wifi_sta_get_ssid()` 读一个名字（用来在页面上显示"现在连不上的是哪个"），
方向是 ap_prov.c → wifi_sta.h，**单向**。

**反过来不行**：wifi_sta.c 要是 include 了 ap_prov.h，两个文件就绕成一个环了。所以 wifi_sta.c
用的是**回调** —— 它只声明"我数够了会喊一个人，签名叫 `wifi_sta_giveup_cb_t`"
（[`wifi_sta_giveup_cb_t`](../main/wifi_sta.h)），至于喊来的是 `ap_prov_run` 还是别的什么，
由 main.c 一行接线决定：

```c
/* main.c 里的 app_main() */
wifi_sta_set_giveup_handler(ap_prov_run);
```

> **没注册回调会怎样？** 管理任务照常往下走，于是就成了"没有这个功能"，
> 行为和加它之前一模一样（[`wifi_sta.c` 里的 `wifi_mgr_task()`](../main/wifi_sta.c)）。
> 这是刻意的：**兜底功能不该变成必经之路** —— 它坏了，正常联网那条路不受影响。

## 3. 一次配网的时序：两条任务 + 一个事件组

这是整个架构里最值得看的一张图。注意这里**有两条任务同时在跑**：

```text
wifi_mgr 任务（栈 6144，prio 4）           httpd 任务（栈 4096，prio IDLE+5）
───────────────────────────────           ──────────────────────────────────
esp_wifi_stop()                                  （还不存在）
esp_wifi_set_mode(AP)
esp_wifi_set_config(AP)
esp_wifi_start()
  └─ WIFI_EVENT_AP_START
       └─ tcpip_adapter：拉网卡 + 起 DHCP  ←  白送的，代码里一行都没有
httpd_start()
  └───────────────────────────────────────► 创建任务 "httpd"，listen 80 端口
注册两条路由 / 和 /save
打印配网横幅（含剩余内存）
xEventGroupWaitBits(..., portMAX_DELAY)      accept → "GET /" → root_get_handler
   ▲                                               │  三段 chunk + 长度 0 收尾
   │                                               ▼
   │                                         手机画出表单
   │                                               │
   │                                         "POST /save"
   │                                               │  切字段 → 还原转义 → 校验
   │                                               │  ① 先发"保存成功"页
   │                                               │  ② 再 xEventGroupSetBits
   └──────────── PROV_DONE_BIT 被点亮 ─────────────┘
      （是②点的，不是①点的）
vTaskDelay(1500ms)                               （服务器自己还在跑）
httpd_stop()      → 拆掉服务器
esp_wifi_stop()   → 关热点
esp_wifi_set_mode(STA)
拷出 ssid / pass，返回 true
```

**三个不能动的顺序**：

| 顺序 | 反了会怎样 |
| --- | --- |
| 先发回执，**再**点事件位（[`ap_prov.c` 里的 `save_post_handler()`](../main/ap_prov.c)） | 先点牌子的话，`ap_prov_run()` 那边可能立刻开始拆服务器，而这个 socket 一个字节还没写出去 —— 用户看到的是"点了保存没反应" |
| 点完牌子再等 **1.5 秒**才拆（[`ap_prov.c` 里的 `ap_prov_run()`](../main/ap_prov.c)） | ① 回执发出去 ≠ 手机画出来了，停热点会当场掐断这条连接；② 处理函数还要再返回一步，服务端在它返回之前就被拆掉，收尾动作会踩在一个正在退出的会话上 |
| 先 `httpd_stop()` **再** `esp_wifi_stop()`（[`ap_prov.c` 里的 `ap_prov_run()`](../main/ap_prov.c)） | 反过来，网页还活着的那一小会儿它的网卡已经没了，那条 socket 会以什么方式结束就不好说了 |

> **返回时 WiFi 是什么状态？** 停着的、STA 模式 —— 热点已经关了，STA 也还没启动。
> 接着连的动作（存 NVS + `esp_wifi_start()`）是调用方的事
> （[`ap_prov.h` 里的 `ap_prov_run()`](../main/ap_prov.h)）。这样切的好处是：
> **ap_prov.c 从头到尾不必知道"凭据最后要拿去干嘛"**，它只管把两个字符串收上来交出去。

## 4. 手机侧：它是怎么走到 192.168.4.1 的

有三件事是**白送**的，代码里一行都没有：

| 白送的东西 | 谁给的 |
| --- | --- |
| `192.168.4.1` 这个地址 | `tcpip_adapter_init()` 里 `IP4_ADDR()` 写死的**编译期常量**（`tcpip_adapter_lwip.c`），不是 DHCP 分的 |
| DHCP 服务器 | AP 一启动，`handle_ap_start()` 就调 `tcpip_adapter_start(IF_AP, ...)`，里面 `dhcps_start()`（`tcpip_adapter_lwip.c` 里的 `tcpip_adapter_start()`） |
| 手机上的"已连接" | WPA2-PSK + 8 字节密码（`AP_PASSWORD`），短了热点根本起不来 |

手机分到的地址来自池子 —— 串口上那行
`softAP assign IP to station,IP is: 192.168.4.2` 就是它。

**但 DNS 是坏的，而且这不是 bug。** DHCP 的 OFFER 里确实带了 DNS 服务器选项，
默认指向板子自己（`dhcpserver.c` 里的 `add_offer_options()` 把 `ipadd` 设成 AP 自己的地址；没配 DNS 时同一个 `ipadd` 又被当成默认 DNS 发出去
无条件写选项 6：没配置过 `dhcps_dns` 就把 `ipadd` 写进去）——**可 ESP 上没有任何东西在听 UDP 53。**

| 后果 | 手机上的表现 |
| --- | --- |
| 域名一律解析不了 | **只能手输 IP**。而 `192.168.4.1` 是 IP，不需要 DNS，正好能用 |
| 手机的联网检测失败（iOS 打 `captive.apple.com`，Android 打 `connectivitycheck.gstatic.com`） | 系统提示"已连接，但无法访问互联网" |
| 有的手机会弹"登录到网络" | 点开是浏览器，照样打不开 —— 那个探针网址也是域名 |

> **这里有个没捡的便宜。** 加一个几十行的 UDP 53 小服务，把**任何**域名都回成
> `192.168.4.1`，手机上那个"登录到网络"的弹窗就会**自己打开**我们的配网页 ——
> 商业智能设备那套"强制门户"（captive portal）就是这么做的，用户一个字都不用输。
> 现在没做，所以必须自己敲地址。要加的话动哪儿见 §9。

## 5. 板子侧：一个真的 web 服务器

**没有 HTML 文件，没有文件系统。** 页面是 C 字符串常量（`PAGE_HEAD` / `PAGE_TAIL`），
由一个跑在板子上的 TCP 服务器临时拼出来发过去。所以准确的说法不是"托管了一个网页"，
而是"实现了一个 web 服务器"。

`httpd_start()` 起一条 FreeRTOS 任务（`httpd_main.c`，任务名就叫 `"httpd"`），
配置来自 `HTTPD_DEFAULT_CONFIG()`（[`ap_prov.c` 里的 `start_ap_and_web()`](../main/ap_prov.c)）：默认栈 4096、
优先级 `tskIDLE_PRIORITY+5`、**80 端口**（`esp_http_server.h` 里的 `HTTPD_DEFAULT_CONFIG` 宏）。
它 accept 之后解析请求，拿「方法 + 路径」去查一张**注册过的路由表**：

| 方法 | 路径 | 处理函数 | 干什么 |
| --- | --- | --- | --- |
| GET | `/` | [`root_get_handler`](../main/ap_prov.c) | 发表单页 |
| POST | `/save` | [`save_post_handler`](../main/ap_prov.c) | 收凭据 |

表里没有的一律 404 —— 浏览器自动来要的 `/favicon.ico` 就是这么变成两条警告的
（页面里内嵌一张 1×1 的 GIF 就是为了让它别来）。

ap_prov.c 只改了三处默认值（[`ap_prov.c` 里的 `start_ap_and_web()`](../main/ap_prov.c)）：

| 字段 | 默认 | 这里 | 为什么 |
| --- | --- | --- | --- |
| `max_open_sockets` | 7 | 3 | 一个人拿手机填，3 条够（浏览器开一个页面会同时占好几条）；省内存 |
| `max_uri_handlers` | 8 | 2 | 就两个路由，8 份槽位白占内存 |
| `lru_purge_enable` | **false** | **true** | **必须开**：手机开页面会同时占好几条连接且经常不主动关，槽位满了新连接被直接拒掉 —— 表现是"热点明明连上了，页面就是打不开"，刷新多少次都没用 |

> **请求头的上限不在这张表里。** 它是编译期的 `CONFIG_HTTPD_MAX_REQ_HDR_LEN`
> （`esp_http_server.h` → Kconfig，默认 512），我们调成了 2048。
> `httpd_config_t` 里**根本没有**这个字段，所以运行时代码改不了它 —— 这是踩过的坑，
> 见 [ESP8266开发流程.md](ESP8266开发流程.md) §10.8。

页面是怎么发出去的（[`ap_prov.c` 里的 `root_get_handler()`](../main/ap_prov.c)）：

```c
httpd_resp_set_type(req, "text/html; charset=utf-8");  /* 少了它中文乱码 */
httpd_resp_send_chunk(req, PAGE_HEAD, -1);   /* 上半截，断在 <b> 前面 */
httpd_resp_send_chunk(req, now,       -1);   /* 中间插"现在连不上的是哪个"，转义过 */
httpd_resp_send_chunk(req, PAGE_TAIL, -1);   /* 表单 + 提示 + </html> */
httpd_resp_send_chunk(req, NULL, 0);         /* 长度 0 = 发完了 */
```

> **为什么分三段？** 因为中间要插 `wifi_sta_get_ssid()`，长度不定、还得转义。
> 分三段就不用把整页拼进一个缓冲区，也就不会拼不下被截断。
>
> **最后那个长度 0 的调用是必须的**：chunked 编码没有它，浏览器会一直等
> "后面还有没有"，页面永远转圈 —— 而固件这边看上去一切正常。这是最难查的一类故障。

表单本体（[`ap_prov.c` 里的 `PAGE_TAIL[]`](../main/ap_prov.c)）里，两个 `name` 是后端取值的钥匙：
`name="ssid"` / `name="pass"`，提交方式是 `method="POST" action="/save"`。
**改了 `name=` 而不改 `httpd_query_key_value()` 那两个实参，就对不上了** —— 两边是一对，改一处必改另一处。

## 6. 凭据是怎么被解出来的

浏览器提交的正文是 `application/x-www-form-urlencoded`，形如 `ssid=xxx&pass=yyy`，
两个值都做过转义。所以 [save_post_handler](../main/ap_prov.c) 里是四步：

```text
① recv_body()              按 req->content_len 收，不是"读到读不动为止"        ap_prov.c
                           正文不保证以 '\0' 结尾，只能按长度收
② httpd_query_key_value()  把两个字段切出来 —— 它【不做】转义还原
③ url_decode()             自己还原 %XX 和 +（空格提交上来是 '+'，一个汉字三组 %XX）  ap_prov.c
④ 校验字节长度 → 发回执 → 点事件位                                        ap_prov.c
```

④ 之后还有一步：把两个值从临时缓冲区拷进模块自己的 `s_new_ssid` / `s_new_pass`。

> ### ⚠ 这一步里踩过的坑：缓冲区容量要按【转义后】算，不是按 SSID 算
>
> 最早图省事，让 `httpd_query_key_value()` 直接把值解进 `s_new_ssid` / `s_new_pass`
> —— 那两个的容量是协议上限加 `'\0'`（32+1、64+1）。看着天衣无缝，**其实错了一层**：
>
> | | 装进去的东西 | 一个汉字占 | 4 个汉字的中文热点名 |
> | --- | --- | --- | --- |
> | `httpd_query_key_value()` 切出来的 | **转义形式** | 9 字符（`%E8%B4%A5`） | **36 字符** |
> | `url_decode()` 之后 | 真正的 UTF-8 字节 | 3 字节 | **12 字节** |
>
> 拿 33 字节的 `s_new_ssid` 去装 36 个字符的转义串 → 直接返回"装不下" →
> 网页上弹出**"热点名没填或者太长"**。而它解码后只有 12 字节，离 32 字节的上限差得远。
>
> 实际能过的最长中文名是 **3 个汉字**（27 字符），第 4 个字一定被拒。
> 纯 ASCII 的热点名不受影响 —— 字母数字转义前后都是 1 个字符，所以 32 字符刚好卡在上限上，
> 测的时候很容易漏掉。
>
> **修法**：字段先切进按三倍开的临时缓冲区 `raw_ssid[100]` / `raw_pass[196]`
> （`(上限+1)×3+1`，多留一个字节的余量），`url_decode()` 之后再做长度校验，
> 通过了才拷进 `s_new_ssid` / `s_new_pass`。
> 余量那一字节的作用是让"超长一点点"也能解出来，好在网页上报出
> **"33 字节，要求 1~32"**，而不是笼统一句"没填或者太长"。
>
> 一句话记住：**长度校验必须放在 `url_decode()` 之后** —— 放在之前量的是转义串的长度。

> **校验为什么要在网页这一道先做一遍？** 因为**拿着手机的人看不见串口**。
> 少了这道，超长或者空的 SSID 会一路存进 NVS，页面已经说了"保存成功"，
> 然后干等两分钟又被踢回配网页面 —— 中间那句真正的原因只在串口上。
> wifi_sta.c 里还有第二道检查，那道才是最终把关的。

## 7. 架构上几条刻意的选择

| 选择 | 备选 | 为什么不那么做 |
| --- | --- | --- |
| 运行中切模式，**一次也不复位** | 收完凭据 `esp_restart()` | 继电器挂在 GPIO0 上，"关"就是把这个脚拉低，而 GPIO0 低电平正是**下载模式的 strapping 脚** —— 复位那一刻它被重新采样，采到低就直接进下载模式，程序根本不跑，串口一片安静（开发流程 §9.8 ①） |
| 只开 AP，**不开 STA+AP 混合** | 混合模式（不用先关 STA） | ① 两个接口各占一份协议栈资源，这块板子内存本来就紧；② 混合模式的热点是**常开**的 —— 等于把配网入口一直挂在空气里，旁边的人随时能连上来改你的凭据 |
| ap_prov.c **不碰 NVS** | 收完直接存进去 | 存哪儿、存不存、要不要先试试能不能连上，都是 wifi_sta.c 的事。ap_prov.c 只管"把两个字符串收上来交出去" |
| 用 SDK 的 `esp_http_server` | 自己写 socket 服务器 | 自己写就要自己处理 Content-Length、chunked、keep-alive、超长请求头 —— §10.8 那个 431 就是它替我们报出来的，自己写的话连报错都没有 |
| 不做 DNS、不做强制门户 | 一个 UDP 53 回任何域名 | 现在没做，所以要手输 IP。**这是可以加的**，见 §4 那个"没捡的便宜" |

## 8. 出问题按这个顺序查

先看配网横幅 —— 该知道的它都打出来了：

```text
[prov] ========== 进入配网模式 ==========
[prov] 热点  : ESP-01S-Setup-A4B2
[prov] 密码  : 12345678
[prov] 手机连上后打开 http://192.168.4.1/
[prov] 剩余内存: NNNNNN 字节          ← 内存够不够，看这个数
[prov] ================================
```

| 串口上看到 | 是什么 |
| --- | --- |
| `[wifi] 已经失败 30 次了，转配网模式` | 触发正常，接下来该开热点了 |
| `[prov] ✗ 热点起不来` | `AP_PASSWORD` 短于 8 字节？或者模式没切过去 |
| `[prov] ✗ HTTP 服务起不来` | 内存不够，看横幅里的剩余内存 |
| 热点连上了，页面就是打不开 | `lru_purge_enable` 没开（§5），或者浏览器在等一个没发完的 chunk（收尾那个长度 0 漏了） |
| `431 Request Header Fields Too Large` | 请求头缓冲区不够，`CONFIG_HTTPD_MAX_REQ_HDR_LEN`，见 §10.8 |
| `URI '/favicon.ico' not found` | 无害 |
| `AES PN: ...` | WiFi 驱动的重复包提示，和配网无关 |
| 点了保存没反应 | 回执和事件位的顺序反了（§3 第一条） |
| 收完凭据热点不关 | 卡在 1.5 秒那个 `vTaskDelay` 或 `httpd_stop()` 上 |
| **中文热点名填到第 4 个字就报"没填或者太长"** | 拿"解码后"的缓冲区去装"解码前"的转义串了，见 §6 那个坑。长度校验必须放在 `url_decode()` 之后 |

## 9. 想改的话改哪儿

| 想改什么 | 去哪儿 |
| --- | --- |
| 失败几次才转配网 | `WIFI_RETRY_BEFORE_AP`，[wifi_sta.h](../main/wifi_sta.h) |
| 多久检查一次、日志多久吭一声 | `WIFI_DOWN_REPORT_MS`，[wifi_sta.c](../main/wifi_sta.c) |
| 管理任务的栈 | [`wifi_sta.c` 里的 `wifi_sta_init()`](../main/wifi_sta.c) 那个 6144 |
| 谁来兜底 | [`main.c` 里的 `app_main()`](../main/main.c) 的 `wifi_sta_set_giveup_handler()` |
| 热点的名字前缀 / 密码 / 信道 / 最多几台 | ap_prov.c 顶部的 `AP_SSID_PREFIX` / `AP_PASSWORD` / `AP_CHANNEL` / `AP_MAX_CONN` |
| 网页长什么样 | ap_prov.c 的 `PAGE_HEAD` / `PAGE_TAIL` |
| 表单字段名 | `PAGE_TAIL` 的 `name=` **和** `save_post_handler` 里 `httpd_query_key_value()` 的实参，两处一起改 |
| 请求头最多能有多长 | `sdkconfig` 的 `CONFIG_HTTPD_MAX_REQ_HDR_LEN`，**改完必须重新编译** |
| 强制门户（手机自动弹页面） | 还没有。要加就是在 `start_ap_and_web()` 之后起一个 UDP 53 的应答器 |

> **加新文件时别忘两件事**：`main/CMakeLists.txt` 的 `SRCS` 里要加
> （`main/component.mk` 是自动扫描的，不用管）；确认新文件对 `wifi_sta.h` 的依赖仍然是单向的，
> 别为了图方便把 `ap_prov.h` include 进 `wifi_sta.c` —— 那一刀切下去就绕成环了。

## 10. 一页速查

```text
触发    wifi_sta.c 数到 30 次失败 → 回调 → ap_prov_run()
接线    main.c      wifi_sta_set_giveup_handler(ap_prov_run)
热点    ESP-01S-Setup-<AP MAC 后两字节> / 12345678 / 信道 1 / 最多 2 台
地址    192.168.4.1（编译期常量，不是 DHCP 分的）；手机拿到 192.168.4.2
DNS     没有。DHCP 把 192.168.4.1 当 DNS 给了手机，可那儿没人听 → 域名全废，只能输 IP
网页    SDK 的 esp_http_server，任务名 "httpd"，80 端口，两条路由 GET / 和 POST /save
数据    POST 表单 → 切字段 → 还原 %XX 和 + → 校验字节长度 → 回执 → 点事件位
交接    事件组 PROV_DONE_BIT；先发回执再点牌子，点完等 1.5 秒才拆
收工    httpd_stop() → esp_wifi_stop() → set_mode(STA)，全程不复位
返回    true = 两个缓冲区填好了；false = 没动，调用方回去接着重试
不碰    NVS、GPIO0（不复位）、STA+AP 混合模式
```
