/**
  * @file    cmd.c
  * @brief   命令解析 —— 缓冲区、关键字表、动作派发
  *
  * 对外接口和下位机契约见 cmd.h。
  *
  * @par 两条并存的命令路径
  *
  *   ① 文本关键字（旧）：把收到的字节攒进缓冲区，再找 "开灯" / "on" /
  *      "net tcp" / "wifi ..." 这些关键字。这套为 TCP/UDP/MQTT 三路共用，
  *      见 cmd_try_one() 和那张 s_cmds 表。
  *
  *   ② JSON 灯控协议（新）：主控通过 UDP 组播（239.0.0.2:8889）下发
  *      {"cmd":"on"/"off","light":"bedroom","version":1}。只要一包数据
  *      以 '{' 开头就走这条路，不进缓冲区 —— 既防 JSON 里的 "on"/"off"
  *      误中旧命令表，也保留旧文本命令路径。协议规范在
  *      ../MusicPlaySyncSpeakers/docs/02-设计/灯控协议规范.md。
  *
  * @see     学习笔记/ESP8266-TCP-UDP-WiFi-STA.md §1.2、§5.5、§6
  */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wifi_sta.h"
#include "relay.h"
#include "link.h"
#include "mqtt_link.h"
#include "cmd.h"

/**
  * @brief  命令缓冲区大小（字节）
  *
  * @par 坑 1：TCP 是【字节流】，不是消息队列
  *
  *   你点一次"发送"，这边 recv() 收到的可能是完整的 "开灯"、被拆开的
  *   "开" + "灯"、或者粘在一起的 "开灯关灯"。
  *
  *   所以【绝对不能用每次 recv 到的内容直接去比较】。正确做法：把收到的字节
  *   先攒进这个缓冲区，再在缓冲区里找关键字；找到就把那条命令抠掉，
  *   剩下的继续留着等下一批字节来拼。
  *
  * @note  坑 1 是【TCP 特有】的。换成 UDP 之后只剩"粘包"那一半：
  *        UDP 保留消息边界，不会被拆开，但发送方仍然可以在一包里塞两条命令。
  *        所以下面这套写法【照旧必须留着】。
  *
  * @see   学习笔记/ESP8266-TCP-UDP-WiFi-STA.md §1.2、学习笔记/ESP8266开发流程.md §9.4
  *
  * @par 坑 2：中文"长什么样"，取决于【谁】把它变成字节
  *
  *   strstr() 比的是字节。同样是"开灯"，UTF-8 是 6 字节、GBK 是 4 字节。
  *   本文件存成 UTF-8，而中文 Windows 上的网络调试助手大多发 GBK ——
  *   两边对不上就永远找不到，现象是：
  *
  *     【串口明明打印"收到 N 字节"，但继电器不动，也没有回复。】
  *
  *   所以下面那张表把两种编码都收进来，另外再给一组纯 ASCII 的 on/off。
  *
  * @see   学习笔记/ESP8266-TCP-UDP-WiFi-STA.md §6.2（怎么看出来的）、学习笔记/ESP8266开发流程.md §9.5
  */
#define CMD_BUF_SIZE  64

/**
  * @brief  一条链路的接收缓冲区
  *
  * @par 为什么每条链路一份，而不是共用一个
  *
  *   现在最多有两条链路同时在收，而它们的回调跑在【两条不同的任务】上。
  *   共用一份的话，两个任务会同时往里 memcpy、同时改长度 —— 攒出来的
  *   东西谁都不认识。各用各的，就不需要加锁。
  *
  * @note  顺带解决了一个旧毛病：以前换链路要清空缓冲区，怕的是 A 链路留下
  *        的半条命令被 B 链路的字节补全，拼出一条谁都没发过的命令。现在
  *        两条的字节根本不见面，那个坑从结构上没有了 —— 但【关掉某条链路
  *        时仍然要把它自己那份清掉】，理由和做法见 cmd_on_rx_from()。
  */
typedef struct {
    char buf[CMD_BUF_SIZE + 1];   ///< 攒字节的地方，+1 留给结尾的 '\0'
    int  len;                     ///< 里面当前有多少个有效字节
    volatile bool stale;          ///< 这条链路被关过，里面可能留着半条命令
} cmd_rx_t;

/** @brief 每条链路一份，下标就是 link_mode_t */
static cmd_rx_t s_rx[LINK_COUNT];

/**
  * @brief "开灯" 的 UTF-8 字节
  *
  * @note  关键字特意写成【转义字节】，而不是让字面量跟着文件编码走 ——
  *        这样无论本文件存成 UTF-8 还是 GBK，匹配到的都是同一串字节。
  */
#define KEY_KAI_UTF8   "\xE5\xBC\x80\xE7\x81\xAF"

