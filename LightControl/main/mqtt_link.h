/**
  * @file    mqtt_link.h
  * @brief   MQTT 链路 —— 连巴法云 broker、订阅主题、收发消息
  *
  * 和 tcp_client / udp_client 一样是【纯传输层】：只搬字节，完全不知道
  * "开灯"是什么。业务通过 mqtt_link_set_rx_handler() 接进来。
  *
  * @par 为什么叫 mqtt_link 而不是 mqtt_client
  *
  *   SDK 的 esp-mqtt 组件自己就有一份 mqtt_client.h
  *   （components/mqtt/esp-mqtt/include/），而且它的目录和 main/ 一样躺在
  *   全局的包含路径上。本模块要是也叫 mqtt_client.h，两边就会互相遮蔽 ——
  *   谁先被找到取决于编译器搜路径的顺序，不是能靠规矩维持的事。
  *
  * @par 它和另外两条链路不一样在哪
  *
  *   ① **对面不是一台设备，是一个 broker。** 收发都按【主题】走，不按地址，
  *      所以没有"服务端 IP"这个概念，也没有 TCP/UDP 那种明确的"对端"。
  *   ② **地址是域名**（mqtt.bemfa.com），要 DNS。所以它需要一个真的能上外网、
  *      会下发 DNS 的热点。ESP32 自己开的 SoftAP 没有 DNS，这条链路用不了；
  *      手机热点或路由器才行。
  *   ③ TCP 给的是【字节流】，MQTT 给的是【一条条完整的消息】。但为了和另外
  *      两条链路共用同一张命令表，收到的消息还是原样丢进 cmd.c 的缓冲区，
  *      由那套"攒起来再找关键字"的逻辑统一解析。
  *
  * @par 两个发布口（巴法云的约定）
  *
  *   <主题>/up    推给所有订阅了这个主题的设备；发布者自己收不到
  *   <主题>/set   只更新云端记着的值，不推给任何人
  *
  *   回执走 mqtt_link_send() → /up；/set 由 mqtt_link_send_set() 发，
  *   命令表里对应 cloud on / cloud off 两条。
  *
  * @warning 本模块的 stop() 【不会断开和 broker 的连接】，只让模块不再收发。
  *          这不是偷懒，是唯一安全的写法 —— 原因写在 mqtt_link_stop() 上。
  *
  * @see     学习笔记/ESP8266-TCP-UDP-WiFi-STA.md §5.5（链路和命令之间的三道闸）
  */

#pragma once

/**
  * @brief    收到一条 MQTT 消息的回调
  *
  * @param[in] data  消息内容
  * @param[in] len   字节数
  *
  * @note     和 tcp_client.h 里那个同名回调【不一样】：TCP 给的可能是半条
  *           消息，这里给的一定是【完整的一条】。
  *
  * @warning  尽管如此，两条规矩照旧：data 【不以 '\0' 结尾】，长度只看 len；
  *           回调返回后这块内存就作废，要用请自己拷走。
  */
typedef void (*mqtt_rx_handler_t)(const char *data, int len);

/**
  * @brief    注册"收到消息"的回调
  *
  * 🟢 L2 —— 工具：一次赋值。"必须在 start() 之前调"是调用方的规矩。
  *
  * @param[in] handler  上层提供的处理函数；传 NULL 表示不处理
  *
  * @warning  必须在 mqtt_link_start() 【之前】调用 —— 后者会立刻开始连接，
  *           注册晚了第一条到达的消息会因为回调还是 NULL 而被悄悄丢掉。
  */
void mqtt_link_set_rx_handler(mqtt_rx_handler_t handler);

