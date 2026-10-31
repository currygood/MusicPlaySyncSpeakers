/**
  * @file    wifi_sta.h
  * @brief   WiFi STA（客户端）连接模块 —— 对外接口
  *
  * 这个模块把"连上一个热点"整个封起来。外面只看得见五个函数：
  *
  *   wifi_sta_init()               初始化 + 开始连接（异步，不阻塞）
  *   wifi_sta_wait_ip()            阻塞等拿到 IP —— 这才是"真的连上了"
  *   wifi_sta_set_credentials()    换一个热点，存 NVS 并立刻重连
  *   wifi_sta_get_ssid()           当前用的热点名
  *   wifi_sta_set_giveup_handler() 连不上了叫谁来救（现在是 ap_prov.c）
  *
  * @par 调用顺序
  *
  *   wifi_sta_init() 是【异步】的，它返回时还没连上。要和网络打交道，
  *   必须先等 wifi_sta_wait_ip()：
  *
  *   @code
  *   wifi_sta_init();
  *   wifi_sta_wait_ip(UINT32_MAX);   // 等到这一步之后，建 socket 才有意义
  *   @endcode
  *
  *   没有 IP 的时候 socket 建了也没用，所以 tcp_client / udp_client
  *   内部第一件事就是这个等待。
  *
  * @par 热点名和密码从哪来
  *
  *   三层，从上往下找：
  *
  *     ① NVS（命名空间 "app_cfg"）—— 运行时改过就用这个，掉电不丢
  *     ② wifi_sta.c 顶部的 WIFI_SSID_DEFAULT / WIFI_PASSWORD_DEFAULT
  *        —— 编译进固件里的"出厂默认值"
  *     ③ 都没有 → 用 ②，并顺手写进 ①
  *
  *   @warning 这意味着 ② 【只在第一次上电时有效】。NVS 里一旦有了凭据，
  *            改 ② 再重烧也不会生效。改热点请用 wifi_sta_set_credentials()。
  *
  * @par 连不上怎么办
  *
  *   连续失败 30 次之后，本模块会去叫 wifi_sta_set_giveup_handler() 注册进来的
  *   那个回调 —— 接上线的（见 main.c）是 ap_prov.c，它会把自己变成一个热点，
  *   让人用手机连上来、在网页里把新的 SSID 和密码填进去。
  *
  *   @warning 没注册回调就退回老行为：一直重试，只在串口上每隔 30 秒吭一声，
  *            那时候节点一旦连不上主节点，就只剩拆下来重烧一条路。
  *
  *   @warning 但配网这条路【也只救得了"改错了凭据"】。它救不了：
  *            热点只在 5GHz 上、板子离得太远、主节点根本没开机 ——
  *            这些不是填个新名字能解决的。
  *
  * @warning 改主节点凭据的顺序仍然是：① 先发命令改从节点（它此时还连得上）
  *          ② 再改主节点。配网兜底是用来救这个顺序写反了的，不是用来省掉它的。
  *
  * @warning 这件事【跟 OTA 无关】：OTA 是换固件，改密码是改配置。
  *
  * @see     学习笔记/ESP8266-NVS.md（凭据为什么放 NVS、怎么擦除）、
  *          学习笔记/ESP8266开发流程.md §9.6（换热点的操作纪律）、§9.7（连不上怎么查）、
  *          §10（连不上的兜底：AP 配网模式）
  */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/**
  * @brief    初始化并开始连接
  *
  * 🟡 L1 —— 架构：注册事件回调 + 起管理任务，6144 那个栈大小在这里定；
             异步，返回时还没连上。
  *
  * @note     本函数【不阻塞】：返回时连接还在后台进行，过程和结果由串口日志输出。
  *           要等"真的拿到 IP"，调 wifi_sta_wait_ip()。
  *
  * @note     内部会再起一条管理任务：没连上时每隔 30 秒在串口上报一次状态，
  *           提示该去查 SSID 拼写、密码、以及热点是不是只在 5GHz 上。
  *           它【只报不修】—— 重连本身是事件驱动的，不需要定时器。
  *
  * @warning  必须在任何网络操作之前调用，而且只调用一次。
  */
void wifi_sta_init(void);

