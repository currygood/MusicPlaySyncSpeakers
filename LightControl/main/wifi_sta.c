/**
  * @file    wifi_sta.c
  * @brief   WiFi STA（客户端）连接模块 —— 凭据读写、断线重连、状态上报
  *
  * 对外接口和调用顺序见 wifi_sta.h。
  *
  * @warning 改主节点凭据的顺序仍然是：① 先发命令改从节点 ② 再改主节点。
  *          配网兜底（下面那条）是用来救这个顺序写反了的，不是用来省掉它的。
  *
  * 连不上时的兜底在本文件里只有【一个判断和一次调用】，真正开热点、起网页的事
  * 全在 ap_prov.c 里 —— 走的是 wifi_sta_set_giveup_handler() 注册进来的回调，
  * 接线那一步在 main.c。本文件因此既不知道热点名，也不知道有网页这回事。
  *
  * @see     学习笔记/ESP8266-NVS.md（凭据为什么放 NVS、怎么擦除）、
  *          学习笔记/ESP8266-TCP-UDP-WiFi-STA.md §4（STA 的 API 与事件）、
  *          学习笔记/ESP8266开发流程.md §9.6（换热点的操作纪律）、§9.7（连不上怎么查）、
  *          §10（连不上的兜底：AP 配网模式）
  */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_event.h"
#include "esp_wifi.h"
#include "tcpip_adapter.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "wifi_sta.h"

/* ======================= 出厂默认凭据 ======================= */

/**
  * @brief    出厂默认热点名
  *
  * @note     这是三个凭据来源里【最低的那一层】：NVS > 这里 > 内置默认。
  *           第一次上电时 NVS 是空的，就用这里，并顺手写进 NVS；
  *           之后不管用什么方式改过凭据，都以 NVS 为准，这一行不再生效。
  *
  * @warning  SSID 区分大小写，而且【空格也算一个字符】—— 写错不会报错，
  *           只会一直连不上（串口上会看到 reason=201）。
  *
  * @warning  ESP8266 只支持 2.4GHz。热点开在 5GHz 上芯片根本扫不到，
  *           表现和"SSID 写错"一模一样。
  *
  * @warning  这个值必须和主节点那边【逐字一致】。两边是各自独立编译的，
  *           编译器不会帮你核对（见 学习笔记/ESP8266开发流程.md §9.2 的四值表）。
  */
#define WIFI_SSID_DEFAULT       "ESP32-S3-host"

/**
  * @brief    出厂默认密码
  *
  * @warning  不能少于 8 字节 —— WPA2 的下限，短了对方拒绝开 AP。
  *
  * @note     主节点的加密方式必须选 WPA2-PSK：本文件里 threshold.authmode
  *           设的就是它（见 apply_credentials()），主节点开成【开放式】
  *           热点反而连不上。
  */
#define WIFI_PASSWORD_DEFAULT   "88888888"

/* ======================= NVS ======================= */

/**
  * @brief    NVS 命名空间
  *
  * @note     用 "app_cfg" 是为了避开 WiFi 驱动自己用的那些 ——
  *           驱动把 PHY 校准数据也存在这里，两者靠着命名空间互不干扰。
  */
#define NVS_NAMESPACE   "app_cfg"

/** @brief  NVS 里存热点名的键 */
#define NVS_KEY_SSID    "ssid"

/** @brief  NVS 里存密码的键 */
#define NVS_KEY_PASS    "pass"

/** @brief 802.11 协议规定的 SSID 上限（字节，不含结尾 '\0'） */
#define SSID_MAX_LEN    32

/** @brief 802.11 协议规定的密码上限（字节，不含结尾 '\0'） */
#define PASS_MAX_LEN    64

/* ======================= 重连参数 ======================= */

/**
  * @brief    没连上时，每隔多久在串口上吭一声（毫秒）
  *
  * @note     重连本身【不靠这个值】—— 收到"断开"事件就直接重连了，不需要定时器。
  *           这个值只控制报平安的频率：不然连不上时日志一片安静，看起来像死机。
  *
  * @note     管理任务也是按这个间隔去数"失败够不够 30 次了"，所以改大它会让
  *           进配网模式更迟一点 —— 但不会漏掉，判断用的是 >=。
  */