/** @brief "开灯" 的 GBK 字节 */
#define KEY_KAI_GBK    "\xBF\xAA\xB5\xC6"

/** @brief "关灯" 的 UTF-8 字节 */
#define KEY_GUAN_UTF8  "\xE5\x85\xB3\xE7\x81\xAF"

/** @brief "关灯" 的 GBK 字节 */
#define KEY_GUAN_GBK   "\xB9\xD8\xB5\xC6"

/**
  * @brief  一条命令对应一个"动作"
  *
  * @note   用枚举而不是 bool：再来第三种（比如"查询状态"）时，
  *         只是往下面那张表里加一行、往 switch 里加一个 case。
  */
typedef enum {
    ACT_ON,         ///< 继电器吸合
    ACT_OFF,        ///< 继电器释放
    ACT_NET_TCP,    ///< 主链路换成 TCP
    ACT_NET_UDP,    ///< 主链路换成 UDP
    ACT_MQTT_ON,    ///< 打开 MQTT 那一路
    ACT_MQTT_OFF,   ///< 关掉 MQTT 那一路
    ACT_CLOUD_ON,   ///< 只往云端写 "on"，继电器不动
    ACT_CLOUD_OFF,  ///< 只往云端写 "off"，继电器不动
} cmd_action_t;

/**
  * @brief  命令表里的一行：关键字 → 动作
  */
typedef struct {
    const char   *key;    ///< 关键字（按字节比较）
    int           len;    ///< 关键字的【字节数】—— 中文不是 1 个字 1 个字节
    cmd_action_t  action; ///< 命中后执行哪个动作
} cmd_t;

/**
  * @brief  命令表
  *
  * @warning 这张表是【扫完再决定】的，不是"找到第一条就返回" —— 别改成那样。
  *          规则：在所有命中的关键字里，挑【在缓冲区里位置最靠前】的那一个。
  *
  *          这条规则不是摆设：下面的 "cloud on" / "cloud off" / "mqtt on" /
  *          "mqtt off" 都【包含】短关键字 "on" / "off"，两条都能命中。
  *          靠的就是它 —— 拿 "cloud on" 说，c 在第 0 位、"on" 在第 6 位，
  *          c 靠前，所以选中的是 cloud on。
  *
  * @note    往这张表里加关键字时，len 必须数【字节】：
  *          "on" = 2、"off" = 3、"net tcp" = 7，中文按上面的转义串数。
  *          数错了不会报错，只会把后面相邻的字节一起吃掉。
  */
static const cmd_t s_cmds[] = {
    { KEY_KAI_UTF8,  6, ACT_ON  },
    { KEY_KAI_GBK,   4, ACT_ON  },
    { KEY_GUAN_UTF8, 6, ACT_OFF },
    { KEY_GUAN_GBK,  4, ACT_OFF },
    { "on",          2, ACT_ON  },   /* ASCII 别名，小写；不区分大小写的版本没做 */
    { "off",         3, ACT_OFF },

    /* 主链路切换。故意带上 "net " 前缀，而不是直接叫 "udp" / "tcp" ——
       裸的 "tcp" 太容易在别的句子里撞上，而这两个词是要改设备行为的，
       撞一次就够难受了。
       ⚠ 和 "wifi " 不同，它们【不需要换行结尾】，匹配到就立刻执行，
         这一点和 on / off 一样。
       ⚠ 换的是【跟 ESP32 那条主链路】，MQTT 那一路不受影响。 */
    { "net tcp",     7, ACT_NET_TCP },
    { "net udp",     7, ACT_NET_UDP },

    /* MQTT 那一路的开关。和上面两条分开写，因为它是【独立】的一路：
       可以和主链路同时开着，也可以单独关掉。
       ⚠ 关键字里也含 "on" / "off"，靠的还是"位置最靠前"那条规则，
         理由同下面的 cloud。 */
    { "mqtt on",     7, ACT_MQTT_ON  },
    { "mqtt off",    8, ACT_MQTT_OFF },

    /* 云端。这两条【只管云端记着的那个值】，板子上的继电器一动不动 ——
       这就是巴法云 /set 的语义：不推给任何订阅者，只更新服务端存的值。
       两者都带上 "cloud " 前缀，是为了不和裸的 on / off 混在一起
       （裸的那两个是真拉继电器的）。 */
    { "cloud on",    8, ACT_CLOUD_ON  },
    { "cloud off",   9, ACT_CLOUD_OFF },
};

/**
  * @brief 带参数的命令头：wifi \<SSID\>,\<密码\>
  *
  * @note  它没法放进上面那张表 —— 表里每条都是【定长关键字】，匹配到就能按
  *        固定长度抠掉；而这条后面挂着两个长度不定的参数。
  *        所以单独处理，但它的【先后顺序】仍然要和表里那些一起排
  *        （见 cmd_try_one()），否则 "开灯wifi A,B\n" 的执行顺序就乱了。
  *
  * @note  为什么用逗号而不是空格分隔？因为 SSID 里【可以有空格】——
  *        用空格当分隔符会把 "My Home WiFi" 劈成两半。
  */
