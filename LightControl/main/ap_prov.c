/**
  * @file    ap_prov.c
  * @brief   AP 配网模式 —— 开热点、起网页、把手机填的凭据收上来
  *
  * 对外接口和整段流程见 ap_prov.h。这个文件里只有三件事值得单独记：
  * 表单的两个字段怎么解出来、网页长什么样、以及收工的时候按什么顺序拆。
  *
  * 它对外只依赖 wifi_sta.h 里的一个只读函数（wifi_sta_get_ssid()，用来在
  * 网页上显示"现在连不上的是哪个热点"）—— 凭据收上来之后交给谁、存到哪，
  * 这个文件一概不知道。
  *
  * @see     学习笔记/ESP8266开发流程.md §10（配网模式）
  */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"

#include "esp_http_server.h"
#include "esp_system.h"         /* esp_get_free_heap_size() */

#include "wifi_sta.h"
#include "ap_prov.h"

/* ======================= 热点 ======================= */

/**
  * @brief    热点名的前缀，后面接上 MAC 的最后两个字节
  *
  * @note     带上 MAC 是为了同时开着两块板子的时候，手机的热点列表里能分清
  *           哪块是哪块。都叫 "ESP-01S-Setup" 的话，你只能靠信号强弱猜，
  *           猜错了就是把 A 板的密码填进 B 板的 NVS 里。
  *
  * @note     算上"-XXXX"一共 18 个字符，离 802.11 的 32 字节上限还早。
  */
#define AP_SSID_PREFIX   "ESP-01S-Setup"

/**
  * @brief    配网热点自己的密码
  *
  * @warning  这不是"设备的密码"，只是别让旁边的人顺手把你的节点配到他家
  *           热点上去。它是【编译进固件的常量】，谈不上保密 —— 手里有这份
  *           固件的人都知道它。想换就改这一行。
  *
  * @note     不能少于 8 字节：WPA2 的下限，短了 esp_wifi_set_config() 会直接
  *           拒绝，表现是热点根本起不来。
  */
#define AP_PASSWORD      "12345678"

/** @brief 热点开在哪个信道。1 / 6 / 11 是互不重叠的三个，随便挑一个就行 */
#define AP_CHANNEL       1

/**
  * @brief    最多允许几台设备同时连
  *
  * @note     故意留成 2 而不是 1：手机有时会先连上一条、过一会儿才把旧的
  *           释放掉，卡在 1 上就会出现"明明没人连，却说连满了"。
  *           ESP8266 软 AP 的硬上限是 4。
  */
#define AP_MAX_CONN      2

/* ======================= 网页 ======================= */

/**
  * @brief    一次 POST 最多收多少字节的表单正文
  *
  * @note     够用：SSID 最多 33 字节、密码最多 65 字节（各比协议上限多一个，
  *           是为了给出准确的错误提示，见 PROV_RAW_SSID_SIZE），每个字节最坏
  *           会被转义成 3 个字符，再加两个字段名和分隔符，满打满算 307。
  *
  * @warning  超长的【整条拒收】，不截断。截断出来的 SSID 照样能写进 NVS，
  *           但永远连不上 —— 而页面已经对用户说过"保存成功"了，手机上
  *           什么都看不出来。
  */
#define PROV_BODY_MAX   384

/**
  * @brief    802.11 协议规定的 SSID 上限（字节）
  *
  * @note     和 wifi_sta.c 顶部那个是同一个协议常量，两个文件各留了一份。
  *           这里是【第一道】检查，作用是把错误当场显示在网页上；
  *           wifi_sta.c 里那道是最终把关的。
  */
#define PROV_SSID_MAX   32

/**
  * @brief    802.11 协议规定的密码上限（字节）
  *
  * @note     同上：这里是第一道，wifi_sta.c 里那道是最终把关的。
  *
  * @note     这道检查不是可有可无的：少了它，用户会看到"保存成功"，然后干等
  *           两分钟又被踢回配网页面 —— 中间那句真正的原因在串口上，
  *           而拿着手机的人看不到串口。
  */
#define PROV_PASS_MAX   64

/* ======================= 收转义串用的缓冲区 ======================= */

/**
  * @brief    转义之后，一个字节最坏会变成几个字符
  *
  * @note     就是 '%' 加两位十六进制：一个字节 → 三个字符。这是【上界】不是
  *           平均值 —— 字母数字保持一个字符不变，汉字和 @ # % & = 这些
  *           才会真的胀到三倍。
  */
