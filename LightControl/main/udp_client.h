/**
  * @file    udp_client.h
  * @brief   UDP 传输模块 —— 对外接口
  *
  * 和 tcp_client.h 是一对：接口形状几乎一样，只是底层换成了 UDP。
  *
  * @par 为什么 UDP 这边没有"连接"这回事
  *
  *   UDP 不建立连接，也没有客户端/服务端的角色之分 —— 不需要 connect()，
 *   不需要 listen()/accept()，一个 socket 就能收发。这个模块做的事只有两件：
 *   bind 住本机一个端口，然后 sendto 到灯控组播组（地址和端口见 udp_client.c 顶部的 UDP_GROUP_IP / UDP_PORT）。
  *
  * @par 和 TCP 最本质的差别：UDP【保留消息边界】
  *
  *   TCP 那边点一次"发送"，这边可能分两次收到，也可能和下一次粘在一起。
  *   UDP 不会：发一次 = 收一次，一包就是一包，长度固定。
  *
  *   @warning 但这【不能】让业务层那套"先攒进缓冲区再找关键字"的逻辑退休 ——
  *            拆包确实不可能了，粘包却仍然可能：发送方完全可以在一包里
  *            塞"开灯关灯"两条命令。UDP 只帮你消掉了一半的坑。
  *
 * @warning 组播地址（UDP_GROUP_IP）和端口（UDP_PORT）在 udp_client.c 顶部，是编译期常量，
 *          必须和主控端【逐字一致】。它和 tcp_client.c 里的 TCP_SERVER_IP 是【两份独立的拷贝】，
 *          编译器不会帮你核对，改一处必须改另一处。
  *
  * @warning 用电脑当主节点时，网络调试助手要监听 8087（UDP），不是 8086。
  *
  * @par 主节点的地址为什么是写死的
  *
  *   想让它动态其实很容易：recvfrom() 会把发送方的地址交出来，从收到的包里
  *   "学习"一下就行了。这个模块【故意不这么做】——
  *
  *     那样等于让热点里任何一台设备，只要往本机端口丢一包，
  *     就能把自己变成"主节点"。
  *
  *   写死地址虽然死板，但它把"谁能指挥这个节点"钉死在编译期。
  *
  * @see     学习笔记/ESP8266-TCP-UDP-WiFi-STA.md §1（两者的区别）、§3（UDP 的 API）
  */

#pragma once

/**
  * @brief    收到数据的回调
  *
  * @param[in] data  这一包的字节
  * @param[in] len   这一包的字节数 —— 就是发送方一次 sendto 的长度，
  *                  不会多也不会少
  *
  * @note     和 tcp_client 的回调不同，这里【一包就是一条】，不存在拆包。
  *           但仍然可能是"两条命令塞在一包里"，所以业务层的循环匹配照样要做。
  *
  * @warning  data 不保证以 '\0' 结尾。回调返回后这块内存就作废，要用请自己拷走。
  */
typedef void (*udp_rx_handler_t)(const char *data, int len);

/**
  * @brief    注册"收到数据"的回调
  *
  * 🟢 L2 —— 工具：一次赋值。"必须在 start() 之前调"是调用方的规矩。
  *
  * @param[in] handler  上层提供的处理函数；传 NULL 表示不处理
  *
  * @warning  必须在 udp_client_start() 【之前】调用。注册晚了，
  *           第一包数据会因为回调还是 NULL 而被悄悄丢掉。
  */
void udp_client_set_rx_handler(udp_rx_handler_t handler);

/**
  * @brief    启用 UDP
  *
  * 🟡 L1 —— 架构：任务只建一次；内部先等 IP 再 bind，bind 失败每
             2 秒重试（比如端口被占）。
  *
  * @note     任务只会被创建一次，重复调用等于"确保它开着" ——
  *           从别的模式切回来时直接调它就行，不会多出一条任务。
  *
  * @note     内部会先等 WiFi 拿到 IP，再 bind 本机端口。
  *
  * @note     bind 失败会每 2 秒重试（比如端口被占）。
  */
void udp_client_start(void);

/**
  * @brief    停用 UDP：关掉 socket，任务转成空转，但【不退出】
  *
  * 🟡 L1 —— 架构：不阻塞、不等任务退出，真正关 socket 要等 recvfrom()
             那一轮超时（最多 5 秒）。
  *
  * @note     本函数【不阻塞】、【立刻返回】，也不会等任务真正停下来 ——
  *           所以在任何地方调用都是安全的（包括从接收回调里调）。
  *
  * @warning  代价是"停"不是瞬时的：真正关 socket 要等 recvfrom() 那一轮
  *           超时（最多 5 秒）。这段窗口里到达的包会被 main.c 的回调挡掉。
  *
  * @see      学习笔记/ESP8266-TCP-UDP-WiFi-STA.md §5.4（为什么只能这么写）
  */
void udp_client_stop(void);

/**
  * @brief    往主节点发一个数据报
  *
  * 🟢 L2 —— 工具：往写死的地址 sendto。地址为什么写死，见文件头。
  *
  * @param[in] data  要发的数据
  * @param[in] len   字节数；传 0 表示"data 是 C 字符串，自己算长度"
  *
  * @return   实际发出的字节数；返回 -1 表示当前没在用（数据被丢弃）
  *
  * @warning  UDP 的发送是"发出去就不管了"：这里返回成功【不代表】对方收到了。
  *           链路断、对方没开、对方端口没人监听，本地都看不出来。
  */
int udp_client_send(const char *data, int len);