#define KEY_WIFI       "wifi "

/** @brief KEY_WIFI 的字节数（不含结尾 '\0'） */
#define KEY_WIFI_LEN   5

/** @brief 802.11 协议规定的 SSID 上限（字节） */
#define WIFI_SSID_MAX  32

/** @brief 802.11 协议规定的密码上限（字节） */
#define WIFI_PASS_MAX  64

/** @brief 命令表里有多少条 */
#define CMD_COUNT  (sizeof(s_cmds) / sizeof(s_cmds[0]))

/**
  * @brief    处理一条 wifi 命令
  *
  * 🟢 L2 —— 工具：切出逗号两边的 SSID 和密码。真正的坑（半条命令不能
             动执行表）在 cmd_try_one() 那一段。
  *
  * @param[in] src  这条命令是从哪条链路来的（回执发回那儿）
  * @param[in] p    指向缓冲区里找到的 "wifi "
  *
  * @retval   true   这一行处理完了（不管成功失败），已经从缓冲区里抠掉，调用方继续
  * @retval   false  【行还没收全】，什么都没做，等下一批字节
  *
  * @warning  返回值跟别的命令【不一样】，调用方要留意。
  *
  * @note     为什么必须等到整行？因为 TCP 是字节流，一次 recv 可能只到
  *           "wifi DESKTOP" 就断了，这时候后面的密码还没来。
  *           硬猜的话会把半截 SSID 当成真的存进 NVS。
  *
  * @note     所以规矩是：以换行结尾。网络调试助手发的时候记得勾"发送新行"。
  */
static bool cmd_do_wifi(link_mode_t src, char *p)
{
    cmd_rx_t *rx = &s_rx[src];   /* p 就指在这条链路自己那份缓冲区里 */
    char   *nl;
    char   *comma;
    char   *next;
    char    ssid[WIFI_SSID_MAX + 1];
    char    pass[WIFI_PASS_MAX + 1];
    size_t  n;
    bool    valid = true;

    nl = strpbrk(p, "\r\n");
    if (nl == NULL) {
        return false;              /* 行还没到齐 */
    }

    p += KEY_WIFI_LEN;             /* 跳过 "wifi " 本身 */

    comma = memchr(p, ',', (size_t)(nl - p));
    if (comma == NULL) {
        printf("[cmd] ✗ wifi 命令里没有逗号。格式：wifi <SSID>,<密码>\n");
        valid = false;
    } else {
        n = (size_t)(comma - p);
        if (n == 0 || n > WIFI_SSID_MAX) {
            printf("[cmd] ✗ SSID 长度 %u 不合法（1~%d 字节）\n",
                   (unsigned)n, WIFI_SSID_MAX);
            valid = false;
        } else {
            memcpy(ssid, p, n);
            ssid[n] = '\0';

            /* 密码从逗号后一直到行尾。允许是空串（开放热点），
               所以这里不像 SSID 那样检查 n == 0。 */
            p = comma + 1;
            n = (size_t)(nl - p);
            if (n > WIFI_PASS_MAX) {
                printf("[cmd] ✗ 密码超过 %d 字节\n", WIFI_PASS_MAX);
                valid = false;
            } else {
                memcpy(pass, p, n);
                pass[n] = '\0';
            }
        }
    }

    /* 参数没问题才真去改。改失败时 wifi_sta_set_credentials() 内部会打原因，
       而且【不会】动当前连接 —— 所以这条命令是安全的，
       打错字不会把节点弄成砖。 */
    if (valid) {
        printf("[cmd] 换热点 → \"%s\"\n", ssid);
        /* ⚠ 回执【必须全大写】：MQTT 模式下每条回执都会被推到 /up 上，
           别的节点会拿它当命令解析。这里原来写的是 "reconnecting"，
           里面那个小写的 "on" 正好命中命令表，会让对面的继电器吸合。
           这条规矩的完整理由见 link.h 的 link_send()。 */
        link_send(src, wifi_sta_set_credentials(ssid, pass)
                       ? "WIFI OK (RECONNECTING)\r\n"
                       : "WIFI FAIL (see serial log)\r\n", 0);
    } else {
        link_send(src, "WIFI FAIL (bad format)\r\n", 0);
    }

    /* 不管成功失败，这一整行都要从缓冲区里抠掉。
       漏掉的话它会永远卡在最前面，把后面所有命令都堵死。 */
    next = nl;
    while (*next == '\r' || *next == '\n') {
        next++;
    }
    rx->len -= (int)(next - rx->buf);
    memmove(rx->buf, next, rx->len);
    rx->buf[rx->len] = '\0';

    return true;
}