#define PROV_ESC_FACTOR  3

/**
  * @brief    收【转义形式】的热点名用的缓冲区大小
  *
  * @note     为什么不直接用 s_new_ssid（33 字节）：后者是按"解码后"的容量开的，
  *           而 httpd_query_key_value() 切出来的还是【解码前】的转义串 ——
  *           一个汉字在那里是 9 个字符（"%E8%B4%A5"）。拿 33 字节去装，
  *           4 个汉字（36 个字符）就被判"太长"，可它解码后只有 12 字节。
  *
  * @note     括号里先 +1 再 ×3，是【故意多留一个字节的余量】：这样"超长一点
  *           点"的 SSID 也能收进来并解码，好让下面那道按字节的检查在网页上
  *           报出"你填了 33 字节，要求 1~32"，而不是笼统一句"没填或者太长"。
  *           代价是 100 字节栈，只在点"保存"那一瞬间占着。
  */
#define PROV_RAW_SSID_SIZE   ((PROV_SSID_MAX + 1) * PROV_ESC_FACTOR + 1)

/** @brief 收【转义形式】的密码用的缓冲区大小。理由同上 */
#define PROV_RAW_PASS_SIZE   ((PROV_PASS_MAX + 1) * PROV_ESC_FACTOR + 1)

/* ======================= 模块内部状态 ======================= */

/** @brief 配网事件组：网页那边收全了就在这里立个牌子 */
static EventGroupHandle_t s_prov_group;

/** @brief 事件位：用户已经提交过了，ap_prov_run() 可以收工了 */
#define PROV_DONE_BIT   BIT0

/** @brief HTTP 服务句柄；没起来的时候是 NULL */
static httpd_handle_t s_server;

/**
  * @brief 用户填的热点名（已解码）。比协议上限多一个字节，留给结尾的 '\0'
  *
  * @warning 别拿它去接 httpd_query_key_value() —— 那一步拿到的还是转义串，
  *          得先落进 PROV_RAW_SSID_SIZE 那个缓冲区里解码。见那里的说明。
  */
static char s_new_ssid[PROV_SSID_MAX + 1];

/** @brief 用户填的密码（已解码）。容量说明和警告同 s_new_ssid */
static char s_new_pass[PROV_PASS_MAX + 1];

/* ======================= 小工具 ======================= */

/**
  * @brief    把 src 拷进定长缓冲区，超长就截断，并保证以 '\0' 结尾
  *
  * 🟢 L2 —— 工具：定长拷贝，超长截断并补 \0。
  *
  * @note     wifi_sta.c 里有一个一模一样的。没有抽成公共头文件，是因为它只有
  *           六行 —— 为六行纯函数开一个模块不划算。改动的时候记得两边一起看。
  *
  * @warning  不要"简化"回 strncpy()：源串长度 >= 目标缓冲区时它【不补 '\0'】，
  *           会留下一个没有结尾的字符串，后面 strlen / printf 一路读下去越界。
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

/**
  * @brief    把一个十六进制字符转成 0~15
  *
  * 🟢 L2 —— 工具：一个字符换个数字，非法返回 -1。
  *
  * @param[in] c  待转换的字符
  *
  * @return   0~15；不是十六进制字符时返回 -1
  */