#define WIFI_DOWN_REPORT_MS   30000

/* ======================= 模块内部状态 ======================= */

/** @brief WiFi 事件组。用来在"事件回调"和"等待的任务"之间传递状态。 */
static EventGroupHandle_t s_wifi_event_group;

/** @brief 事件位：拿到 IP 了（真的能通信了） */
#define WIFI_GOT_IP_BIT   BIT0

/** @brief 事件位：掉线过一次（用来把管理任务从"已连上"的睡眠里叫醒） */
#define WIFI_DOWN_BIT     BIT1

/**
  * @brief 当前生效的热点名
  *
  * @note  放在这里而不是每次去读 NVS，是因为事件回调里要打印它，
  *        而 NVS 读操作不该在回调里频繁做。
  *
  * @note  比协议上限多一个字节，留给结尾的 '\0'。
  */
static char s_ssid[SSID_MAX_LEN + 1];

/** @brief 当前生效的密码。容量说明同 s_ssid。 */
static char s_pass[PASS_MAX_LEN + 1];

/**
  * @brief 连接重试计数
  *
  * @note  除了在串口上区分"第几次失败"，管理任务还会把它打出来 ——
  *        数字一直涨就是真的在原地打转，该去查 SSID / 密码 / 频段了。
  */
static int s_retry = 0;

/**
  * @brief 连不上时该找谁 —— 由 wifi_sta_set_giveup_handler() 注册
  *
  * @note  默认是 NULL，也就是"没人管"：那就一直重试、只报平安，退回加这个
  *        功能之前的行为。本工程里 main.c 会把它接上 ap_prov.c。
  */
static wifi_sta_giveup_cb_t s_giveup_handler = NULL;

/* ======================= 小工具 ======================= */

/**
  * @brief    把 src 拷进定长缓冲区，超长就截断，并保证以 '\0' 结尾
  *
  * 🟢 L2 —— 工具：定长拷贝，超长截断并补 \0。
  *
  * @param[out] dst       目标缓冲区
  * @param[in]  dst_size  目标缓冲区的总容量（含留给 '\0' 的那一个字节）
  * @param[in]  src       源字符串
  *
  * @warning  不要"简化"回 strncpy()：源串长度 >= 目标缓冲区时它【不补 '\0'】，
  *           留下一个没有结尾的字符串，后面 strlen/printf 会一路读下去越界。
  *
  * @see      学习笔记/ESP8266-TCP-UDP-WiFi-STA.md §6.1
  */