/**
  * @brief    在缓冲区里找一条命令并执行
  *
  * 🟡 L1 —— 架构：挑缓冲区里位置最靠前的那条命令执行。抠掉命令和清
             缓冲区的先后、半条 wifi 行不许动执行表，都是这里定的。
  *
  * @retval   true   找到并处理了一条（调用方要接着再试，可能还有第二条）
  * @retval   false  没找到完整命令，等下一批字节
  */
static bool cmd_try_one(link_mode_t src)
{
    const cmd_t *found = NULL;
    char        *hit   = NULL;
    int          i;
    cmd_rx_t    *rx = &s_rx[src];    /* 这条链路自己那份缓冲区 */
    bool         switched = false;   /* 这一轮是不是换了主链路 */
    link_mode_t  old_master = LINK_TCP;   /* 换之前是哪条，换完好清它的缓冲区 */

    /* 扫整张表，挑【位置最靠前】的那条 —— 这样收到 "开灯关灯" 时，
       会先执行开灯、再执行关灯，顺序和对方发送的顺序一致。 */
    for (i = 0; i < (int)CMD_COUNT; i++) {
        char *p = strstr(rx->buf, s_cmds[i].key);
        if (p != NULL && (hit == NULL || p < hit)) {
            hit   = p;
            found = &s_cmds[i];
        }
    }

    /* 把带参数的 "wifi " 也拉进来一起比位置。

       ⚠ 这里有个规矩容易忽略：如果 "wifi " 靠前、但【整行还没到齐】，
         cmd_do_wifi() 会返回 false，我们就必须原样返回 false 继续等，
         而【不能】退回去执行表里那条 —— 那等于把后面的命令提前执行了。

       "wifi " 末尾那个空格是有意的：它保证匹配到的是命令头，
       而不是某个 SSID 里恰好出现的 "wifi"。 */
    {
        char *w = strstr(rx->buf, KEY_WIFI);
        if (w != NULL && (hit == NULL || w < hit)) {
            return cmd_do_wifi(src, w);
        }
    }

    if (found == NULL) {
        return false;      /* 还没拼出完整的命令，继续等 */
    }

    switch (found->action) {
    case ACT_ON:
        relay_on();
        /* 回复用纯 ASCII：无论调试助手设成 UTF-8 还是 GBK，都不会显示成乱码 */
        link_send(src, "LED ON  (GPIO0 = HIGH)\r\n", 0);
        break;

    case ACT_OFF:
        relay_off();
        link_send(src, "LED OFF (GPIO0 = LOW)\r\n", 0);
        break;

    /* 换主链路。回执由 link_switch() 自己发（发回 src）—— 它要赶在旧主链路
       被关掉【之前】把回执发出去，所以这里不要再补一句 link_send()，
       补了会发两遍，而且第二遍多半已经发不出去了。
       ⚠ 要关掉的是【换向之前】那条主链路，所以先把它的名字记下来 ——
         换完再问 link_master() 拿到的就是新值了。 */
    case ACT_NET_TCP:
    case ACT_NET_UDP: {
        link_mode_t target = (found->action == ACT_NET_TCP) ? LINK_TCP : LINK_UDP;

        old_master = link_master();
        link_switch(src, target);
        /* 真换了才算 —— 本来就是这条的话，链路和缓冲区都该原样留着，
           不能把这条命令后面那几条还没执行的命令一起清掉。 */
        switched = (link_master() != old_master);
        break;
    }

    /* MQTT 那一路的开关。回执同样由 link_set_mqtt() 自己发 ——
       关的时候它是"先回执、再关"，这里补一句就发不出去了。 */
    case ACT_MQTT_ON:
        link_set_mqtt(src, true);
        break;

    case ACT_MQTT_OFF:
        link_set_mqtt(src, false);
        break;

    /* 云端那两条：只往 <主题>/set 发布，【继电器一动不动】。
       回执照旧走 link_send()，也就是命令来的那条路 —— 命令要是从 MQTT
       进来的，它会推到 /up 上。所以回执文案【必须全大写】：里面要是混进
       一个小写的 "on"，别的订阅了这个主题的节点收到后会当成开灯命令执行。 */
    case ACT_CLOUD_ON:
        if (mqtt_link_send_set("on", 0) < 0) {
            link_send(src, "CLOUD FAIL (mqtt not up)\r\n", 0);
        } else {
            link_send(src, "CLOUD SET ON\r\n", 0);
        }
        break;

    case ACT_CLOUD_OFF:
        if (mqtt_link_send_set("off", 0) < 0) {
            link_send(src, "CLOUD FAIL (mqtt not up)\r\n", 0);
        } else {
            link_send(src, "CLOUD SET OFF\r\n", 0);
        }
        break;
    }

    /* 把这条命令从缓冲区里抠掉：
       把 hit 之后的内容整体搬到开头，长度相应减少。
       注意减掉的是找到的那条命令【自己的字节数】，不是写死的 2。 */
    char *next = hit + found->len;
    rx->len -= (int)(next - rx->buf);
    memmove(rx->buf, next, rx->len);
    rx->buf[rx->len] = '\0';

    /* 刚换了主链路 → 给【两条】主链路各挂一个"要清"的标志。

       关掉的那条可能攒着半条命令，等它下次再被打开，新来的字节会接着
       那半条往下拼，拼出一条谁都没发过的命令；新打开的那条也可能留着
       上一回用剩的残渣。MQTT 那份不用管 —— 它一次给一条完整消息，
       攒不出半条命令来。

       ⚠ 这里【只置标志，不直接清】。本函数跑在命令来的那条链路的任务上，
         而另一条链路的任务这会儿可能正往它自己的缓冲区里 memcpy ——
         两个任务一起写同一块内存就撞车了。真正动手清的是各自的任务，
         见 cmd_on_rx_from()。

       ⚠ 且必须放在【抠掉命令之后】。放前面的话 rx->len 先归零，
         上面那句相减会变成负数，memmove 的长度会大得离谱，直接崩。 */
    if (switched) {
        s_rx[old_master].stale = true;
        s_rx[link_master()].stale = true;   /* 换完的这条 = 新打开的那条 */
    }

    return true;
}