static int hex_val(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/**
  * @brief    就地把表单里的转义还原成原字符
  *
  * 🟢 L2 —— 工具：表单解码，就地把 %XX 和 + 还原。
  *
  * @param[in,out] s  以 '\0' 结尾的字符串，就地改写
  *
  * @note     浏览器提交表单时，空格写成加号，其余"不安全"的字节写成百分号
  *           加两位十六进制 —— 一个汉字会变成三组。不还原的话，写进 NVS 的
  *           就是那串转义本身，然后一直连不上，而且串口上看着 SSID 是"对"的。
  *
  * @note     还原只会让串【变短】（三字节变一字节、一字节变一字节），
  *           所以就地改写是安全的：写指针永远不越过读指针。
  *
  * @warning  单独的加号确实会被还原成空格。这不是误伤：热点名里真有加号时，
  *           浏览器会先把它转义成别的东西发过来，走到这里时那个加号已经不是
  *           加号了。能走到这个分支的加号，本来的含义就是空格。
  */
static void url_decode(char *s)
{
    char       *w = s;      /* 写指针 */
    const char *r = s;      /* 读指针 */

    while (*r != '\0') {
        /* 条件里那两个 hex_val 是"先探路"，值本身下面再算一遍 —— 多算两次
           换来的是没有中间变量，也就没有"哪个分支没赋上值"的余地。 */
        if (*r == '+') {
            *w++ = ' ';
            r++;
        } else if (*r == '%' &&
                   hex_val(r[1]) >= 0 && hex_val(r[2]) >= 0) {
            *w++ = (char)((hex_val(r[1]) << 4) | hex_val(r[2]));
            r += 3;
        } else {
            /* 走到这里的 % 是孤零零一个（r[1] 是 '\0'，第一个条件就不成立，
               && 短路，根本没有去读 r[2]），原样搬过去即可。 */
            *w++ = *r++;
        }
    }
    *w = '\0';
}

/**
  * @brief    把 HTML 里有特殊含义的字符换成实体
  *
  * 🟢 L2 —— 工具：四个字符换实体 —— 防的是把 SSID 里的尖括号当标签用。
  *
  * @param[out] dst       目标缓冲区
  * @param[in]  dst_size  dst 的容量
  * @param[in]  src       源字符串
  *
  * @note     要转义的是热点名：它是用户上一轮自己填进来的，可能带尖括号或
  *           引号。不转义的话，一个叫 "<b>" 的热点就能把整个页面的结构冲掉。
  *
  * @note     余量不够时宁可截断也不越界 —— 但正常路径到不了截断：
  *           SSID 最多 32 字节，最坏全变成一个字符 6 字节的实体，也就 192。
  */
static void escape_html(char *dst, size_t dst_size, const char *src)
{
    size_t left = dst_size;

    /* left > 6 是留出"最长那个实体(6 字节) + 结尾的 '\0'" */
    while (*src != '\0' && left > 6) {
        const char *rep = NULL;
        size_t      n;

        switch (*src) {
        case '&':  rep = "&amp;";  break;
        case '<':  rep = "&lt;";   break;
        case '>':  rep = "&gt;";   break;
        case '"':  rep = "&quot;"; break;
        default:   break;
        }

        if (rep != NULL) {
            n = strlen(rep);
            memcpy(dst, rep, n);
        } else {
            dst[0] = *src;
            n = 1;
        }

        dst  += n;
        left -= n;
        src++;
    }

    *dst = '\0';
}

/**
  * @brief    把 POST 的表单正文整条读进缓冲区
  *
  * 🟡 L1 —— 架构：按 Content-Length 收满正文，缓冲区上限和读超时都在
             这几行里定；它一阻塞，整个 httpd 任务都得跟着等。
  *
  * @param[in]  req       请求
  * @param[out] buf       目标缓冲区
  * @param[in]  buf_size  buf 的容量
  *
  * @retval  >= 0  读到的字节数，buf 末尾已补 '\0'
  * @retval  -1    读失败，或者正文长度超过 buf_size - 1
  *
  * @note     要读多长是从 req->content_len 拿的，不是"读到读不动为止" ——
  *           HTTP 正文不保证以 '\0' 结尾，只能按长度收。
  *
  * @note     读超时不算错误，重来一次就行。这是 SDK 自带示例的处理方式
  *           （examples/protocols/http_server/simple/main/main.c）。
  */
static int recv_body(httpd_req_t *req, char *buf, size_t buf_size)
{
    size_t want = req->content_len;
    size_t got  = 0;

    if (want == 0 || want > buf_size - 1) {
        return -1;
    }

    while (got < want) {
        int n = httpd_req_recv(req, buf + got, want - got);

        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            return -1;      /* 0 = 对端关了连接；其余负值是真错误 */
        }
        got += n;
    }

    buf[got] = '\0';
    return (int)got;
}

/* ======================= 网页 ======================= */

/**
  * @brief 表单页的上半截，一直断在"现在连不上的是哪个热点"前面
  *
  * @note  断成两半是为了在中间插进那个热点名（它要转义，长度不定）。
  *        这样既不用把整页拼进一个缓冲区，也不用担心拼不下被截断。
  */