/**
  * @brief    启用 MQTT：建客户端、开始连 broker
  *
  * 🟡 L1 —— 架构：幂等，重复调等于"确保它开着"；连不上由 esp-mqtt 自己
  *           每 10 秒重来，不需要调用方操心。
  *
  * @note     本函数【不阻塞】：它只是把 esp-mqtt 自己那条任务建起来，
  *           真正的连接、重连、心跳都在那条任务里跑。
  *
  * @note     它自己不等 IP，但调用方【要等】：broker 是域名，没有 IP 就没有
  *           DNS，这时候启起来只是在白转。所以 main.c 是先
  *           wifi_sta_wait_ip() 拿到 IP，再 link_init() → 走到这里。
  *           不等也不会崩 —— 连不上它自己会退后 10 秒再试一遍。
  *
  * @note     客户端只会被 init 一次。切走再切回来时直接调它即可，
  *           不用重新建，也不会多出一条任务。
  *
  * @note     连上之后会自动订阅主题；【每次重连都会重新订阅一遍】，
  *           因为 clean session 是开着的，broker 不会替我们记着订阅关系。
  *           漏了这一步的现象是"日志说连上了，但发消息过来没反应"。
  */
void mqtt_link_start(void);

/**
  * @brief    停用 MQTT：不再收发，但【不断开连接】
  *
  * 🟡 L1 —— 架构：只置一个标志。代价是连接和它那条任务一直留着 ——
  *           最省事的写法也是最安全的写法，理由见下。
  *
  * @note     刻意【不】调用 esp_mqtt_client_stop()。那个函数有两个坑，
  *           踩中任何一个都会让整条 MQTT 链路废掉：
  *
  *             ① 它一旦发现自己是跑在 MQTT 自己的任务里，就直接返回错误，
  *                而返回前【没有把 API 锁放掉】。那是一把递归锁，
  *                漏掉一次 take，之后任何任务再调 MQTT 的 API 都会永久卡死。
  *                而本函数正是会被 link_set_mqtt() 从接收回调里调起来的
  *                （发一条 mqtt off 就走到这儿）—— 也就是跑在 MQTT 任务
  *                自己的栈上，必踩。
  *
  *             ② 就算不在 MQTT 任务里，它也会阻塞等待那条任务退出
  *                （portMAX_DELAY，无限等），和另外两个传输模块
  *                "只置标志、立刻返回"的约定正好相反。
  *
  *           所以这里只置标志：不再收（由事件回调看着）、不再发
  *           （由 mqtt_publish() 看着）。链接层要的"关掉之后这一路立刻失效"，
  *           由 cmd_on_rx_from() 那道开关判断兜底 —— 它才是真正管用的那道闸。
  *
  * @warning  因此【资源不会释放】：连接、socket、那条 6KB 栈的任务都还在。
  *           如果将来发现内存不够，正确的做法是让 esp-mqtt 别自己重连
  *           （disable_auto_reconnect）再另起一条任务管生命周期，
  *           而不是把 esp_mqtt_client_stop() 塞进这里。
  */
void mqtt_link_stop(void);

/**
  * @brief    把一段字节发布到 <主题>/up
  *
  * 🟡 L1 —— 架构：没连上就直接丢并打日志，不等 —— 和 tcp_client_send()
  *           一个取舍：拿"发送可能静默失败"换"不拖住命令解析"。
  *
  * @param[in] data  要发的数据
  * @param[in] len   字节数；传 0 表示"data 是 C 字符串，自己算长度"
  *
  * @return   发出去的字节数；失败返回 -1
  *
  * @note     推给所有订阅了这个主题的设备，【发布者自己收不到】——
  *           这是 broker 的规矩，不是本模块的。
  *
  * @warning  用的是 QoS 0：发出去了不代表对方收到了，返回成功只说明
  *           字节进了协议栈。
  */
int mqtt_link_send(const char *data, int len);

/**
  * @brief    把一段字节发布到 <主题>/set
  *
  * 🟡 L1 —— 架构：和 mqtt_link_send() 只差一个主题，但语义完全不同 ——
  *           这条【不会推给任何人】，只是让云端记着的值变成这段内容。
  *
  * @param[in] data  要发的数据
  * @param[in] len   字节数；传 0 表示"data 是 C 字符串，自己算长度"
  *
  * @return   发出去的字节数；失败返回 -1
  *
  * @note     什么时候用它：只想让云端的值跟上，又不想惊动别的设备。
  *           走 /up 通知同伴的话，两台设备会互相把对方的消息当成命令，
  *           来回弹；/set 没有这个回路。
  */
int mqtt_link_send_set(const char *data, int len);