/**
  * @brief    把收到的字节按十六进制打出来
  *
  * 🟢 L2 —— 工具：调试用，最多打 16 个字节。
  *
  * @param[in] data  要打印的字节
  * @param[in] len   字节数
  *
  * @note     加这个是因为"编码不对"这类毛病，光看【字节数】是看不出来的：
  *           把实际字节打出来，一眼就能对上自己是 UTF-8 还是 GBK。
  *           最多打 16 个，免得刷屏。
  */
static void dump_hex(const char *data, int len)
{
    int n = (len > 16) ? 16 : len;
    int i;

    for (i = 0; i < n; i++) {
        /* 必须转成 unsigned char：char 在这里是有符号的，
           不转的话 0xBF 会被符号扩展成 FFFFFFBF，打印出 8 位。 */
        printf(" %02X", (unsigned char)data[i]);
    }
    if (len > n) {
        printf(" ...");
    }
    printf("\n");
}

/* =========================================================================
 * JSON 灯控协议（新路径）
 * =========================================================================
 * 主控通过 UDP 组播下发 {"cmd":...,"light":...,"version":...}。
 * 这里只做"够用就好"的最小解析：
 *   - 不引入 cJSON：1MB 的板子 + 冻结的 ESP8266_RTOS_SDK v3.4，加组件有
 *     编译/链接风险；协议字段又固定、又都是简单 ASCII，手搓更安全。
 *   - 不分配内存：所有解析都在定长栈缓冲区里完成。
 * 版本协商、light 校验、verr 回复都严格照 灯控协议规范.md 来。
 */

/**
  * @brief 本端支持的灯控 JSON 协议版本
  *
  * @note  协议规范里当前就是 1。收到 version > 这个值的报文判为不兼容：
  *        丢弃 + 回 verr。
  */
#define LIGHT_PROTO_VER   1

/** @brief status 周期上报间隔（毫秒），规范写死 30 秒 */
#define LIGHT_STATUS_MS   30000

/** @brief status 报文拼出来最长也就这个量级，留足余量 */
#define LIGHT_STATUS_BUF  64

/**
  * @brief  在 [hay, hay+len) 里找子串 needle（长度 nlen）
  *
  * 🟢 L2 —— 工具：newlib 的 strstr 要 '\0' 结尾，而 recvfrom 出来的数据
  *         不保证结尾有 '\0'；memmem 在 ESP 的 libc 里又不一定有。
  *         所以自己写一个长度有界的子串查找，避免任何对 '\0' 的依赖。
  *
  * @retval 首次命中的位置，找不到返回 NULL
  */
static const char *mem_find(const char *hay, int len,
                            const char *needle, int nlen)
{
    int i;

    if (nlen <= 0 || nlen > len) {
        return NULL;
    }
    for (i = 0; i <= len - nlen; i++) {
        if (memcmp(hay + i, needle, nlen) == 0) {
            return hay + i;
        }
    }
    return NULL;
}

