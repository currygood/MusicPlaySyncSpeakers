/**
  * @file    tcp_client.c
  * @brief   TCP 客户端模块 —— 连服务端、收发字节
  *
  * 对外接口和回调契约见 tcp_client.h。
  *
  * @see     学习笔记/ESP8266-TCP-UDP-WiFi-STA.md §2（API 逐个讲）、
  *          §5.3（任务为什么永不退出）、§5.4（stop 为什么只置标志）
  */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* BSD socket 风格的那套 API（socket / connect / recv / send ...） */
#include "lwip/sockets.h"
#include "lwip/err.h"
#include "lwip/sys.h"

#include "wifi_sta.h"
#include "tcp_client.h"

/**
  * @brief 服务端地址（当前生效的是台面调试：电脑开移动热点 + 网络调试助手当服务器）
  *
  * @warning 它和 udp_client.c 顶部的 UDP_GROUP_IP、以及主控端是【两份独立的拷贝】，
  *          编译器不会帮你核对。改一个就必须改另一个。
  *
  * @note    正式部署时换成下面注释掉的那行（ESP32 主节点开 SoftAP）。
  */
#define TCP_SERVER_IP    "192.168.137.1"

/* 正式：ESP32 主节点开 SoftAP */
/* #define TCP_SERVER_IP    "192.168.4.1" */

/**
  * @brief 服务端监听的端口
  *
  * @warning 必须和主节点那边一致。用电脑当主节点时，网络调试助手要监听它。
  */
#define TCP_SERVER_PORT  8086

/** @brief 接收缓冲区大小（字节）。一次 recv 最多收这么多。 */
#define TCP_RX_BUF_SIZE     128

/**
  * @brief recv() 的接收超时（秒）
  *
  * @note  没有它的话，连接半死不活的时候 recv() 会永远阻塞，
  *        这个任务就再也醒不过来了。超时不算错误，靠 errno 区分。
  */
#define TCP_RECV_TIMEOUT_S  5

/**
  * @brief    当前连接的 socket，-1 表示没连上
  *
  * @note     存成全局的是为了让 tcp_client_send() 能在任务外面被调用。
  */
static int s_sock = -1;

/**
  * @brief 收到数据时往哪儿送。由 tcp_client_set_rx_handler() 填。
  */
static tcp_rx_handler_t s_rx_handler = NULL;

/**
  * @brief    本模块当前是不是"在用"
  *
  * @note     main.c 切链路时把它置成 false，任务在下一轮发现后自行关闭
  *           socket —— 关 socket 的动作【只由任务自己做】，别的任务不碰。
  */
static volatile bool s_running = false;

/**
  * @brief 任务是否已经创建过
  *
  * @note  这个标志让 start() 变成幂等的：切回 TCP 时直接调用即可，
  *        不会重复创建任务（重复创建的后果是两条任务共用一个 s_sock，
  *        一条在 recv、另一条把它 close 掉，谁都说不清接下来会怎样）。
  */
static bool s_task_started = false;

/* ======================= 对外接口 ======================= */
/* 接口契约都在 tcp_client.h 里，这里只记实现上不能动的地方。 */

void tcp_client_set_rx_handler(tcp_rx_handler_t handler)
{
    s_rx_handler = handler;
}

int tcp_client_send(const char *data, int len)
{
    if (!s_running || s_sock < 0) {
        printf("[tcp] 还没连上服务端，%d 字节被丢弃\n", len ? len : (int)strlen(data));
        return -1;
    }
    if (len == 0) {
        len = strlen(data);
    }
    return send(s_sock, data, len, 0);
}

/**
  * @brief    传输任务：连服务端 → 收发 → 断了就重连，永不退出
  *
  * 🟡 L1 —— 架构：等 IP→连接→recv→交回调→断了重连，永不退出。
             s_sock 必须先置 -1 再 close，否则别的任务会往已关的 fd 写。
  *
  * @param[in] pvParameters  任务参数，本模块没用
  *
  * @note     整个流程是一个死循环：连上 → 收数据 → 断了 → 等 1 秒 → 再连。
  *
  * @note     这个任务【不会退出】，s_running 为 false 时它只是空转。
  *           这样切模式就不用反复创建/删除任务，也不会出现两条任务
  *           抢同一个 socket 的局面。
  */
