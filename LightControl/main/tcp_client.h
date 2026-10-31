/**
  * @file    tcp_client.h
  * @brief   TCP 客户端模块 —— 对外接口
  *
  * 这个模块是【纯传输层】：只负责把字节搬过去、搬回来，
  * 完全不知道"开灯"是什么。业务通过 tcp_client_set_rx_handler() 接进来。
  *
  * @par 为什么必须是"客户端"
  *
  *   TCP 必须先建立连接，而连接【只能由客户端发起】。服务端只能被动等
  *   （listen → accept），在有人连上来之前，它连"发送对象"都不存在 ——
  *   网络调试助手会显示"当前没有连接对象"。
  *
  *   UDP 没有这个问题：它没有连接的概念，谁都能随时往谁那儿丢数据。
  *
  * @par 服务端地址在哪
  *
  *   tcp_client.c 顶部的 TCP_SERVER_IP / TCP_SERVER_PORT，是【编译期常量】。
  *   改的时候必须和 wifi_sta.c 顶部的 SSID / 密码【一起切】——
  *   那边管"连哪个网"，这边管"连网里的谁"。
  *
  *   只切一半的现象：WiFi 连上了（串口打印出 GOT_IP），TCP 却一直刷 errno=113。
  *
  * @warning 用电脑当主节点时，网络调试助手要监听 8086。
  *
  * @see     学习笔记/ESP8266-TCP-UDP-WiFi-STA.md §2（TCP 的 API 怎么用）、
  *          学习笔记/ESP8266开发流程.md §9.2（拓扑与四值表）
  */

#pragma once

/**
  * @brief    收到数据的回调
  *
  * @param[in] data  收到的字节
  * @param[in] len   字节数
  *
  * @warning  data 是【裸字节流】，不保证是一条完整消息 —— TCP 是字节流，
  *           不是消息队列。你点一次"发送"，这边可能：
  *
  *             一次收到完整的 "开灯"
  *             分两次收到 "开" 和 "灯"
  *             一次收到 "开灯关灯"（连点了两次）
  *
  *           所以回调里【不能】拿 data 直接去 strcmp，必须先攒起来再找关键字。
  *
  * @warning  data 也【不保证以 '\0' 结尾】，别当 C 字符串用。
  *           回调返回后这块内存就作废，要用请自己拷走。
  */
typedef void (*tcp_rx_handler_t)(const char *data, int len);

/**
  * @brief    注册"收到数据"的回调
  *
  * 🟢 L2 —— 工具：一次赋值。"必须在 start() 之前调"是调用方的规矩。
  *
  * @param[in] handler  上层提供的处理函数；传 NULL 表示不处理
  *
  * @warning  必须在 tcp_client_start() 【之前】调用。注册晚了，
  *           第一段到达的数据会因为回调还是 NULL 而被悄悄丢掉 ——
  *           连日志都不会打一行。
  */
void tcp_client_set_rx_handler(tcp_rx_handler_t handler);

/**
  * @brief    启用 TCP
  *
  * 🟡 L1 —— 架构：任务只建一次，重复调等于"确保它开着"；内部先等
             IP 再连，连不上每 2 秒重来。
  *
  * @note     任务只会被创建一次，重复调用等于"确保它开着" ——
  *           从别的模式切回来时直接调它就行，不会多出一条任务。
  *
  * @note     内部会先等 WiFi 拿到 IP，再去连服务端，
  *           所以可以放心地在 wifi_sta_init() 之后立刻调用，不会白跑。
  *
  * @note     连不上会每 2 秒自动重连，不需要调用方操心。
  */
void tcp_client_start(void);

/**
  * @brief    停用 TCP：关掉连接，任务转成空转，但【不退出】
  *
  * 🟡 L1 —— 架构：不阻塞、不等任务退出，代价是最长 5 秒的窗口里旧
             连接还在收 —— 挡包是 cmd 那边的事。
  *
  * @note     本函数【不阻塞】、【立刻返回】，也不会等任务真正停下来 ——
  *           所以在任何地方调用都是安全的（包括从接收回调里调）。
  *
  * @warning  代价是"停"不是瞬时的：真正断开要等 recv() 那一轮超时（最多 5 秒），
  *           或者重连等待那一轮（最多 2 秒）。这段窗口里到达的包会被
  *           main.c 的回调挡掉，不会被执行。
  *
  * @see      学习笔记/ESP8266-TCP-UDP-WiFi-STA.md §5.4（为什么只能这么写）
  */
void tcp_client_stop(void);

/**
  * @brief    往服务端发一段字节
  *
  * 🟡 L1 —— 架构：没连上就直接丢并打日志，不等 —— 拿"发送可能静默
             失败"换"不拖住命令解析"。
  *
  * @param[in] data  要发的数据
  * @param[in] len   字节数；传 0 表示"data 是 C 字符串，自己算长度"
  *
  * @return   实际发出的字节数；失败返回 -1
  *
  * @note     没连上时【不会阻塞等待】，而是直接丢弃并打一行日志 ——
  *           调用方（命令解析）不该因为网络没通就卡住。
  *
  * @warning  返回成功只说明"字节进了协议栈的发送缓冲区"，
  *           【不代表对方已经收到】。
  */
int tcp_client_send(const char *data, int len);