static void copy_str(char *dst, size_t dst_size, const char *src)
{
    size_t n = strlen(src);
    if (n > dst_size - 1) {
        n = dst_size - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* ======================= NVS 读写 ======================= */

/**
  * @brief    从 NVS 读凭据到 s_ssid / s_pass
  *
  * 🟡 L1 —— 架构：从 NVS 读凭据；读不到（第一次上电）就退回编译进去
             的默认值 —— 所以第一次开机不配网也能连上。
  *
  * @retval   true   读到了（两个键都在，而且 SSID 不是空串）
  * @retval   false  没读到，或者读到的 SSID 是空串
  *
  * @note     读出来的值【直接写进模块自己的】s_ssid / s_pass，不从参数返回 ——
  *           这两个缓冲区是 wifi_sta.c 的私事。
  *
  * @warning  失败时【不会清空缓冲区】。所以调用方在 false 分支里必须把
  *           【两个】缓冲区都改成默认值 —— 否则会拿"SSID 是新的、密码是旧的"
  *           这种半新半旧的凭据去连，然后在串口上对着一个正确的 SSID 怀疑人生。
  *
  * @note     别把这里的 false 一律读成"没配过"：忘了 nvs_flash_init() 时
  *           也走同一条 return false，表现和"NVS 是空的"一模一样。
  *
  * @see      学习笔记/ESP8266-NVS.md §3.4（length 语义、out_value 不被修改）
  */
static bool nvs_load_credentials(void)
{
    nvs_handle_t h;
    size_t       len;
    esp_err_t    err;

    /* 以只读方式打开 —— 这一步顺便就是"这个节点配过没有"的判据：
       命名空间不存在时，NVS_READONLY 直接返回 ESP_ERR_NVS_NOT_FOUND，
       第一次上电正是这种情况，所以 return false 不是错误，串口上不用打东西。

       不要改成 NVS_READWRITE：那样 open 会顺手把命名空间建出来并写进 flash，
       一个纯读的函数不该有写副作用，而且那会把"命名空间在不在"这个信息抹掉。 */
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }

    /* nvs_get_str 的 length 是【传进缓冲区大小、传出实际长度】，
       所以每次调用前都要重新赋 sizeof，不能把上一次写回来的值再用一遍。 */
    len = sizeof(s_ssid);
    err = nvs_get_str(h, NVS_KEY_SSID, s_ssid, &len);

    if (err == ESP_OK) {
        /* 三步共用一个 err，末尾那个是【最后执行到的那一步】的结果：
           SSID 这一步没成功，密码那一步根本不会执行。 */
        len = sizeof(s_pass);
        err = nvs_get_str(h, NVS_KEY_PASS, s_pass, &len);
    }

    /* 两个键要么都读，要么都不算数 —— 所以不管成败都得关句柄，
       不关的话每读一次凭据就漏一个。 */
    nvs_close(h);

    /* s_ssid[0] != '\0' 是第二道，防御性的 —— 正常路径到不了这儿，因为两个
       写入口都已经拒收空 SSID 了。但 NVS 里的值如果不是这版固件写的，
       空串会一路走到 esp_wifi_set_config()，然后永远连不上，
       而且 reason 还是 201，跟"SSID 拼错"长得一模一样。 */
    return (err == ESP_OK && s_ssid[0] != '\0');
}

/**
  * @brief    把凭据写进 NVS
  *
  * 🟡 L1 —— 架构：写 NVS 再 commit；断电之后凭据还在不在，就看这一步。
  *
  * @param[in] ssid      热点名
  * @param[in] password  密码
  *
  * @retval   true   写入成功（已落盘）
  * @retval   false  打开 NVS 或写入失败，串口上会打原因
  *
  * @note     这里【只写 NVS】，不碰当前连接 —— 所以 wifi_sta_init() 里
  *           写默认值的时候也能安全调用。改连接是 wifi_sta_set_credentials() 的事。
  *
  * @see      学习笔记/ESP8266-NVS.md §3.3（nvs_commit 在本 SDK 上是空操作）
  */
static bool nvs_save_credentials(const char *ssid, const char *password)
{
    nvs_handle_t h;
    esp_err_t    err;

    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        printf("[wifi] ✗ NVS 打不开（分区表里有 nvs 分区吗？）\n");
        return false;
    }

    err = nvs_set_str(h, NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, NVS_KEY_PASS, password);
    }
    if (err == ESP_OK) {
        /* 这一句在本 SDK（v3.4）上是空操作 —— set 已经当场落盘了。
           留着是因为 ESP-IDF v4+ 的 NVS 真的加了缓存，到那边漏掉它就是
           "掉电丢失，而且当场看不出任何异常"。 */
        err = nvs_commit(h);
    }

    nvs_close(h);

    if (err != ESP_OK) {
        printf("[wifi] ✗ NVS 写入失败: %d\n", err);
        return false;
    }
    return true;
}

/* ======================= 应用凭据 ======================= */

/**
  * @brief    把 s_ssid / s_pass 灌进 WiFi 驱动
  *
  * 🟡 L1 —— 架构：把 s_ssid/s_pass 灌进 wifi_config_t 下发。只发配置
             不发起连接 —— threshold.authmode 那行也在这里，它就是从机
             连不上开放式热点的原因。
  *
  * @note     只改配置，不负责连接。真正发起连接的是 esp_wifi_connect()。
  *
  * @warning  threshold.authmode 是加密方式的【下限】，不是"要求"。
  *           设成 WPA2_PSK 就是"比 WPA2 弱的（WEP、WPA-TKIP）我都不连"，
  *           所以主节点开成【开放式】热点反而连不上。
  *           热点是 WPA/WPA2 混合模式或 WPA3 时，把这一行去掉再试。
  *
  * @see      学习笔记/ESP8266-TCP-UDP-WiFi-STA.md §4.1（wifi_config_t 的三个讲究）
  */
