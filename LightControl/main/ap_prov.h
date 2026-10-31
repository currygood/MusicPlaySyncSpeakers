/**
  * @file    ap_prov.h
  * @brief   AP 配网模式 —— 对外接口
  *
  * 这个模块只干一件事：主节点连不上的时候，让设备自己开一个热点，
  * 人在手机上连上来、在网页里把要连的 SSID 和密码填进去，模块把这两个
  * 字符串收上来交出去。
  *
  * 它【不知道】这两个字符串接下来会被拿去干什么 —— 存 NVS 也好、直接连也好，
  * 都是 wifi_sta.c 的事。反过来，本项目里也没有第二个模块需要知道"网页"这回事。
  *
  * @par 谁会在什么时候调用它
  *
  *   不是 app_main() 调的，也没有哪条命令能喊它起来 —— 是 wifi_sta.c 数到
  *   「连了 30 次都没成功」之后，通过 wifi_sta_set_giveup_handler() 注册进来的
  *   回调喊起来的，接线那一步在 main.c 里。
  *
  * @par 整段流程
  *
  *   @code
  *   ap_prov_run(ssid_out, ..., pass_out, ...)          // 阻塞，可能好几分钟
  *     |-- WiFi 切成 AP 模式，起热点 ESP-01S-Setup-XXXX
  *     |-- 起 HTTP 服务（80 端口）
  *     |-- 等人在 http://192.168.4.1/ 上提交  <----------+
  *     |-- 停 HTTP 服务、关热点                          |
  *     |-- 把收到的两个字符串拷进 ssid_out / pass_out    |
  *     +-- 返回 true                                     |
  *                                                       |
  *   wifi_sta.c 接着  nvs_save_credentials() ------------+
  *                   apply_credentials()
  *                   esp_wifi_start()
  *   @endcode
  *
  * @warning 这条路上【没有 esp_restart()】，而且是故意的：继电器挂在 GPIO0 上，
  *          "关"就是把这个脚拉低，而 GPIO0 低电平正是芯片的下载模式选择脚 ——
  *          复位那一刻它会被重新采样，采到低就直接进下载模式，程序根本不跑，
  *          串口一片安静，看着像把板子刷坏了（详见
  *          学习笔记/ESP8266开发流程.md §9.8 ①）。
  *          所以整段流程都在运行中切模式，一次也不复位。
  *
  * @warning 这条兜底路和当初被拆掉的 SmartConfig 【不是一回事】。SmartConfig
  *          走的是"手机在特殊 802.11 帧里广播凭据、设备在混杂模式下嗅探"，
  *          中间四个环节都不在我们手里，坏了也看不见。这里是设备自己开热点、
  *          手机连上来 —— 连不上就是连不上，是看得见的。
  *
  * @see     学习笔记/ESP8266开发流程.md §10（配网模式：为什么这么做、怎么用）、
  *          §9.7（被拆掉的 SmartConfig，别再往回接）
  */

#pragma once

#include <stdbool.h>
#include <stddef.h>

/**
  * @brief    进入配网模式，一直等到有人在网页上提交为止
  *
  * 🟡 L1 —— 架构：整段兜底流程（停射频→起热点→等人填表→收摊），
             阻塞几分钟；返回时 WiFi 一定停着且是 STA 模式。
  *
  * @param[out] ssid_out   收到的热点名，保证以 '\0' 结尾
  * @param[in]  ssid_size  ssid_out 的容量（含留给 '\0' 的那一个字节）
  * @param[out] pass_out   收到的密码，可以是空串（表示要连开放热点）
  * @param[in]  pass_size  pass_out 的容量
  *
  * @retval   true   收到了，两个缓冲区都填好了（长度上限由调用方把关）
  * @retval   false  热点或网页没起来 —— 两个缓冲区【没动】，里面还是原样
  *
  * @warning  本函数【阻塞很久】—— 它等的就是有个人过来掏手机，可能是几分钟，
  *           也可能一直没人来。所以只能在一条不干别的活的任务里调，
  *           没人在事件回调里等这个（事件回调一堵，整个 WiFi 状态机就停摆）。
  *
  * @warning  返回时 WiFi 一定是【停着的、STA 模式】：热点已经关了，
  *           但 STA 也还没启动。接着连的动作（apply_credentials() +
  *           esp_wifi_start()）是调用方的事 —— 成功和失败两条路都一样，
  *           差别只在用哪套凭据。这样这个模块就不必知道"凭据最后要拿去干嘛"。
  *
  * @note     返回 false 之后【不要立刻再调一遍】：起不了热点通常是因为内存
  *           不够，紧接着重试只会再失败一次。调用方回去接着数它的重试次数，
  *           下次数满了再来。
  */
bool ap_prov_run(char *ssid_out, size_t ssid_size,
                 char *pass_out, size_t pass_size);