static const char PAGE_HEAD[] =
    "<!DOCTYPE html>\n"
    "<html lang=\"zh-CN\">\n"
    "<head>\n"
    "<meta charset=\"utf-8\">\n"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
    /* 一张 1x1 的全透明 GIF，直接内嵌。浏览器只要有页面就会顺手要
       /favicon.ico，而这个服务只注册了两个路由 —— 结果是串口里多两条
       无关的 404 警告。给一个内嵌图标，它就不来问了。 */
    "<link rel=\"icon\" href=\"data:image/gif;base64,"
    "R0lGODlhAQABAAAAACH5BAEKAAEALAAAAAABAAEAAAICTAEAOw==\">\n"
    "<title>ESP-01S 配网</title>\n"
    "<style>\n"
    "body{font-family:sans-serif;margin:0;padding:20px;background:#f4f4f4;color:#222}\n"
    "h1{font-size:20px;margin:0 0 10px}\n"
    ".now{color:#666;font-size:14px;line-height:1.6;margin:0 0 18px}\n"
    "form{background:#fff;padding:16px;border-radius:8px}\n"
    "label{display:block;font-size:14px;margin:0 0 6px}\n"
    "input{width:100%;box-sizing:border-box;font-size:16px;padding:10px;\n"
    "      margin:0 0 16px;border:1px solid #ccc;border-radius:6px}\n"
    "button{width:100%;font-size:17px;padding:12px;border:0;border-radius:6px;\n"
    "       background:#2a8f6e;color:#fff}\n"
    ".hint{color:#888;font-size:13px;line-height:1.6;margin:16px 0 0}\n"
    "</style>\n"
    "</head>\n"
    "<body>\n"
    "<h1>ESP-01S 配网</h1>\n"
    "<p class=\"now\">这块板子现在连不上 <b>";

/**
  * @brief 表单页的下半截，接在上面的热点名后面
  *
  * @note  两个输入框都写了 autocapitalize / autocorrect / spellcheck 关闭：
  *        iOS 默认会把输入框的第一个字母改成大写，而 SSID 【区分大小写】。
  *        不改的话，用户明明照着填对了，却永远连不上 —— 这是最难查的一类。
  *
  * @note  密码框【故意】不用 type="password"：手机上没法核对，打错一个字符
  *        就要再等两分钟才知道。这个热点只在配网那几分钟存在，看得见比藏着强。
  */
static const char PAGE_TAIL[] =
    "</b>。<br>填一个 2.4GHz 的热点，保存后它就去连那个。</p>\n"
    "<form method=\"POST\" action=\"/save\">\n"
    "<label>热点名（SSID）</label>\n"
    "<input name=\"ssid\" maxlength=\"32\" required\n"
    "       autocapitalize=\"off\" autocorrect=\"off\" autocomplete=\"off\"\n"
    "       spellcheck=\"false\">\n"
    "<label>密码</label>\n"
    "<input name=\"pass\" maxlength=\"64\"\n"
    "       autocapitalize=\"off\" autocorrect=\"off\" autocomplete=\"off\"\n"
    "       spellcheck=\"false\">\n"
    "<button type=\"submit\">保存并连接</button>\n"
    "</form>\n"
    /* 汉字这条提示是有必要的：输入框的 maxlength 数的是【字符】，
       而 802.11 的上限 32 数的是【字节】。一个汉字在 UTF-8 里占 3 字节，
       所以中文热点名最多 10 个字 —— 第 11 个字会在网页上被拦下来。 */
    "<p class=\"hint\">只能连 2.4GHz —— ESP8266 扫不到 5GHz，名字写对了也连不上。<br>\n"
    "一个汉字占 3 个字节，中文热点名最多 10 个字。<br>\n"
    "密码留空表示对方是开放热点。</p>\n"
    "</body>\n"
    "</html>\n";

/**
  * @brief    回一个一两句话的说明页
  *
  * 🟢 L2 —— 工具：拼一条状态提示页发出去。
  *
  * @param[in] req    请求
  * @param[in] status HTTP 状态行，用 SDK 的 HTTPD_200 / HTTPD_400 那些宏
  * @param[in] title  标题，也就是正文
  * @param[in] tail   正文后面那一段，允许是编译期的 HTML 字面量
  *
  * @note     title 和 tail 只传编译期的字面量，不传用户填的东西 ——
  *           所以这里【不需要】转义。要显示用户填的内容，先过 escape_html()。
  */
