/**
  * @file    main.c
  * @brief   把各模块串起来并启动
  *
  * 这个文件【只做粘合剂和启动器】，不含任何业务逻辑 —— 继电器怎么动、
  * 命令怎么解析、走 TCP 还是 UDP，全在下面这些模块里：
  *
  *   relay.c        继电器（GPIO0、上电为关、strapping 脚）
  *   link.c         哪几条链路开着 + 所有回执的出口
  *   cmd.c          收字节 → 拼命令 → 调 relay / link / wifi_sta
  *   wifi_sta.c     连热点、凭据存 NVS、断线重连
  *   ap_prov.c      连不上时的兜底：开热点 + 网页，让人用手机填新凭据
  *   tcp_client.c   连服务端、收发字节（纯传输，不知道"开灯"是什么）
  *   udp_client.c   不建连接、直接丢数据报（同样纯传输）
  *   mqtt_link.c    连巴法云 broker、订阅主题（同样纯传输，只是对端是 broker）
  *
  * @par 要改什么，去哪儿
  *
  *   - 服务端 IP / 端口   → tcp_client.c 和 udp_client.c 的顶部【两个文件都要改】
  *   - broker / 主题 / ID → mqtt_link.c 顶部那几行 #define（用户名密码也在那儿）
  *   - 热点名 / 密码      → 优先发命令 wifi \<SSID\>,\<密码\>；改代码只能改默认值
  *   - 能识别的命令        → cmd.c 的 s_cmds 表
  *   - 继电器接哪个脚      → relay.c 顶部的 RELAY_GPIO
  *   - 开机走哪条主链路    → 本文件 app_main() 里的 link_init()
  *   - 开机 MQTT 开不开    → link.c 的 link_init()（默认开）
  *   - 配网热点的名字/密码 → ap_prov.c 顶部的 AP_SSID_PREFIX / AP_PASSWORD
  *   - 失败几次转配网      → wifi_sta.h 里的 WIFI_RETRY_BEFORE_AP
  *
  * @warning 改主节点凭据的顺序仍然是：① 先发命令改从节点（它此时还连得上）
  *          ② 再改主节点。反过来做的话，从节点会掉进配网模式 —— 不致命，
  *          但要跑过去拿手机连它的热点重填一遍。
  *
  * @warning 配网兜底不等于"什么都能救"：只有当【凭据填错了】它才有用。
  *          热点开在 5GHz、板子太远、主节点根本没开机，这些它一样救不了。
  *          所以"先改从节点、再改主节点"这条纪律没有被省掉。
  *
  * @warning "改配置"和"换固件"是两件事：改 WiFi 密码靠上面那条命令，
  *          换固件只能插串口线（本板 1MB，两个 app 槽放不下，所以没有 OTA）。
  *
  * @see     学习笔记/ESP8266开发流程.md §9.1（模块分工）、§9.7（连不上怎么查）、§9.9（为什么没有 OTA）、
  *          学习笔记/ESP8266-TCP-UDP-WiFi-STA.md §5（整体架构）
  *
  * This example code is in the Public Domain (or CC0 licensed, at your option.)
  */

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_system.h"
#include "esp_spi_flash.h"

#include "wifi_sta.h"
#include "ap_prov.h"
#include "tcp_client.h"
#include "udp_client.h"
#include "mqtt_link.h"
#include "relay.h"
#include "link.h"
#include "cmd.h"

/**
  * @brief 启动时把芯片信息打到串口上
  *
  * 🟢 L2 —— 工具：开机打几行芯片信息。
  *
  * @note  spi_flash_get_chip_size() 读的是【编译时写进 sdkconfig 的值】，
  *        不是探测出来的 —— 所以这行打印不能用来判断板上真实有多少 flash。
  *        要确认容量，用 esptool.py flash_id（见 学习笔记/ESP8266开发流程.md）。
  */