static void apply_credentials(void)
{
    wifi_config_t cfg;

    /* 整块清零：结构体里还有信道、BSSID、各种阈值，
       不清零的话它们就是栈上的随机值。 */
    memset(&cfg, 0, sizeof(cfg));

    /* 这里是运行期赋值，不能用初始化器写法：左右两边类型对不上
       （uint8_t[32] 和 char*），必须老老实实拷贝。 */
    copy_str((char *)cfg.sta.ssid,     sizeof(cfg.sta.ssid),     s_ssid);
    copy_str((char *)cfg.sta.password, sizeof(cfg.sta.password), s_pass);

    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_set_config(ESP_IF_WIFI_STA, &cfg);
}

/* ======================= 事件回调 ======================= */

/**
  * @brief    WiFi / IP 事件回调
  *
  * 🟡 L1 —— 架构：STA 启动/断开/拿到 IP 三个分支；重试计数和给别的任务
             等的事件位都在这里改。
  *
  * @param[in] arg         注册时传进来的参数，本模块没用（注册时传的 NULL）
  * @param[in] event_base  事件大类：WIFI_EVENT 或 IP_EVENT
  * @param[in] event_id    具体是哪个事件
  * @param[in] event_data  事件携带的数据，类型由 event_id 决定，要自己转
  *
  * @warning  它由 esp_event 的默认事件循环任务（"sys_evt"，优先级 10）调用，
  *           【不在调用者的任务里跑】。所以这里绝对不能做阻塞操作
  *           （比如 vTaskDelay），否则整个 WiFi 状态机都会停摆。
  *
  * @note     事件分两大类，靠 event_base 区分：
  *             WIFI_EVENT —— WiFi 状态变化（启动、关联上 AP、断开……）
  *             IP_EVENT   —— 网络层事件（拿到 IP……）
  *
  * @see      学习笔记/ESP8266-TCP-UDP-WiFi-STA.md §4.2、§4.5（reason 码对照表）
  */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        /* esp_wifi_start() 只是把驱动拉起来，并不会去连 AP。
           真正的连接动作要在这里发起。 */
        printf("[wifi] STA 已启动，开始连接 \"%s\" ...\n", s_ssid);
        esp_wifi_connect();

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        /* 连不上、被踢掉、掉线，都走这里。
           reason 是 802.11 的断开原因码，最常撞见的两个：
             201     —— 找不到这个 SSID（名字写错，或者热点开在 5GHz 上）
             15 / 202 / 204 —— 密码不对 */
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)event_data;

        xEventGroupClearBits(s_wifi_event_group, WIFI_GOT_IP_BIT);
        xEventGroupSetBits(s_wifi_event_group, WIFI_DOWN_BIT);

        printf("[wifi] 第 %d 次失败，reason=%d\n", ++s_retry, d->reason);

        /* 直接重连，不在这里 sleep：esp_wifi_connect() 内部要先扫一遍信道、
           再等认证超时，本身就要好几秒。反过来，在事件回调里 vTaskDelay
           会把整个默认事件循环卡住，得不偿失。 */
        esp_wifi_connect();

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        /* 这才是"真的连上了"的标志：DHCP 拿到了 IP 地址。
           WIFI_EVENT_STA_CONNECTED 只是关联上了 AP，那时还没有 IP。 */
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)event_data;
        s_retry = 0;

        /* ip4addr_ntoa() 返回的是指向【静态缓冲区】的指针，连着调用三次会
           互相覆盖 —— 所以下面一行只放一次调用。 */
        printf("\n========== WiFi 连接成功 ==========\n");
        printf("  SSID     : %s\n", s_ssid);
        printf("  IP       : %s\n", ip4addr_ntoa(&e->ip_info.ip));
        printf("  子网掩码 : %s\n", ip4addr_ntoa(&e->ip_info.netmask));
        printf("  网关     : %s\n", ip4addr_ntoa(&e->ip_info.gw));
        printf("===================================\n\n");

        xEventGroupSetBits(s_wifi_event_group, WIFI_GOT_IP_BIT);
    }
}

/* ======================= 管理任务 ======================= */