static esp_err_t send_notice(httpd_req_t *req, const char *status,
                             const char *title, const char *tail)
{
    /* static 而不是栈上：这条任务的栈只有几 KB，而这个页面不算小。
       整条 httpd 是【一条任务顺序处理所有请求】的，所以一份 static
       缓冲区不会两边打架。

       704 是量出来的，不是拍的：模板（含上面那个内嵌 favicon）固定占 389 字节，
       最长的两条提示是"密码长度不对"和"保存成功"，整页分别 501 / 507 字节。
       留 200 字节余量给以后改文案。下面那个检查会在真装不下时报警。 */
    static char page[704];
    int n = snprintf(page, sizeof(page),
                     "<!DOCTYPE html>\n<html lang=\"zh-CN\">\n<head>\n"
                     "<meta charset=\"utf-8\">\n"
                     "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
                     /* 和 PAGE_HEAD 里那个同一个理由：不让浏览器来要 favicon */
                     "<link rel=\"icon\" href=\"data:image/gif;base64,"
                     "R0lGODlhAQABAAAAACH5BAEKAAEALAAAAAABAAEAAAICTAEAOw==\">\n"
                     "<title>ESP-01S 配网</title>\n</head>\n"
                     "<body style=\"font-family:sans-serif;padding:20px;line-height:1.6\">\n"
                     "<h1 style=\"font-size:18px\">%s</h1>\n"
                     "%s\n"
                     "</body>\n</html>\n",
                     title, tail);

    if (n < 0 || (size_t)n >= sizeof(page)) {
        /* 只有模板改长了、缓冲区没跟着改才会走到这里。
           宁可回一个空页面，也不发一个被截断的页面出去。 */
        printf("[prov] ✗ 提示页装不下（要 %d 字节），检查 send_notice() 的缓冲区\n", n);
        httpd_resp_set_status(req, HTTPD_500);
        return httpd_resp_send(req, NULL, 0);
    }

    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, page, n);
}

/**
  * @brief  表单页：GET /
  *
  * 🟢 L2 —— 工具：分三段发出表单页，中间插当前 SSID（已转义）。
  *
  * @note   分三段发（前半截、转义过的热点名、后半截），见 PAGE_HEAD 的注释。
  *
  * @warning 三个 httpd_resp_send_chunk 之后【必须】再发一个长度 0 的收尾，
  *          否则浏览器会一直等剩下的数据，页面永远转圈 —— 而固件这边
  *          看上去一切正常。
  */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    char now[256];

    escape_html(now, sizeof(now), wifi_sta_get_ssid());

    if (httpd_resp_set_type(req, "text/html; charset=utf-8") != ESP_OK ||
        httpd_resp_send_chunk(req, PAGE_HEAD, -1) != ESP_OK ||
        httpd_resp_send_chunk(req, now,       -1) != ESP_OK ||
        httpd_resp_send_chunk(req, PAGE_TAIL, -1) != ESP_OK) {
        return ESP_FAIL;
    }

    return httpd_resp_send_chunk(req, NULL, 0);
}

/**
  * @brief  收凭据：POST /save
  *
  * 🟡 L1 —— 架构：必须先发回执、再置配网完成位。反过来的话 ap_prov_run()
             会抢先把网页服务停掉，浏览器只看到连接被掐断。
  *
  * @note   正文是表单默认的 application/x-www-form-urlencoded，形如
  *         "ssid=xxx&pass=yyy"，两个字段都做过了转义。所以顺序是：
  *         先按长度收下来，再把两个值切出来，最后逐个还原转义。
  *
 * @note   两个字段先切进【本函数栈上的】raw 缓冲区，解码、校验都过了之后才
 *         拷进模块自己的 s_new_ssid / s_new_pass。后两个不能直接拿来收转义串
 *         —— 理由见 PROV_RAW_SSID_SIZE：拿"解码后"的容量去装"解码前"的串，
 *         一个中文热点名填到第四个字就会被拒。
 *
  * @warning 校验【必须】在立牌子之前做完。中途任何一条 return 都要带个错误页
  *          出去，不能悄悄返回 ESP_FAIL —— 那在手机上只表现为"页面打不开"，
  *          用户唯一能做的就是再点一次保存。
  */