/**
  * @brief    阻塞等待，直到拿到 IP（DHCP 分配成功，也就是真的能通信了）
  *
  * 🟡 L1 —— 架构："真的连上了"的唯一标志；不清事件位，所以要照顾
             同时在等的其它任务。
  *
  * @param[in] timeout_ms  超时毫秒数；传 UINT32_MAX 表示一直等
  *
  * @retval   true   拿到 IP 了
  * @retval   false  超时了
  *
  * @note     "连上了"的标志是【拿到 IP】，不是"关联上 AP"：关联成功时还没有 IP，
  *           那个阶段建 socket 一样是白建。
  *
  * @note     多个任务可以同时在这里等 —— 本函数【不清除】事件位，所以不影响别人。
  */
bool wifi_sta_wait_ip(uint32_t timeout_ms);

/**
  * @brief    换一个热点：写进 NVS（掉电不丢）+ 立刻用新凭据重连
  *
  * 🟡 L1 —— 架构：先存 NVS 再重连，顺序和字节长度检查都不能省；
             失败时当前连接不受影响。
  *
  * @param[in] ssid      新的热点名，长度 1 ~ 32 字节
  * @param[in] password  新的密码，长度 <= 64 字节；可以是空串（开放热点）
  *
  * @retval   true   凭据已存进 NVS 并开始切换
  * @retval   false  参数不合法或写 NVS 失败 —— 此时【当前连接不受影响】
  *
  * @warning  长度单位是【字节】不是字符 —— 一个中文 SSID 一个字 3 字节。
  *           超长的 SSID 被截断后照样能写进 NVS，但永远连不上，
  *           现象是"命令说成功了、就是连不上"，最难查。
  *
  * @note     切换【不是瞬时】的：函数返回后要等新连接建立起来，
  *           期间当前连接会断开一次。
  */
bool wifi_sta_set_credentials(const char *ssid, const char *password);

/**
  * @brief    读出当前生效的热点名
  *
  * 🟢 L2 —— 工具：返回内部静态缓冲区，别 free、别长期保存。
  *
  * @return   指向模块内部静态缓冲区的指针
  *
  * @warning  不要 free()，也不要长期保存 —— 它会在换凭据时被就地改写。
  */
const char *wifi_sta_get_ssid(void);

/**
  * @brief    连续失败这么多次之后，改走配网模式
  *
  * @note     30 次【不是 30 秒】：每失败一次都要先扫一遍信道、再等认证超时，
  *           一次几秒，所以 30 次大致是【一两分钟】。嫌等得久就把这个数字改小，
  *           改它不用动别的地方。
  *
  * @note     这个值是 wifi_sta.c 里数数用的；ap_prov.c 那边不知道自己是被
  *           第几次失败叫起来的，也不关心。
  */
#define WIFI_RETRY_BEFORE_AP   30

/**
  * @brief    配网回调：STA 连续失败 WIFI_RETRY_BEFORE_AP 次之后被调用
  *
  * @param[out] ssid_out   收到的热点名，保证以 '\0' 结尾
  * @param[in]  ssid_size  ssid_out 的容量
  * @param[out] pass_out   收到的密码，可以是空串
  * @param[in]  pass_size  pass_out 的容量
  *
  * @retval   true   拿到了新凭据，两个缓冲区都填好了
  * @retval   false  没拿到（比如热点没起来），两个缓冲区没动
  *
  * @warning  它是在【管理任务】里被调用的，不是在事件回调里 —— 所以可以
  *           放心阻塞，比如在那儿等一个人掏手机。等到天荒地老也没关系，
  *           整个系统就这一条任务在等，它等的也正是那件事。
  *
  * @warning  回调【必须】在返回前把 WiFi 停掉、并切回 STA 模式。本模块会
  *           马上调用 apply_credentials() 和 esp_wifi_start() 接着往下走，
  *           不会替它收拾。
  */
typedef bool (*wifi_sta_giveup_cb_t)(char *ssid_out, size_t ssid_size,
                                     char *pass_out, size_t pass_size);

/**
  * @brief    注册"连不上该找谁"的回调
  *
  * 🟢 L2 —— 工具：存下一个函数指针，什么时候用它由重试计数决定。
  *
  * @param[in] cb  回调；传 NULL 等于取消注册 —— 那就退回老行为：一直重试，
  *                只在串口上每隔 30 秒吭一声
  *
  * @note     在 wifi_sta_init() 之前还是之后注册都行：要失败 30 次才会用到它，
  *           那是几分钟以后的事，不存在"注册晚了第一次没人接"。
  *
  * @warning  只能注册一个。本工程里就 main.c 那一处调用。
  */
void wifi_sta_set_giveup_handler(wifi_sta_giveup_cb_t cb);