/**
  * @brief    从配网模式回来：存下新凭据、把 WiFi 拉回 STA、开始连
  *
  * 🟡 L1 —— 架构：配网回来这一趟 —— 先存新凭据（存失败就放弃），再切回
             STA、重下配置、起射频，最后交给事件回调去连。
  *
  * @param[in] ssid      用户在网页上填的热点名
  * @param[in] password  同上，密码
  *
  * @note     回到 STA 的顺序是照抄 wifi_sta_init() 的：声明模式 → 灌配置 →
  *           启动。只多了一句 esp_wifi_set_mode()，因为 ap_prov_run() 返回时
  *           WiFi 是【停着的】、而且模式还是 AP —— 那正是它交班时的约定
  *           （见 ap_prov.h）。
  *
  * @warning 这里【不能】图省事改用 wifi_sta_set_credentials()：那个函数结尾要
  *          esp_wifi_disconnect()，可此刻 WiFi 根本没启动 —— disconnect 会失败、
  *          也不会产生任何事件，它补刀的那句 esp_wifi_connect() 同样失败。
  *          两次调用都白打，而且串口上什么都不说。它前半截存 NVS 的事这里照做。
  *
  * @warning 存不进 NVS 就【什么都别做】：这时候切过去连，下次上电又变回旧凭据，
  *          变成"这次好使、重启就不好使"这种最难查的毛病。
  *
  * @warning 这里【不用】ESP_ERROR_CHECK：它失败时会 abort，而 abort 会复位芯片。
  *          这条路上我们一次都不想复位 —— 继电器正把 GPIO0 拉低，那正是芯片的
  *          下载模式选择脚。宁可什么都不做，等下一次命令或者重新上电。
  */
static void sta_restart_from_prov(const char *ssid, const char *password)
{
    if (!nvs_save_credentials(ssid, password)) {
        printf("[wifi] ✗ 新凭据没存住，维持原样\n");
        return;
    }

    copy_str(s_ssid, sizeof(s_ssid), ssid);
    copy_str(s_pass, sizeof(s_pass), password);

    esp_wifi_set_mode(WIFI_MODE_STA);
    apply_credentials();

    if (esp_wifi_start() != ESP_OK) {
        printf("[wifi] ✗ STA 拉不起来，只能重新上电了\n");
        return;
    }

    printf("[wifi] 配网结束，改用 \"%s\" 重新连\n", s_ssid);
}

/**
  * @brief    状态上报任务
  *
  * 🟡 L1 —— 架构：没连上就定时上报，失败满 WIFI_RETRY_BEFORE_AP 次就
             叫 ap_prov_run()。等 IP 时只能用 pdFALSE —— 清了事件位，
             wifi_sta_wait_ip() 就永远等不到。
  *
  * @param[in] arg  任务参数，本模块没用
  *
  * @note     这条任务干两件事：没连上的时候每隔 WIFI_DOWN_REPORT_MS 在串口上
  *           吭一声；失败够 WIFI_RETRY_BEFORE_AP 次了，就叫配网回调来接手。
  *           它从不退出。
  *
  * @note     为什么要单独一条任务：因为"等一段时间"本质上是阻塞的，
  *           而事件回调跑在系统的事件循环任务上 —— 在那里阻塞，
  *           整个 WiFi 状态机就停摆了。配网那一段要在网页那头等人掏手机，
  *           更是只能在这条任务里等。
  *
  * @warning  重连本身【不在这里】：收到"断开"事件就直接 esp_wifi_connect() 了。
  *           所以这条任务挂掉不影响重连，只会让日志变哑。
  */