static esp_err_t save_post_handler(httpd_req_t *req)
{
    char      body[PROV_BODY_MAX];
    /* 装【转义形式】的两个字段。栈上一共 384+100+196 字节，只在点"保存"那一
       瞬间占着；httpd 任务默认的栈是 4 KB，够。 */
    char      raw_ssid[PROV_RAW_SSID_SIZE];
    char      raw_pass[PROV_RAW_PASS_SIZE];
    esp_err_t err;

    if (recv_body(req, body, sizeof(body)) < 0) {
        printf("[prov] 表单正文读不出来（content_len=%d，上限 %d）\n",
               (int)req->content_len, PROV_BODY_MAX - 1);
        return send_notice(req, HTTPD_400, "表单没读全",
                           "<p><a href=\"/\">返回重填</a></p>");
    }

    /* ⚠ 这两个缓冲区里装的是【转义形式】，不是 SSID 和密码本身 —— 一个汉字
       在这里占 9 个字符（"%E8%B4%A5"）。所以容量必须按三倍算，不能拿
       s_new_ssid / s_new_pass（33 / 65 字节）来接：那样 4 个汉字（36 个字符）
       就会被判"太长"，而它解码后其实只有 12 字节。 */
    if (httpd_query_key_value(body, "ssid", raw_ssid, sizeof(raw_ssid)) != ESP_OK) {
        printf("[prov] 表单里的 ssid 字段不对（缺了，或者转义后超过 %d 字符）\n",
               PROV_RAW_SSID_SIZE - 1);
        return send_notice(req, HTTPD_400, "热点名没填或者太长",
                           "<p>一个汉字要占 3 个字节，最多 10 个汉字。</p>\n"
                           "<p><a href=\"/\">返回重填</a></p>");
    }
    if (httpd_query_key_value(body, "pass", raw_pass, sizeof(raw_pass)) != ESP_OK) {
        printf("[prov] 表单里的 pass 字段不对（转义后超过 %d 字符）\n",
               PROV_RAW_PASS_SIZE - 1);
        return send_notice(req, HTTPD_400, "密码太长",
                           "<p>上限 64 字节。</p>\n"
                           "<p><a href=\"/\">返回重填</a></p>");
    }

    /* 还原之后才是真正的 SSID / 密码：三字节变一字节，串只会变短。
       下面的长度判断【必须】放在这一步之后 —— 放在之前量的是转义串的长度，
       那就是"四个汉字的中文热点名填不进去"那个毛病。 */
    url_decode(raw_ssid);
    url_decode(raw_pass);

    /* 长度按【字节】算，不是按字符。手机输入框的 maxlength 数的是字符，
       拦不住 11 个汉字（33 字节）这种超长的 SSID —— 汉字在 UTF-8 里占 3 字节。
       在这里拦下来，用户当场就能看见错在哪；放过去的话，页面会说"保存成功"，
       然后干等两分钟又被踢回这个页面。 */
    if (raw_ssid[0] == '\0' || strlen(raw_ssid) > PROV_SSID_MAX) {
        printf("[prov] SSID 长度不合法：%u 字节（要求 1~%d）\n",
               (unsigned)strlen(raw_ssid), PROV_SSID_MAX);
        return send_notice(req, HTTPD_400, "热点名长度不对",
                           "<p>要求 1~32 字节。一个汉字算 3 字节。</p>\n"
                           "<p><a href=\"/\">返回重填</a></p>");
    }
    if (strlen(raw_pass) > PROV_PASS_MAX) {
        printf("[prov] 密码长度不合法：%u 字节（上限 %d）\n",
               (unsigned)strlen(raw_pass), PROV_PASS_MAX);
        return send_notice(req, HTTPD_400, "密码太长",
                           "<p>上限 64 字节。</p>\n"
                           "<p><a href=\"/\">返回重填</a></p>");
    }

    /* 到这儿两个值才第一次"确定是合法的"，可以进模块自己的缓冲区了 ——
       那两个的容量正好是协议上限加 '\0'，装不下就说明上面漏了一道检查。 */
    copy_str(s_new_ssid, sizeof(s_new_ssid), raw_ssid);
    copy_str(s_new_pass, sizeof(s_new_pass), raw_pass);

    printf("[prov] 收到：SSID=\"%s\"，密码 %u 字节\n",
           s_new_ssid, (unsigned)strlen(s_new_pass));

    /* 先把回执发出去，再立牌子。反过来的话，ap_prov_run() 那边可能已经开始
       收摊了，而这个 socket 还没写出去一个字节 —— 用户看到的就是"保存不了"。 */
    err = send_notice(req, HTTPD_200, "保存成功",
                      "<p>这块板子这就去连它。配网热点马上会关掉，"
                      "手机回到原来那个 WiFi 就行。</p>");

    xEventGroupSetBits(s_prov_group, PROV_DONE_BIT);
    return err;
}

/* ======================= 起热点和网页 ======================= */

/**
  * @brief  把 WiFi 切成 AP 模式、起热点、起 HTTP 服务
  *
  * 🟡 L1 —— 架构：停射频→切 AP 模式→下发配置→起射频→起网页服务，
             这个顺序换不得（和 wifi_sta_init() 是同一套路）。
  *
  * @retval   true   都起来了，可以等人来填了
  * @retval   false  中途失败。已经拉起来的部分【不在这里拆】，由 ap_prov_run()
  *                  统一收口 —— 拆的动作只有一处，就不会出现两条路各拆一半。
  */
