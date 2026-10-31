/**
  * @file    link.h
  * @brief   链路选择 —— 哪几条链路开着、回执往哪条发
  *
  * 现在最多可以【同时开着两条】：
  *
  *   主链路（对上那条，跟 ESP32）  tcp_client 或 udp_client，二选一
  *   第二路（对云那条，巴法云）    mqtt_link，单独开关
  *
  * 各收各的、各回各的 —— 哪条链路发来的命令，回执就发回哪条。所以这个模块
  * 回答的是两个问题，而不是一个：
  *
  *   ① 哪几条开着       → link_is_open()
  *   ② 这条命令从哪来   → 由调用方顺着参数一路传进来（link_send(to, ...)）
  *
  * 【不再有】全局的"当前链路"了。两条在同时收，一个全局变量记不住两个答案；
  * 而且三条链路的接收回调跑在三条【不同的任务】上（tcp 一条、udp 一条、
  * MQTT 一条），那个全局变量会被互相覆盖 —— "谁问的就回给谁"只能靠参数传，
  * 不能靠全局状态。
  *
  * @warning 主链路【同时只开一条】：tcp 和 udp 都是跟同一个 ESP32 说话的两条
  *          路，两条都开的话同一条命令会被执行两次、回执也发两条。
  *          MQTT 不在这条约束里 —— 它的对端是 broker，不是那个 ESP32。
  *
  * @warning "停用"对三条链路来说不是一回事：tcp/udp 的 stop() 会把 socket
  *          关掉，MQTT 的 stop() 【不断开和 broker 的连接】（原因见
  *          mqtt_link.h）。所以真正让一条链路失效的【不是】stop()，
  *          而是 cmd_on_rx_from() 那道开关判断 —— 关掉能立刻生效靠的是它。
  *
  * @see     学习笔记/ESP8266-TCP-UDP-WiFi-STA.md §5.5（链路和命令之间的三道闸，
  *          这里是第一道）
  */

#pragma once

#include <stdbool.h>

/**
  * @brief 走哪条链路
  */
typedef enum {
    LINK_TCP,    ///< 走 tcp_client.c（主链路：连 ESP32 的 TCP 服务端）
    LINK_UDP,    ///< 走 udp_client.c（主链路：直接把数据报丢给 ESP32）
    LINK_MQTT,   ///< 走 mqtt_link.c（第二路：连巴法云 broker，要能上外网的热点）
    LINK_COUNT,  ///< 一共几条链路 —— 只用来开数组，它不是一条真的链路
} link_mode_t;

/**
  * @brief    开机：定下主链路并启动它，同时把 MQTT 那一路也开起来
  *
  * 🟡 L1 —— 架构：开机之后哪几路是开的，全工程只有这一处；调它之前三个
             接收回调必须都已注册 —— 它一返回就有链路在收了。
  *
  * @param[in] master  主链路走哪条：LINK_TCP 或 LINK_UDP
  *
  * @warning  必须在三个模块的 set_rx_handler() 【之后】调用 ——
  *           它会立刻 start，而注册晚了第一段数据会因为回调还是 NULL
  *           而被悄悄丢掉。
  *
  * @warning  调用之前 WiFi 必须【已经拿到 IP】。MQTT 那一路的 broker 是域名
  *           （mqtt.bemfa.com），没有 IP 就没有 DNS，启起来只能白转 ——
  *           所以 main.c 里是等 wifi_sta_wait_ip() 返回之后才调过来的。
  *           真不等也不会崩（esp-mqtt 会自己每 10 秒重试一遍），但那不是
  *           "能用"，只是"没崩"。
  *
  * @note     MQTT 那一路是【默认开着】的：本函数会顺手调 mqtt_link_start()。
  *           不想要就在开机后发一条 mqtt off，或者把这里那行去掉。
  *           它不阻塞 —— 连不上（比如热点根本没有外网）会自己按 10 秒的
  *           间隔一直重试，串口上会看到它反复报错。
  */
void link_init(link_mode_t master);

/**
  * @brief    这一路现在收不收
  *
  * 🟢 L2 —— 工具：纯查询。给接收回调判断"这包数据是不是已经关掉的那条
  *           链路送来的"用。
  *
  * @param[in] mode  哪条链路
  *
  * @retval    true   开着，数据要处理
  * @retval    false  已经关了，数据要丢掉
  *
  * @note      为什么要判：stop() 都只置个标志就返回，链路不会立刻安静下来 ——
  *            tcp/udp 要等下一次超时（最多 5 秒）才真正断开，MQTT 那个则
  *            一直在收。这段窗口里要是放行，关掉之后发来的命令照样会被执行。
  */