static void wifi_mgr_task(void *arg)
{
    (void)arg;

    for (;;) {
        /* 已经连上 —— 睡到掉线为止。
           portMAX_DELAY = 无限等，不占 CPU。 */
        if (xEventGroupGetBits(s_wifi_event_group) & WIFI_GOT_IP_BIT) {
            xEventGroupWaitBits(s_wifi_event_group, WIFI_DOWN_BIT,
                                pdTRUE,    /* 等到了就清掉，下次还能再等 */
                                pdTRUE,    /* 指定的位全到齐 */
                                portMAX_DELAY);
            continue;   /* 掉线了，回到循环开头重新判断 */
        }

        /* 没连上 —— 等一段。这里必须 pdFALSE：不能清除 GOT_IP 位，
           否则 wifi_sta_wait_ip() 那边永远等不到。 */
        if (xEventGroupWaitBits(s_wifi_event_group, WIFI_GOT_IP_BIT,
                                pdFALSE, pdTRUE,
                                pdMS_TO_TICKS(WIFI_DOWN_REPORT_MS))
            & WIFI_GOT_IP_BIT) {
            continue;   /* 连上了 */
        }

        /* 数够了就转配网模式。
           没注册回调（s_giveup_handler 是 NULL）就往下走 —— 那就成了"没这个
           功能"，行为和加它之前一模一样。 */
        if (s_retry >= WIFI_RETRY_BEFORE_AP && s_giveup_handler != NULL) {
            char ssid[SSID_MAX_LEN + 1];
            char pass[PASS_MAX_LEN + 1];

            printf("\n[wifi] 已经失败 %d 次了，转配网模式\n", s_retry);

            /* 回调里会开热点、起网页，然后在那头等人来填 —— 一直阻塞到有人
               提交为止。这是本任务唯一一次长时间不回来，也正是设计如此：
               它等的那件事，本来就是"有个人拿着手机过来"。 */
            if (s_giveup_handler(ssid, sizeof(ssid), pass, sizeof(pass))) {
                sta_restart_from_prov(ssid, pass);
            }

            /* 不管成没成，计数都从头来 —— 否则下一圈立刻又会判定"数够了"，
               变成反复开热点关热点。配网失败时这一步顺带就是一次退避：
               回到 STA 再试 30 次，几分钟后才轮到下一次开热点。 */
            s_retry = 0;
            continue;
        }

        printf("[wifi] 还没连上 \"%s\"：已失败 %d 次，仍在重试\n", s_ssid, s_retry);
        printf("[wifi]   查这三样：SSID 拼写（空格和大小写都算）、密码、\n");
        printf("[wifi]   热点是不是只在 5GHz 上（ESP8266 只认 2.4GHz）\n");
    }
}

/* ======================= 对外接口 ======================= */
/* 接口契约（参数范围、返回值、调用顺序）都在 wifi_sta.h 里，这里只记
   实现顺序上不能动的地方。 */

void wifi_sta_init(void)
{
    s_wifi_event_group = xEventGroupCreate();

    /* WiFi 驱动要把 PHY 校准数据之类的存进 NVS，不初始化会直接启动失败。
       第一次上电时 NVS 分区是空的，驱动会自己把需要的页初始化好。
       如果分区表动过、或者上一版固件用的是另一种 NVS 格式，这两个错误会冒出来，
       标准做法是擦掉整个 NVS 分区再重来一次。

       ⚠ nvs_flash_erase() 会把【我们的凭据也一起擦掉】——
         所以这条路径走完，节点会退回代码里的默认凭据。
         这其实正好是一个"恢复出厂设置"的办法。 */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        printf("[wifi] NVS 需要重建 (%d)，正在擦除...\n", ret);
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 凭据：NVS 优先，没有就用编译进去的默认值。
       注意 false 分支必须把两个缓冲区都覆盖掉 —— 理由见 nvs_load_credentials()。 */
    if (nvs_load_credentials()) {
        printf("[wifi] 凭据来自 NVS\n");
    } else {
        copy_str(s_ssid, sizeof(s_ssid), WIFI_SSID_DEFAULT);
        copy_str(s_pass, sizeof(s_pass), WIFI_PASSWORD_DEFAULT);
        printf("[wifi] NVS 里没有凭据，用出厂默认值，并写入 NVS\n");
        nvs_save_credentials(s_ssid, s_pass);
    }

    /* 这两行是所有网络功能的地基：
         tcpip_adapter_init()             —— 把 lwIP 协议栈拉起来
         esp_event_loop_create_default()  —— 起默认事件循环，"连上了 / 断开了"
                                             就是靠它把事件派发给上面的回调 */
    tcpip_adapter_init();
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* 显式关掉省电模式。

       ⚠ 在本 SDK 上这一句是【冗余的】，不是任何问题的修复：
         esp_wifi.h:413 写着 "Default power save type is WIFI_PS_NONE"。

         留着是为了把前提显式写死，而且将来会真的有用：SDK 自带的 ESP-NOW
         示例（examples/wifi/espnow/README.md）里明确写了，接收方是 station
         模式且连着一个 AP 时，必须关掉 modem sleep。
         哪天从节点改用 ESP-NOW，省电开着就会收不到包。 */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    /* 注册回调。IP 那一侧只关心"拿到 IP"这一个事件，其它不用管。

       ⚠ 必须在 esp_wifi_start() 【之前】注册：start 会触发
         WIFI_EVENT_STA_START，而连接动作是在那个事件的处理函数里发起的，
         注册晚了就没人接。 */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));

    /* 顺序不能动：先声明模式，再灌配置，最后启动。 */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    apply_credentials();
    ESP_ERROR_CHECK(esp_wifi_start());

    /* 起管理任务。栈给到 6KB —— 比"只做 printf"那会儿大了一半：它现在还要
       跑配网那一段（切模式、起热点、起 HTTP 服务），那些函数各有各的调用深度，
       而且要在这一条栈上摆开一个 wifi_config_t 和一个 httpd_config_t。
       栈给少了的表现是随机崩，而且常常崩在别的任务里，非常难查。 */
    if (xTaskCreate(wifi_mgr_task, "wifi_mgr", 6144, NULL, 4, NULL) != pdPASS) {
        printf("[wifi] ✗ 管理任务创建失败（只影响「还没连上」那几行提示）\n");
    }

    /* esp_wifi_start() 是异步的：它返回时还没连上，连接结果由事件循环
       在后台通过 wifi_event_handler() 打印出来。 */
}

