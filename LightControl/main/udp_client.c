/**
  * @file    udp_client.c
  * @brief   UDP 传输模块 —— 不建连接，直接往组播组丢数据报
  *
  * 对外接口和回调契约见 udp_client.h。
  *
  * @warning 这个模块和 tcp_client 是【二选一】的，同一时刻只该有一个在收。
  *          两个一起开的话，同一条命令会从两条路各到一次 —— "开灯"会被
  *          执行两次，"关灯"也是。切换逻辑在 link.c 的 link_switch()。
  *          （MQTT 不在这条约束里：它的对端是 broker，可以和主链路并存。）
  *
  * @note    本模块现在工作在【组播】模式下：bind 到本机 INADDR_ANY:UDP_PORT
  *          之后，再用 IP_ADD_MEMBERSHIP 加入 UDP_GROUP_IP 这个组播组。
  *          灯控主控（音频主控 / ESP32）也加入同一个组，于是"一发多收"
  *          天然成立 —— 主控发一条开关命令，所有灯控节点都能收到。
  *          30 秒周期的状态上报（见 cmd.c）同样发往这个组播组，由 cmd 模块
  *          统一负责，本模块只管把字节丢到组播地址去。
  *
  * @see     学习笔记/ESP8266-TCP-UDP-WiFi-STA.md §3（UDP 的 API）、
  *          §5.3（任务为什么永不退出）、§5.4（stop 为什么只置标志）
  */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 和 tcp_client.c 同一套 BSD socket API，只是多了 bind / sendto / recvfrom。
   INADDR_ANY 来自 lwip/inet.h，它已经被 lwip/sockets.h 转手带进来了。 */
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/err.h"
#include "lwip/sys.h"

#include "wifi_sta.h"
#include "udp_client.h"

/**
  * @brief 灯控组播组地址
  *
  * @note  所有灯控节点和主控都加入这个组（239.0.0.0/8 是"组织本地作用域"
  *        的组播地址，不会出路由器，适合局域网内一发多收）。地址是灯控协议
  *        约定的一部分，必须和主控端【逐字一致】。
  *
  * @warning 它和 tcp_client.c 顶部的 TCP_SERVER_IP、以及主控端是【两份独立
  *          的拷贝】，编译器不会帮你核对。
  *
  * @see     ../MusicPlaySyncSpeakers 里的灯控协议规范
  */
#define UDP_GROUP_IP   "239.0.0.2"

/**
  * @brief 组播端口
  *
  * @note  本端 bind 的端口和发送目的端口用的是【同一个值】：收到组播包时，
  *        源地址是 主控IP:UDP_PORT；回包 / 状态上报时我们同样发往
  *        UDP_GROUP_IP:UDP_PORT，主控和所有节点都在这个组里听着。
  *
  * @warning 必须和主控端一致，而且【不】要设成 8086 —— 那是 TCP 那边的端口。
  */
#define UDP_PORT        8889

/** @brief 接收缓冲区大小（字节）。一个数据报超过它，多出来的部分会被丢掉。 */
#define UDP_RX_BUF_SIZE     128

/**
  * @brief recvfrom() 的接收超时（秒）
  *
  * @note  这个超时在这里的唯一作用是：让任务有机会看到 s_running 被置成 false，
  *        否则会永远阻塞。周期性的"报到 / 状态上报"已经交给 cmd.c 的定时器任务
  *        统一负责，本模块不再自己发心跳。
  */
#define UDP_RECV_TIMEOUT_S  5

/**
  * @brief    当前 socket，-1 表示没在收
  *
  * @note     存成全局的是为了让 udp_client_send() 能在任务外面被调用。
  */
static int s_sock = -1;

/**
  * @brief 收到数据时往哪儿送。由 udp_client_set_rx_handler() 填。
  */
static udp_rx_handler_t s_rx_handler = NULL;

/**
  * @brief    这个模块当前是不是"在用"
  *
  * @note     main.c 切链路时把它置成 false，任务在下一轮发现后自行关闭
  *           socket —— 关 socket 的动作【只由任务自己做】，别的任务不碰。
  */
static volatile bool s_running = false;

/**
  * @brief 任务是否已经创建过
  *
  * @note  这个标志让 start() 变成幂等的：切回 UDP 时直接调用即可，
  *        不会重复创建任务（重复创建的后果是两条任务抢同一个 s_sock）。
  */
static bool s_task_started = false;

/* ======================= 对外接口 ======================= */
/* 接口契约都在 udp_client.h 里，这里只记实现上不能动的地方。 */

void udp_client_set_rx_handler(udp_rx_handler_t handler)
{
    s_rx_handler = handler;
}

int udp_client_send(const char *data, int len)
{
    struct sockaddr_in dest;

    if (!s_running || s_sock < 0) {
        printf("[udp] 当前没在用，%d 字节被丢弃\n", len ? len : (int)strlen(data));
        return -1;
    }
    if (len == 0) {
        len = strlen(data);
    }

    memset(&dest, 0, sizeof(dest));
    dest.sin_family      = AF_INET;
    dest.sin_port        = htons(UDP_PORT);
    /* 发往组播组，而不是某个固定单播地址 —— 主控和所有节点都在组里听着。
       加入组播组这一步在 udp_client_task() 里做（bind 之后）。 */
    dest.sin_addr.s_addr = inet_addr(UDP_GROUP_IP);

    return sendto(s_sock, data, len, 0,
                  (struct sockaddr *)&dest, sizeof(dest));
}