static void tcp_client_task(void *pvParameters)
{
    char rxbuf[TCP_RX_BUF_SIZE];

    /* 先等 WiFi 真的拿到 IP。没有 IP 的时候 connect() 必然失败，
       与其盲目重试，不如在这里阻塞等待，拿到 IP 再开工。
       UINT32_MAX = 一直等。 */
    if (!wifi_sta_wait_ip(UINT32_MAX)) {
        printf("[tcp] 等 IP 超时，放弃\n");
        vTaskDelete(NULL);
        return;
    }
    printf("[tcp] 已获得 IP，准备连接 %s:%d\n", TCP_SERVER_IP, TCP_SERVER_PORT);

    while (1) {
        struct sockaddr_in dest;
        int sock;

        /* 没被启用就空转。500ms 一轮：够快，切换手感不迟钝；
           够慢，空转时基本不占 CPU。

           ⚠ 这一句同时也兜住了下面那些 vTaskDelay(2000) 的重试等待 ——
             切走的时候最多多等一轮重试（2 秒），之后就会走到这里停下。 */
        if (!s_running) {
            vTaskDelay(500 / portTICK_PERIOD_MS);
            continue;
        }

        sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock < 0) {
            printf("[tcp] 创建 socket 失败: errno=%d\n", errno);
            vTaskDelay(2000 / portTICK_PERIOD_MS);
            continue;
        }

        /* 给 recv() 设接收超时，否则连接半死不活时它会永远阻塞。 */
        struct timeval tv = { .tv_sec = TCP_RECV_TIMEOUT_S, .tv_usec = 0 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        /* sockaddr_in 里的东西都是"网络字节序"，所以端口要用 htons() 转。
           sin_addr 用 inet_addr() 把点分十进制字符串直接转成 32 位整数。 */
        memset(&dest, 0, sizeof(dest));
        dest.sin_family      = AF_INET;
        dest.sin_port        = htons(TCP_SERVER_PORT);
        dest.sin_addr.s_addr = inet_addr(TCP_SERVER_IP);

        /* 失败时 errno 是查问题的第一手证据，最常撞见的两个：
             113 EHOSTUNREACH  —— 没有路由。多半是地址写错网段了
             111 ECONNREFUSED  —— 网络是通的，但那个端口上没人监听 */
        if (connect(sock, (struct sockaddr *)&dest, sizeof(dest)) != 0) {
            printf("[tcp] 连接 %s:%d 失败 errno=%d，2 秒后重试\n",
                   TCP_SERVER_IP, TCP_SERVER_PORT, errno);
            close(sock);
            vTaskDelay(2000 / portTICK_PERIOD_MS);
            continue;
        }

        printf("[tcp] 已连接上 %s:%d\n", TCP_SERVER_IP, TCP_SERVER_PORT);

        /* 对上层公开，tcp_client_send() 从这里拿到 fd */
        s_sock = sock;

        /* 先打个招呼：让调试助手那边立刻看到东西，
           一眼就能确认这条链路是双向通的。 */
        tcp_client_send("hello from ESP-01S\r\n", 0);

        /* 收数据循环。条件是 s_running 而不是 1：被切走之后，
           最多等 recv() 那一轮超时（5 秒）就会退出这个循环去关连接。
           这段窗口里到达的包由 main.c 那边挡掉，不会被执行。 */
        while (s_running) {
            int len = recv(sock, rxbuf, sizeof(rxbuf) - 1, 0);

            if (len > 0) {
                /* recv 交出来的是裸字节流，不是 C 字符串。
                   这里【不做任何解释】，原样交给业务层的回调，
                   由它决定这一堆字节是什么意思。 */
                if (s_rx_handler != NULL) {
                    s_rx_handler(rxbuf, len);
                }

            } else if (len == 0) {
                /* recv 返回 0 只有一个含义：对端主动关掉了连接 */
                printf("[tcp] 对端关闭了连接\n");
                break;

            } else {
                /* len < 0：可能只是超时（这 5 秒没数据），也可能是真出错 */
                if (errno == EWOULDBLOCK || errno == EAGAIN) {
                    continue;
                }
                printf("[tcp] 接收出错 errno=%d\n", errno);
                break;
            }
        }

        /* 先置 -1 再 close：s_sock 是对外公开的句柄，tcp_client_send()
           靠它判断能不能发。顺序反了的话，中间那一瞬间别的任务可能
           拿着一个已经关掉的 fd 去 send()。 */
        s_sock = -1;
        close(sock);

        /* 这句要说准：切走的时候【不会】再连了，日志里必须能看出来，
           否则查问题时会被"1 秒后重连"这句误导成"它应该自己回来了"。 */
        if (s_running) {
            printf("[tcp] 连接结束，1 秒后重连\n");
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        } else {
            printf("[tcp] 已停用\n");
        }
    }
}

void tcp_client_start(void)
{
    if (!s_task_started) {
        s_task_started = true;
        xTaskCreate(tcp_client_task, "tcp_client", 4096, NULL, 5, NULL);
    }
    s_running = true;
}

void tcp_client_stop(void)
{
    /* 只置标志、立刻返回 —— 不能在这里等任务退出：调用本函数的代码
       正跑在这个任务自己的栈上（回调链），等它退出就是在等自己。 */
    s_running = false;
}