static bool start_ap_and_web(void)
{
    wifi_config_t  ap;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    uint8_t        mac[6] = { 0 };
    httpd_uri_t    root = {
        .uri = "/", .method = HTTP_GET, .handler = root_get_handler
    };
    httpd_uri_t    save = {
        .uri = "/save", .method = HTTP_POST, .handler = save_post_handler
    };

    /* 顺序是照抄上电那条已经跑通的：先停，再声明模式，再灌配置，最后启动。
       能不能省掉 stop 直接 set_mode，本 SDK 没有文档 —— esp_wifi_set_mode()
       实现在闭源库里，头文件只写了参数和返回值。既然照抄就不用赌。 */
    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_AP);

    memset(&ap, 0, sizeof(ap));

    /* 热点名带 MAC 最后两字节，理由见 AP_SSID_PREFIX。取不到 MAC 就退回一个
       固定名字 —— 名字重复总比名字里出现栈上的垃圾字符强。 */
    if (esp_wifi_get_mac(ESP_IF_WIFI_AP, mac) == ESP_OK) {
        snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%s-%02X%02X",
                 AP_SSID_PREFIX, mac[4], mac[5]);
    } else {
        snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%s", AP_SSID_PREFIX);
    }
    ap.ap.ssid_len = (uint8_t)strlen((char *)ap.ap.ssid);

    copy_str((char *)ap.ap.password, sizeof(ap.ap.password), AP_PASSWORD);
    ap.ap.channel        = AP_CHANNEL;
    ap.ap.authmode       = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = AP_MAX_CONN;

    if (esp_wifi_set_config(ESP_IF_WIFI_AP, &ap) != ESP_OK) {
        printf("[prov] ✗ AP 配置写不进去\n");
        return false;
    }

    /* 这一句是必须的：上面那个 esp_wifi_stop() 把 WiFi 整个关了，只配置不启动
       的话，热点只是"存在于某个结构体里"，一个信标都不会发出去。

       启动之后 WIFI_EVENT_AP_START 会让 tcpip_adapter 把 AP 那张网卡拉起来，
       顺手把 DHCP 服务也开起来（components/tcpip_adapter/event_handlers.c 的
       handle_ap_start()），所以这里【不用】再手动调 tcpip_adapter_dhcps_start()。
       IP 是 tcpip_adapter_init() 里定死的 192.168.4.1，不是 DHCP 分来的。 */
    if (esp_wifi_start() != ESP_OK) {
        printf("[prov] ✗ 热点起不来\n");
        return false;
    }

    /* 就一个人拿手机来填，3 条连接足够（浏览器开一个页面会同时占好几条，
       所以不能只留 1 条）。少留几条纯粹是为了省内存 —— 这块板子总共没多少。 */
    cfg.max_open_sockets = 3;

    /* 一共就注册下面两个路由。默认是 8，那 8 份槽位白占内存。 */
    cfg.max_uri_handlers = 2;

    /* 这一条是【必须开的】，不是优化：手机打开页面时会同时开好几条连接，
       而且经常不主动关。默认情况下这个开关是关的，槽位一旦占满，新连接会被
       直接拒掉 —— 表现是"热点明明连上了，页面就是打不开"，而且刷新多少次
       都没用。打开之后服务端会把最久没用过的那条踢掉腾位置。 */
    cfg.lru_purge_enable = true;

    /* ⚠ 请求头缓冲区【不在这里调】。上面这一串 cfg.xxx 里找不到它，因为
       httpd_config_t 压根没有这个字段 —— 上限是编译期的，来自 Kconfig：

           CONFIG_HTTPD_MAX_REQ_HDR_LEN   （sdkconfig，默认 512）

       httpd_start() 按 HTTPD_SCRATCH_BUF = MAX(这个值, MAX_URI_LEN) 一次性
       calloc 出解析用的 scratch，整个服务只此一份，不是每条连接一份
       （esp_httpd_priv.h:39，结构体在 :139，分配在 httpd_main.c:313）。

       512 装不下一部现代手机的 POST 请求头 —— 用电脑 curl 试可能没事，
       手机浏览器会带上一大堆 Cookie / UA / Sec-Fetch-*，一发就超。
       症状是网页上写着 "Header fields are too long for server to interpret"，
       串口里是：
           parse_block: response uri/header too big      （SDK 自己的笔误，说的
                                                          其实是 request）
           431 Request Header Fields Too Large
       本工程把它调到了 2048（sdkconfig:170）。代价是 httpd 启动时多占
       1.5 KB 堆，而且只在配网这几分钟里占着，配网一结束就连同服务一起释放。
       改这个值必须重新编译 —— 它是编译进代码里的常量，不是运行时可调的。 */
    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        printf("[prov] ✗ HTTP 服务起不来（多半是内存不够）\n");
        return false;
    }

    if (httpd_register_uri_handler(s_server, &root) != ESP_OK ||
        httpd_register_uri_handler(s_server, &save) != ESP_OK) {
        printf("[prov] ✗ 网页路由注册失败\n");
        return false;
    }

    printf("\n[prov] ========== 进入配网模式 ==========\n");
    printf("[prov] 热点  : %s\n", (char *)ap.ap.ssid);
    printf("[prov] 密码  : %s\n", AP_PASSWORD);
    printf("[prov] 手机连上后打开 http://192.168.4.1/\n");
    /* 把余量打出来，是为了让"内存够不够"这件事【可测量】而不是靠猜。
       这个数是 STA 已经停掉、热点和网页都起来之后的剩余堆；配网全程
       真正紧张的只有这一刻。要是它变成很小的数，上面那个 2048 就该回调。 */
    printf("[prov] 剩余内存: %u 字节\n", (unsigned)esp_get_free_heap_size());
    printf("[prov] ================================\n\n");
    return true;
}