/**
  * @brief    传输任务：bind 本机端口 → 加入组播组 → 收发 → 没在用了就空转
  *
  * 🟡 L1 —— 架构：等 IP→bind→加入组播组→recvfrom→交回调，永不退出；
  *             recvfrom() 的超时值只决定 stop 之后多久真正关 socket。
  *
  * @param[in] pvParameters  任务参数，本模块没用
  *
  * @note     注意这个任务【不会退出】，s_running 为 false 时它只是空转。
  *           这样切模式就不用反复创建/删除任务，也不会出现两条任务
  *           抢同一个 socket 的局面。
  *
  * @note     周期性的"报到 / 状态上报"已经交给 cmd.c 的定时器任务统一负责
  *           （见 cmd_start_status_reporter()），本模块不再自己发心跳。
  */
static void udp_client_task(void *pvParameters)
{
    char rxbuf[UDP_RX_BUF_SIZE];

    /* 先等 WiFi 真的拿到 IP。没有 IP 的时候 bind / sendto 都没意义，
       与其盲目重试，不如在这里阻塞等待。UINT32_MAX = 一直等。 */
    if (!wifi_sta_wait_ip(UINT32_MAX)) {
        printf("[udp] 等 IP 超时，放弃\n");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        struct sockaddr_in local;
        struct timeval     tv;
        int                sock;

        /* 没被启用就空转。500ms 一轮：够快，切换手感不迟钝；
           够慢，空转时基本不占 CPU。 */
        if (!s_running) {
            vTaskDelay(500 / portTICK_PERIOD_MS);
            continue;
        }

        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock < 0) {
            printf("[udp] 创建 socket 失败: errno=%d\n", errno);
            vTaskDelay(2000 / portTICK_PERIOD_MS);
            continue;
        }

        /* 接收超时。没有它的话 recvfrom() 会永远阻塞，这个任务既看不到
           s_running 变成 false，也没有机会让 socket 真正关掉。超时不算错误，
           下面靠 errno == EWOULDBLOCK 把它和真错误区分开。 */
        tv.tv_sec  = UDP_RECV_TIMEOUT_S;
        tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        /* bind：把本机端口固定下来。地址填 INADDR_ANY，意思是"收本机所有
           网卡上的这个端口" —— 组播包和单播回包都能进到这个 socket。 */
        memset(&local, 0, sizeof(local));
        local.sin_family      = AF_INET;
        local.sin_port        = htons(UDP_PORT);
        local.sin_addr.s_addr = INADDR_ANY;

        /* 失败要先 close 再重试，而且下一轮会重新 socket()：
           端口被占时，拿同一个 socket 反复 bind 会一直失败。 */
        if (bind(sock, (struct sockaddr *)&local, sizeof(local)) != 0) {
            printf("[udp] bind 端口 %d 失败 errno=%d，2 秒后重试\n", UDP_PORT, errno);
            close(sock);
            vTaskDelay(2000 / portTICK_PERIOD_MS);
            continue;
        }

        /* 加入组播组：这是【能收到组播命令】的关键一步。不加入的话，
           主控往 239.0.0.2 发的数据报在本机的网络栈里会被直接丢弃，
           即便 bind 了同一个端口也收不到（某些交换机 / 操作系统下尤其如此）。
           imr_interface 填 INADDR_ANY = 让栈自己挑出口网卡（本机只有 WiFi 一张）。 */
        struct ip_mreq mreq;
        mreq.imr_multiaddr.s_addr = inet_addr(UDP_GROUP_IP);
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                       &mreq, sizeof(mreq)) != 0) {
            printf("[udp] 加入组播组 %s 失败 errno=%d（仍可收单播回包）\n",
                   UDP_GROUP_IP, errno);
            /* 不致命：组播加不进，至少单播回包还能收。但灯控命令多半是组播，
               所以这一行报错要当回事，先查网络 / 交换机是否禁了组播。 */
        } else {
            printf("[udp] 已加入组播组 %s:%d\n", UDP_GROUP_IP, UDP_PORT);
        }

        s_sock = sock;
        printf("[udp] 已就绪，本机端口 %d，发往组播 %s:%d\n",
               UDP_PORT, UDP_GROUP_IP, UDP_PORT);

        while (s_running) {
            struct sockaddr_in from;
            socklen_t          fromlen = sizeof(from);

            /* from 拿不拿都行 —— 本模块【故意不】用源地址去更新主节点的
               地址，理由见 udp_client.h（免得热点里谁丢一包就冒充成主节点）。
               留着这个变量只是因为 recvfrom 的参数不能传 NULL。 */
            int len = recvfrom(sock, rxbuf, sizeof(rxbuf), 0,
                               (struct sockaddr *)&from, &fromlen);

            if (len > 0) {
                /* 和 TCP 那边一样，recvfrom 交出来的是裸字节，也不保证是
                   C 字符串，这里【不做任何解释】，原样交给业务层的回调。 */
                if (s_rx_handler != NULL) {
                    s_rx_handler(rxbuf, len);
                }

            } else if (errno == EWOULDBLOCK || errno == EAGAIN) {
                /* 只是这 5 秒没数据。超时只用来让本任务有机会看到
                   s_running 变成 false，不再承担心跳职责。 */

            } else {
                printf("[udp] 接收出错 errno=%d\n", errno);
                break;
            }
        }

        s_sock = -1;
        close(sock);
        printf("[udp] 已停用\n");
    }
}

void udp_client_start(void)
{
    if (!s_task_started) {
        s_task_started = true;
        xTaskCreate(udp_client_task, "udp_client", 4096, NULL, 5, NULL);
    }
    s_running = true;
}

void udp_client_stop(void)
{
    /* 只置标志、立刻返回 —— 不能在这里等任务退出：调用本函数的代码
       正跑在这个任务自己的栈上（回调链），等它退出就是在等自己。 */
    s_running = false;
}