/**
  * @brief  在 JSON 文本里取一个【字符串】字段的值
  *
  * 🟢 L2 —— 工具：只认最简单的 "\"key\": \"value\""，够协议用。
  *
  * @param[in]  json     整段 JSON（不要求 '\0' 结尾，len 管长度）
  * @param[in]  len      json 字节数
  * @param[in]  key      字段名（不含引号，比如 "cmd"）
  * @param[out] out      装去引号后的值
  * @param[in]  outsize  out 大小（含结尾 '\0'）
  *
  * @retval   true   找到，out 已填
  * @retval   false  没该字段 / 不是字符串 / 值太长装不下
  *
  * @note     只取第一个命中的 key；协议里每个 key 只出现一次。
  * @note     值里不含引号、转义（cmd/light 都是简单 ASCII）。
  */
static bool json_get_str(const char *json, int len,
                         const char *key, char *out, int outsize)
{
    char         pat[32];
    const char  *p, *q, *vstart, *vend;
    int          plen;

    snprintf(pat, sizeof(pat), "\"%s\"", key);
    plen = (int)strlen(pat);

    p = mem_find(json, len, pat, plen);
    if (p == NULL) {
        return false;
    }
    /* 跳过标记、空白、冒号、空白 */
    q = p + plen;
    while (q < json + len &&
           (*q == ' ' || *q == ':' || *q == '\t' ||
            *q == '\n' || *q == '\r')) {
        q++;
    }
    if (q >= json + len || *q != '"') {
        return false;       /* 值不是字符串（这协议里 cmd/light 都是字符串） */
    }
    q++;                    /* 跳过开头引号 */
    vstart = q;
    while (q < json + len && *q != '"') {
        q++;
    }
    if (q >= json + len) {
        return false;       /* 引号没闭合，JSON 截断 */
    }
    vend = q;
    if (vend - vstart + 1 > outsize) {
        return false;       /* 装不下 */
    }
    memcpy(out, vstart, (size_t)(vend - vstart));
    out[vend - vstart] = '\0';
    return true;
}

/**
  * @brief  在 JSON 文本里取一个【整数】字段的值
  *
  * 🟢 L2 —— 工具：和 json_get_str 同理，但值按十进制整数解析。
  *         协议里 version 就是整数。
  *
  * @retval   true   找到并解析成功，*out 已填
  * @retval   false  没该字段 / 不是整数
  */
static bool json_get_int(const char *json, int len,
                         const char *key, int *out)
{
    char         pat[32];
    const char  *p, *q;
    int          plen, sign = 1, val = 0, got = 0;

    snprintf(pat, sizeof(pat), "\"%s\"", key);
    plen = (int)strlen(pat);

    p = mem_find(json, len, pat, plen);
    if (p == NULL) {
        return false;
    }
    q = p + plen;
    while (q < json + len &&
           (*q == ' ' || *q == ':' || *q == '\t' ||
            *q == '\n' || *q == '\r')) {
        q++;
    }
    if (q >= json + len) {
        return false;
    }
    if (*q == '-') { sign = -1; q++; }
    else if (*q == '+') { q++; }
    while (q < json + len && *q >= '0' && *q <= '9') {
        val = val * 10 + (*q - '0');
        got = 1;
        q++;
    }
    if (!got) {
        return false;
    }
    *out = sign * val;
    return true;
}

/**
  * @brief  拼一条 status 报文发往组播组
  *
  * 🟢 L2 —— 工具：30 秒周期上报用它，开关命令的即时 ack 也用它（见
  *         cmd_handle_json()）。格式严格照协议规范。
  *
  * @note  ok = 当前【实际】继电器状态，直接读 GPIO 电平（relay_get_state()），
  *        这样即便状态机和真实硬件对不上，上报的也是"灯实际亮没亮"。
  *
  * @note  必须走 UDP 组播：link_send(LINK_UDP) 内部直接调 udp_client_send()，
  *        发往 239.0.0.2:8889 —— 就算本端主链路当前是 TCP，这条回复也得走
  *        UDP 组播（主控只在那个组里听着）。所以这里写死 LINK_UDP，
  *        不跟着 s_master 走。
  */
static void cmd_send_status(void)
{
    bool on = relay_get_state();
    char buf[LIGHT_STATUS_BUF];

    snprintf(buf, sizeof(buf),
             "{\"cmd\":\"status\",\"light\":\"bedroom\",\"ok\":%s,\"version\":1}\r\n",
             on ? "true" : "false");

    if (link_send(LINK_UDP, buf, 0) < 0) {
        printf("[cmd] 状态上报失败：UDP 组播未启用（当前主链路不是 UDP？）\n");
    }
}

/**
  * @brief    处理一条灯控 JSON 报文（UDP 组播那条协议）
  *
  * 🟡 L1 —— 架构：解析 → 版本协商 → 校验 light → 执行 / 回 verr。
  *         和 cmd_try_one() 平级：数据开头是 '{' 走这里，否则走旧文本路径，
  *         两条共存。
  *
  * @param[in] src   报文从哪条链路来（verr / ack 发回同一条路）
  * @param[in] data  整段 JSON 文本
  * @param[in] len   字节数（不要求 '\0' 结尾）
  *
  * @see      ../MusicPlaySyncSpeakers/docs/02-设计/灯控协议规范.md
  */
