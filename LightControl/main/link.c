/**
  * @file    link.c
  * @brief   链路选择 —— 哪几条链路开着、回执往哪条发
  *
  * 对外接口和所有"不能这么改"的告警见 link.h。
  *
  * @see     学习笔记/ESP8266-TCP-UDP-WiFi-STA.md §5.5
  */

#include <stdio.h>

#include "tcp_client.h"
#include "udp_client.h"
#include "mqtt_link.h"

#include "link.h"

/**
  * @brief 主链路 —— 跟 ESP32 说话的那条
  *
  * @note  只可能是 LINK_TCP 或 LINK_UDP，两条【不会同时开】。
  *        想改它只有两条路：link_init() 定初值，link_switch() 运行期换。
  */
static link_mode_t s_master = LINK_TCP;

/**
  * @brief MQTT 那一路开着没有
  *
  * @note  它和主链路【互不相干】，两边可以同时开着 —— 这正是加这一路的目的：
  *        ESP32 那边走 TCP/UDP，云那边走 broker，各说各的。
  */
static bool s_mqtt_on = false;

/**
  * @brief    链路的名字，只用来拼回执和日志
  *
  * 🟢 L2 —— 工具：枚举转字符串，纯查表。
  *
  * @param[in] mode  链路
  *
  * @return   大写的名字
  *
  * @note     回执的【格式】和只有两条链路时完全一样（"NET -> UDP\r\n"），
  *           只是拼出来的。
  */
static const char *link_name(link_mode_t mode)
{
    switch (mode) {
    case LINK_UDP:  return "UDP";
    case LINK_MQTT: return "MQTT";
    default:        return "TCP";
    }
}

void link_init(link_mode_t master)
{
    switch (master) {
    case LINK_UDP:
        s_master = LINK_UDP;
        udp_client_start();
        break;
    default:
        s_master = LINK_TCP;
        tcp_client_start();
        break;
    }

    /* MQTT 那一路默认开。
       ⚠ 本函数【不替调用方等 WiFi】：broker 是域名，没拿到 IP 就没有 DNS，
         所以调用它之前网络必须已经就绪 —— main.c 里是在
         wifi_sta_wait_ip() 返回之后才走到这儿的。真不等也不会崩，esp-mqtt
         会自己按 10 秒的间隔一直重试，只是白转。 */
    s_mqtt_on = true;
    mqtt_link_start();
}

bool link_is_open(link_mode_t mode)
{
    if (mode == LINK_MQTT) {
        return s_mqtt_on;
    }
    return (mode == s_master);
}

link_mode_t link_master(void)
{
    return s_master;
}

int link_send(link_mode_t to, const char *data, int len)
{
    switch (to) {
    case LINK_UDP:  return udp_client_send(data, len);
    case LINK_MQTT: return mqtt_link_send(data, len);
    default:        return tcp_client_send(data, len);
    }
}

void link_switch(link_mode_t reply_to, link_mode_t mode)
{
    /* 回执拼在栈上。24 字节够装 "NET already UDP\r\n"（17 字节）。 */
    char reply[24];

    /* MQTT 不是主链路，走到这儿说明命令表配错了，直接不理。 */
    if (mode != LINK_TCP && mode != LINK_UDP) {
        return;
    }

    if (s_master == mode) {
        snprintf(reply, sizeof(reply), "NET already %s\r\n", link_name(mode));
        link_send(reply_to, reply, 0);
        return;
    }

    /* ① 先回执 —— 发回命令来的那条路。此刻它一定还开着（命令刚从它那儿来），
          而它马上就要被关掉了，所以这一步必须在前。 */
    snprintf(reply, sizeof(reply), "NET -> %s\r\n", link_name(mode));
    link_send(reply_to, reply, 0);

    /* ② 再换向，关掉旧主链路、开新的。

       注意关哪条、开哪条是【写死】的，不是"除了 mode 之外的全关"——
       MQTT 那条不在主链路之列，不该被顺手关掉。 */
    s_master = mode;

    if (mode == LINK_UDP) {
        tcp_client_stop();
        udp_client_start();
    } else {
        udp_client_stop();
        tcp_client_start();
    }
}

void link_set_mqtt(link_mode_t reply_to, bool on)
{
    if (s_mqtt_on == on) {
        link_send(reply_to, on ? "MQTT ALREADY ON\r\n" : "MQTT ALREADY OFF\r\n", 0);
        return;
    }

    /* ① 先回执。关的时候尤其要紧：等会儿这条路就发不出去了。 */
    link_send(reply_to, on ? "MQTT ON\r\n" : "MQTT OFF\r\n", 0);

    /* ② 再改标志、再动手。标志先改，这样关掉是【立刻】生效的 ——
          mqtt_link_stop() 只是让模块不再收发（连接和那条任务都还在），
          真正把数据挡在外面的是 link_is_open() 那道判断。 */
    s_mqtt_on = on;

    if (on) {
        mqtt_link_start();
    } else {
        mqtt_link_stop();
    }
}