/* ======================= 对外接口 ======================= */
/* 接口契约（参数范围、返回值、返回时 WiFi 处在什么状态）都在 ap_prov.h 里，
   这里只记实现顺序上不能动的地方。 */

bool ap_prov_run(char *ssid_out, size_t ssid_size,
                 char *pass_out, size_t pass_size)
{
    bool ok = false;

    if (ssid_out == NULL || pass_out == NULL ||
        ssid_size == 0 || pass_size == 0) {
        return false;
    }

    s_prov_group = xEventGroupCreate();
    if (s_prov_group == NULL) {
        printf("[prov] ✗ 事件组建不出来\n");
        return false;
    }

    /* 先把上一轮的残留抹掉，免得失败路径上调用方读到过期的名字 */
    s_new_ssid[0] = '\0';
    s_new_pass[0] = '\0';

    if (start_ap_and_web()) {
        /* 整段流程就阻塞在这一行。portMAX_DELAY = 没人来就一直等 ——
           配网模式本来就是这个意思：有个人拿着手机过来，它才有下文。 */
        xEventGroupWaitBits(s_prov_group, PROV_DONE_BIT,
                            pdFALSE,        /* 不等完就清牌子，没有下次了 */
                            pdTRUE,         /* 就等这一个位 */
                            portMAX_DELAY);
        ok = true;
    }

    /* 收摊。成没成都走这里，所以拆的动作只有一处。

       顺序不能反：先停网页、再停热点。反过来的话，网页还活着的那一小会儿
       它的网卡已经没了，那条 socket 会以什么方式结束就不好说了。 */
    if (s_server != NULL) {
        if (ok) {
            /* 等 1.5 秒，两个作用：
               ① 让手机把"保存成功"那个页面画出来 —— 下面一停热点，
                  这条连接就断了；
               ② 上面立的牌子只保证 POST 处理函数【写完了回执】，
                  它还要再返回一步；服务端在它返回之前就被拆掉的话，
                  收尾的动作会踩在一个正在退出的会话上。 */
            vTaskDelay(pdMS_TO_TICKS(1500));
        }
        httpd_stop(s_server);
        s_server = NULL;
    }

    esp_wifi_stop();

    /* 模式也在这里切回 STA，不留给调用方：esp_wifi_set_mode() 在"已经停了"
       的状态下调用语义是明确的，而"已经停了"正是上面那一句刚做完的事。 */
    esp_wifi_set_mode(WIFI_MODE_STA);

    if (ok) {
        copy_str(ssid_out, ssid_size, s_new_ssid);
        copy_str(pass_out, pass_size, s_new_pass);
        printf("[prov] 收工 —— 新热点 \"%s\"\n", s_new_ssid);
    } else {
        printf("[prov] 没起来，退回 STA 接着重试\n");
    }

    vEventGroupDelete(s_prov_group);
    s_prov_group = NULL;

    return ok;
}