static void cmd_handle_json(link_mode_t src, const char *data, int len)
{
    char cmd[16];
    char light[16];
    int  version = LIGHT_PROTO_VER;   /* 缺省按 v1 兼容（对端旧固件没带 version） */
    bool have_ver;

    (void)src;   /* 回复一律走 UDP 组播（见下），不顺着 src 回，故未用 */

    /* ① 抠字段。version 缺省就是 1；cmd 没有就直接忽略这包。 */
    have_ver = json_get_int(data, len, "version", &version);
    if (!json_get_str(data, len, "cmd", cmd, sizeof(cmd))) {
        printf("[cmd] JSON 缺 cmd 字段，忽略\n");
        return;
    }
    json_get_str(data, len, "light", light, sizeof(light));  /* 没有就留空串 */

    /* ② 版本协商【必须先于任何执行】。
           version > 本端支持 → 不兼容：丢弃 + 回 verr。
           等于 / 小于 / 缺省 → 都按兼容处理（字段语义没变）。 */
    if (have_ver && version > LIGHT_PROTO_VER) {
        printf("[cmd] 收到不兼容版本 %d（本端 %d），回 verr\n",
               version, LIGHT_PROTO_VER);
        link_send(LINK_UDP,
                  "{\"cmd\":\"verr\",\"light\":\"-\",\"version\":1}\r\n", 0);
        return;
    }

    /* ③ 校验 light：协议当前只有 "bedroom"。拿到的不是给本节点的房间就忽略
           （给客厅的指令不该动卧室的灯）。light 缺省时【当作 bedroom】，
           兼容对端旧报文。 */
    if (light[0] != '\0' && strcmp(light, "bedroom") != 0) {
        printf("[cmd] light=\"%s\" 不是本节点房间，忽略\n", light);
        return;
    }

    /* ④ 按 cmd 分发。 */
    if (strcmp(cmd, "on") == 0) {
        relay_on();
        printf("[cmd] JSON 开灯 (bedroom)\n");
        cmd_send_status();                 /* 即时 ack：同格式 status */
    } else if (strcmp(cmd, "off") == 0) {
        relay_off();
        printf("[cmd] JSON 关灯 (bedroom)\n");
        cmd_send_status();
    } else if (strcmp(cmd, "status") == 0) {
        cmd_send_status();                 /* 主控主动拉状态，立刻回 */
    } else if (strcmp(cmd, "verr") == 0) {
        /* 对端告诉我们它不兼容：节点这边没有进一步动作，记一笔即可。 */
        printf("[cmd] 收到对端 verr（对方固件版本高于本端）\n");
    } else {
        printf("[cmd] JSON cmd=\"%s\" 未识别，忽略\n", cmd);
    }
}

/**
  * @brief    状态上报定时器任务：每 30 秒发一条 status
  *
  * 🟡 L1 —— 架构：周期固定 30s，不受开关命令影响（协议规范原文）。
  *         上报走 UDP 组播（cmd_send_status() 内部写死 LINK_UDP），
  *         和主链路是 TCP 还是 UDP 无关。
  *
  * @param[in] pvParameters  未用
  *
  * @note     先等 WiFi 拿到 IP 再开始，避免一开机狂打失败日志。
  */
static void cmd_status_task(void *pvParameters)
{
    (void)pvParameters;

    if (!wifi_sta_wait_ip(UINT32_MAX)) {
        printf("[cmd] 状态上报任务等 IP 超时，退出\n");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        vTaskDelay(LIGHT_STATUS_MS / portTICK_PERIOD_MS);
        cmd_send_status();
    }
}

/**
  * @brief    启动 30 秒状态上报任务
  *
  * 🟢 L2 —— 工具：开机由 main.c 调一次。任务只创建一次（幂等见内部判断）。
  *
  * @note     上报只发【不收】，和 link_switch() 里"TCP/UDP 互斥"那条规矩
  *           不冲突 —— 它只占用了 UDP 的【发送】能力，没有再开一条接收主链路。
  */
void cmd_start_status_reporter(void)
{
    static bool started = false;

    if (started) {
        return;
    }
    started = true;
    xTaskCreate(cmd_status_task, "cmd_status", 2048, NULL, 4, NULL);
}

/**
  * @brief    每收到一批字节就被调用一次
  *
  * 🟡 L1 —— 架构：追加新字节、循环取出所有完整命令。半条命令要留在
             缓冲区里等下一包 —— 缓冲区多大、留多久，是这里定的。
  *
  * @param[in] src   这批字节是从哪条链路来的
  * @param[in] data  裸字节，可能只是一条命令的一部分
  * @param[in] len   字节数
  *
  * @note     攒的时候用的是【这条链路自己那份】缓冲区，所以两条链路同时
  *           收也不会串味 —— 两个任务各写各的，不用加锁。
  */