bool wifi_sta_wait_ip(uint32_t timeout_ms)
{
    /* pdMS_TO_TICKS(UINT32_MAX) 会被除成一个有限的 tick 数，
       所以"无限等待"要单独映射到 portMAX_DELAY。 */
    TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY
                                                  : pdMS_TO_TICKS(timeout_ms);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_GOT_IP_BIT,
                                           pdFALSE,   /* 不清除这个 bit */
                                           pdTRUE,    /* 要等到全部指定的 bit */
                                           ticks);
    return (bits & WIFI_GOT_IP_BIT) != 0;
}

bool wifi_sta_set_credentials(const char *ssid, const char *password)
{
    size_t n;

    if (ssid == NULL || password == NULL) {
        return false;
    }

    /* 长度必须在【存进 NVS 之前】检查：超长的 SSID 被截断后照样能写进 NVS，
       但永远连不上 —— 那种"命令说成功了、就是连不上"的毛病最难查。 */
    n = strlen(ssid);
    if (n == 0 || n > SSID_MAX_LEN) {
        printf("[wifi] ✗ SSID 长度 %u 不合法（1~%d 字节）\n",
               (unsigned)n, SSID_MAX_LEN);
        return false;
    }
    if (strlen(password) > PASS_MAX_LEN) {
        printf("[wifi] ✗ 密码超过 %d 字节\n", PASS_MAX_LEN);
        return false;
    }

    /* 先落盘。写失败就整个放弃，不动当前连接 ——
       否则会出现"内存里换了、重启又变回去"这种鬼打墙。 */
    if (!nvs_save_credentials(ssid, password)) {
        return false;
    }

    copy_str(s_ssid, sizeof(s_ssid), ssid);
    copy_str(s_pass, sizeof(s_pass), password);

    printf("[wifi] 凭据已更新，正在切到 \"%s\" ...\n", s_ssid);

    apply_credentials();     /* 必须先改配置，再断开重连 */
    s_retry = 0;

    /* esp_wifi_disconnect() 会触发 DISCONNECTED 事件，回调里会自己
       esp_wifi_connect() —— 正常情况下不用在这里再连一次。

       但如果当时本来就没连上，disconnect 会返回错误、也不会有事件，
       那就得手动补一脚。 */
    if (esp_wifi_disconnect() != ESP_OK) {
        esp_wifi_connect();
    }

    return true;
}

const char *wifi_sta_get_ssid(void)
{
    return s_ssid;
}

void wifi_sta_set_giveup_handler(wifi_sta_giveup_cb_t cb)
{
    s_giveup_handler = cb;
}