static void print_chip_info(void)
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    printf("This is ESP8266 chip with %d CPU cores, WiFi, ", chip_info.cores);
    printf("silicon revision %d, ", chip_info.revision);
    printf("%dMB %s flash\n", spi_flash_get_chip_size() / (1024 * 1024),
            (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");
}

/**
  * @brief 程序入口
  *
  * 🟡 L1 —— 架构：全工程的开机顺序就是这几行 —— 先 relay_init()
             （从此刻起继电器才可控），再 WiFi，再注册回调和兜底，
             等 IP，最后 link_init()。
  *
  * @note  下面五步的【顺序不能动】，每一步的原因都写在那一行上面。
  */
void app_main(void)
{
    print_chip_info();

    /* ① 硬件先就位：把 GPIO0 配成输出并置成"关"。 */
    relay_init();

    /* ② 联网：连热点、断线重连、打印 IP。异步的，返回时还没连上。

          紧跟着的这一句是配网兜底：连 30 次都没成，就由 ap_prov.c 顶上来，
          把自己变成一个热点，等人用手机连上来填新凭据。注册得早晚无所谓 ——
          真要用到它是几分钟以后的事，不存在"注册晚了第一次没人接"。 */
    wifi_sta_init();
    wifi_sta_set_giveup_handler(ap_prov_run);

    /* ③ 注册回调 —— 必须在 link_init() 之前：后者会立刻把链路启动起来，
          注册晚了第一段到达的数据会因为回调还是 NULL 而被悄悄丢掉。

          三个都注册：现在最多【两条同时开着】（主链路 + MQTT），每条都得
          有自己的入口。 */
    tcp_client_set_rx_handler(cmd_on_tcp_rx);
    udp_client_set_rx_handler(cmd_on_udp_rx);
    mqtt_link_set_rx_handler(cmd_on_mqtt_rx);

    /* ④ 等 WiFi 真的连上（拿到 IP）再往下走 —— 链路要等这一步。

          ⚠ 这一步主要是给 MQTT 那一路等的：它的 broker 是【域名】
            （mqtt.bemfa.com），没有 IP 就没有 DNS，启起来只能白转。
            主链路 tcp/udp 不受影响 —— 它们本来就在各自的任务里等在同一
            个位置上（建 socket 前先等 IP），多这一等等于没等。

          ⚠ 本函数会【阻塞】在这里，通常一两秒。热点一直连不上就一直等下
            去 —— 那正是我们要的：等 ap_prov.c 那边拿到新凭据、WiFi 真起来
            了，这里再继续。（ap_prov 跑在 wifi_sta 自己的管理任务上，不受
            这里阻塞的影响。）
            wifi_sta_wait_ip() 不清事件位，所以不影响别的任务同时在等。 */
    wifi_sta_wait_ip(UINT32_MAX);

    /* ⑤ 定主链路并启动它，MQTT 那一路也会一起开起来。

          主链路开机默认走 UDP —— 本节点现在是个【灯控接收端】，主控通过
          UDP 组播（239.0.0.2:8889）下发 JSON 开关命令，节点也要往这个组里
          回 status / verr，所以 UDP 必须起来。旧的那套文本关键字命令
          （on/off/net tcp/...）在 UDP 下照样能收（cmd.c 的缓冲区不分链路），
          所以切到 UDP 不会丢掉向后兼容；想换回 TCP 发一条 "net tcp" 即可。

          MQTT 还有个门槛：broker 是域名，热点得会下发 DNS —— ESP32 自己
          开的 SoftAP 没有 DNS，那条路走不通，得用手机热点或路由器。
          所以在"只有 ESP32、没有外网"的现场，串口上会看到它一直在重连；
          不想要就发一条 mqtt off 让它安静。 */
    link_init(LINK_UDP);

    /* ⑥ 启动 30 秒周期状态上报（灯控 JSON 协议要求）。必须在 link_init()
          之后：上报任务会先等 WiFi 拿到 IP，并经由 UDP 组播发往组地址。 */
    cmd_start_status_reporter();

    /* ⑦ 主任务保持存活。真正的活儿都在上面那几条任务里异步跑着。

          ⚠ 这里本身【不要】返回、也不要主动重启：一旦复位，GPIO0 会被重新
            采样成启动模式选择脚，此时若正好处在低电平，芯片就会进下载模式。
            本工程现在没有任何地方会重启，所以这条约束是"别引入"而不是"要处理"——
            将来真要加重启，记得先 relay_on()。 */
    while (1) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}