bool link_is_open(link_mode_t mode);

/**
  * @brief    现在的主链路是哪条
  *
  * 🟢 L2 —— 工具：读一个 static 变量，黑盒测即可。
  *
  * @return   LINK_TCP 或 LINK_UDP —— MQTT 不在主链路里
  *
  * @note     用它的是"换主链路之前先记下要关的是哪条"：换完再问，答案就是
  *           新的那条了。
  */
link_mode_t link_master(void);

/**
  * @brief    把一段字节发到指定的那一条链路
  *
  * 🟢 L2 —— 工具：按参数选路转发。规矩是"所有回执都走它"，
  *           但函数本身只有几行。
  *
  * @param[in] to    发给哪条链路 —— 【就是这条命令从哪条链路来的】
  * @param[in] data  要发的数据
  * @param[in] len   字节数；传 0 表示"data 是 C 字符串，自己算长度"
  *
  * @return   实际发出的字节数；失败返回 -1
  *
  * @warning  所有回执一律调它，【不要直接调 tcp_client_send()】。
  *           漏掉哪一处，那条回执就跑到别的链路去了 ——
  *           命令从 MQTT 进来了，回复却发去了 TCP，
  *           发送方会觉得"命令生效了但没回音"。
  *
  * @warning  to 要从命令行一路传下来，【不能改回全局变量】：三条链路跑在
  *           三条任务上，全局变量会被别的任务覆盖掉。
  *
  * @warning  MQTT 那条路上回执是被【广播】出去的（推到 <主题>/up，所有订阅者
  *           都收得到），对面要是也跑这套固件，就会把你的回执当成一条命令
  *           去解析。所以回执文案里不能含命令表里的关键字 ——
  *           曾经有一句 "WIFI OK (reconnecting)"，里面那个小写的 "on"
  *           会让对面的继电器吸合。一句话：回执一律全大写。
  */
int link_send(link_mode_t to, const char *data, int len);

/**
  * @brief    换主链路，并替调用方发掉那条回执
  *
  * 🟡 L1 —— 架构：先用【命令来的那条路】发回执、再换，反过来对方就永远等不到
             回复；而且只能置标志、不能等任务退出（会在自己的栈上死锁）。
  *
  * @param[in] reply_to  这条命令是从哪条链路来的（回执发回那儿）
  * @param[in] mode      主链路换成哪条：LINK_TCP 或 LINK_UDP
  *
  * @note      回执由本函数【自己】发，调用方不要再补一句 link_send() ——
  *            补了会发两遍。
  *
  * @note      【顺序是有意的】：先用命令来的那条路把回执发出去，发完才换。
  *            反过来的话这条回执就没处发了 —— 命令从 TCP 进来、而 TCP
  *            正是要被换掉的那条，发送方永远等不到结果。现象就是"发了
  *            net udp 之后就再没动静了"，很容易误判成板子死了。
  *
  * @warning   换的【只是主链路】：MQTT 那一路上还是下，本函数不管。
  *
  * @warning   切换【不是瞬时】的：新链路那条任务要等它自己那一轮空转醒来
  *            （最多 500ms）才会建好 socket 并报到。所以切到 UDP 之后，
  *            主节点最快也要过半秒左右才发得进来。
  *
  * @warning   会调用另一条主链路的 stop()。它们的 stop() 都只置标志就返回，
  *            【不能】改成"等对方任务退出"：本函数是顺着接收回调被调用的，
  *            也就是跑在接收任务【自己】的栈上，等它退出就是在等自己，直接死锁。
  */
void link_switch(link_mode_t reply_to, link_mode_t mode);

/**
  * @brief    开关 MQTT 那一路，并替调用方发掉那条回执
  *
  * 🟡 L1 —— 架构：幂等，重复"开"等于确保它开着；主链路不受影响。
  *
  * @param[in] reply_to  这条命令是从哪条链路来的（回执发回那儿）
  * @param[in] on        true 开、false 关
  *
  * @note      回执由本函数【自己】发，而且是【先发回执、再关】——
  *            关掉之后再想回话就没路了。
  *
  * @note      关掉之后【连接还在】（理由见 mqtt_link.h）：不再收、不再发，
  *            但那条任务和那条 socket 都留着，所以再打开是立刻的，
  *            不用重连、也不用重新订阅。
  */
void link_set_mqtt(link_mode_t reply_to, bool on);