static void cmd_on_rx(link_mode_t src, const char *data, int len)
{
    cmd_rx_t *rx = &s_rx[src];
    int       i;

    /* 灯控 JSON 协议走 UDP 组播，报文是完整数据报、开头是 '{'。
       只要这包以 '{'（忽略前导空白）开头就当 JSON 走新协议，不进下面的
       文本关键字缓冲区 —— 既防 JSON 里的 "on"/"off" 误中旧命令表，也保留
       旧文本命令路径（非 JSON 数据继续走 cmd_try_one()）。 */
    for (i = 0; i < len; i++) {
        if (data[i] == ' ' || data[i] == '\t' ||
            data[i] == '\r' || data[i] == '\n') {
            continue;
        }
        if (data[i] == '{') {
            cmd_handle_json(src, data, len);
            return;
        }
        break;   /* 第一个非空白字符不是 '{' → 走文本路径 */
    }

    printf("[cmd] 收到 %d 字节:", len);
    dump_hex(data, len);

    /* 缓冲区放不下就整个丢掉重来。
       走到这一步说明前面攒的字节一直没拼出任何命令（比如对方发的是乱码）。 */
    if (rx->len + len > CMD_BUF_SIZE) {
        printf("[cmd] 缓冲区放不下，丢掉前面攒的 %d 字节\n", rx->len);
        rx->len = 0;
    }

    /* 极端情况：一次就送来超过缓冲区大小的数据，只留最后一段 */
    if (len > CMD_BUF_SIZE) {
        data += (len - CMD_BUF_SIZE);
        len = CMD_BUF_SIZE;
    }

    memcpy(rx->buf + rx->len, data, len);
    rx->len += len;
    rx->buf[rx->len] = '\0';

    /* 一次可能匹配出好几条命令，循环处理干净 */
    while (cmd_try_one(src)) {
        /* 空循环体，活儿都在 cmd_try_one() 里干完了 */
    }
}

/**
  * @brief    带【来源】的入口：从已经关掉的那条链路来的数据，一律丢掉
  *
  * 🟢 L2 —— 工具：关掉的那条来的直接丢，是就转 cmd_on_rx()。
  *
  * @param[in] src   这包数据是从哪条链路来的
  * @param[in] data  裸字节，可能只是一条命令的一部分
  * @param[in] len   字节数
  *
  * @note     判断的是"这一路【开着没有】"，不是"是不是当前那条"—— 现在
  *           MQTT 和主链路可以同时开着，两条都得放行。
  *
  * @note     为什么需要这一层：三个传输模块的 stop() 都只是置个标志就返回，
  *           链路要过一会儿才安静下来 —— tcp/udp 要等下一次 recv/recvfrom
  *           超时（最多 5 秒），MQTT 那个则连 broker 都不肯断开。
  *           这段窗口里【旧链路还在收】—— 不管它的话，关掉之后发过来的
  *           命令照样会被执行。
  *
  *           加上这一层，开关就是【干脆】的：标志一改，那条链路的包
  *           立刻失效，不用等它自己慢慢关。也正因为有这一层兜底，
  *           mqtt_link_stop() 才敢只置一个标志（原因见 mqtt_link.h）。
  *
  * @note     src 还要继续往下传（一路传到 link_send()）—— 回执必须发回命令
  *           来的那条路，不能发到"当前那条"上去，因为现在没有"当前那条"了。
  */
static void cmd_on_rx_from(link_mode_t src, const char *data, int len)
{
    cmd_rx_t *rx = &s_rx[src];

    if (!link_is_open(src)) {
        printf("[cmd] 忽略 %d 字节：来自已关闭的链路\n", len);
        return;
    }

    /* 这条链路被关过 → 先把它那份缓冲区清干净再收新的。
       不清的话，关之前留下的半条命令会被现在这些字节"补全"，
       拼出一条谁都没发过的命令。

       ⚠ 只能在这里清：本函数跑在这条链路【自己的任务】上，动的是自己的
         缓冲区。换链路的那条任务只挂标志（见 cmd_try_one()），不替别人清。 */
    if (rx->stale) {
        rx->len = 0;
        rx->buf[0] = '\0';
        rx->stale = false;
    }

    cmd_on_rx(src, data, len);
}

void cmd_on_tcp_rx(const char *data, int len)
{
    cmd_on_rx_from(LINK_TCP, data, len);
}

void cmd_on_udp_rx(const char *data, int len)
{
    cmd_on_rx_from(LINK_UDP, data, len);
}

void cmd_on_mqtt_rx(const char *data, int len)
{
    cmd_on_rx_from(LINK_MQTT, data, len);
}
