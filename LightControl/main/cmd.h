/**
  * @file    cmd.h
  * @brief   命令解析 —— 把收到的字节变成动作
  *
  * 这个模块吃进裸字节，吐出动作：拉继电器、换主链路、开关 MQTT、换热点。
  * 它不碰 socket，也不碰 GPIO —— 那两件事交给 link.c 和 relay.c。
  *
  * @par 用法
  *
  * 三个回调分别注册给三个传输模块，然后由 link_init() 启动链路：
  *
  *   @code
  *   tcp_client_set_rx_handler(cmd_on_tcp_rx);
  *   udp_client_set_rx_handler(cmd_on_udp_rx);
  *   mqtt_link_set_rx_handler(cmd_on_mqtt_rx);
  *   link_init(LINK_TCP);
  *   @endcode
  *
  * 三个都要注册 —— 现在最多有【两条同时开着】（一条主链路 + MQTT），
  * 每条都要有自己的入口。
  *
  * @par 回执发给谁
  *
  *   命令从哪条链路来，回执就发回哪条 —— 这个"来源"由回调的参数一路传到
  *   link_send()，全模块【没有】"当前链路"这种全局状态。理由：两条链路
  *   同时开着，回调又跑在两条不同的任务上，全局变量记不住、还会被覆盖。
  *
  * @note    本模块【不需要初始化】：状态都放在 static 变量里，启动时是零值
  *          （每条链路那份缓冲区长度都是 0，等于空）。
  *
  * @warning 回调进来的 data 是【裸字节流】，不是一条完整消息。本模块内部会
  *          自己攒、自己找关键字，调用方不要试图先"整理"一遍。
  *
  * @see     学习笔记/ESP8266-TCP-UDP-WiFi-STA.md §1.2（拆包与粘包）、
  *          §5.5（命令表为什么要"扫最靠前"）、§6（两个 C 坑）、
  *          学习笔记/ESP8266开发流程.md §9.3（命令表）、§9.4 / §9.5（两个坑的现象与排查）
  */

#pragma once

/**
  * @brief    注册给 tcp_client 的接收回调
  *
  * 🟢 L2 —— 工具：薄包装，判完来源就转给同一份解析逻辑。
  *
  * @param[in] data  收到的字节
  * @param[in] len   字节数
  *
  * @note     已经关掉的那条链路来的数据会被丢掉（见 link_is_open()）。
  */
void cmd_on_tcp_rx(const char *data, int len);

/**
  * @brief    注册给 udp_client 的接收回调
  *
  * 🟢 L2 —— 工具：薄包装，和 cmd_on_tcp_rx() 只差来源判断。
  *
  * @param[in] data  这一包的字节
  * @param[in] len   字节数
  *
  * @note     和 cmd_on_tcp_rx() 的区别【只在来源】—— 判断逻辑是同一份。
  */
void cmd_on_udp_rx(const char *data, int len);

/**
  * @brief    注册给 mqtt_link 的接收回调
  *
  * 🟢 L2 —— 工具：第三层薄包装，同样只差来源判断。
  *
  * @param[in] data  一条完整消息的内容
  * @param[in] len   字节数
  *
  * @note     三者共用同一张命令表、同一套"攒起来再找关键字"的逻辑，
  *           所以走 MQTT 也能用 on / off / net tcp / wifi 这些命令。
  */
void cmd_on_mqtt_rx(const char *data, int len);

/**
  * @brief    启动 30 秒周期状态上报任务（灯控 JSON 协议）
  *
  * 🟢 L2 —— 工具：开机由 main.c 调一次。任务创建幂等，重复调无害。
  *
  * @note     上报内容 {"cmd":"status","light":"bedroom","ok":<实际电平>,
  *           "version":1}，严格照灯控协议规范。它只【发往 UDP 组播组】
  *           （239.0.0.2:8889），和当前主链路是 TCP 还是 UDP 无关 ——
  *           所以即便主链路是 TCP，只要 UDP 那一路被启用（link_init(LINK_UDP)
  *           或发过 "net udp"），上报就能出去。
  *
  * @warning 这条上报走的是 UDP 组播的【发送】能力，不占用"接收主链路"那个
  *           名额，因此和 link.c 里"TCP/UDP 接收互斥"的规矩不冲突。
  *
  * @see      ../MusicPlaySyncSpeakers/docs/02-设计/灯控协议规范.md
  */
void cmd_start_status_reporter(void);
