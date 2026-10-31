# -*- coding: utf-8 -*-
# 给 main/ 里每个函数的文档块插一行风险等级（🔴 L0 / 🟡 L1 / 🟢 L2）。
# 公开函数的块在 .h，static 函数的块在 .c —— 插在 @brief 那段之后。
#   python _tag.py --dry   只打印「插在哪、插什么」，不动文件
#   python _tag.py         就地插
import io
import os
import sys

DRY = "--dry" in sys.argv

# (文件, 函数名) -> (等级, [理由行...])，首行带等级
TAG = {
    ("relay.h", "relay_init"): ("🔴 L0", [
        "生死线：这句一开驱动器，GPIO0 就按输出寄存器里的复位值（0）",
        "开始输出 —— \"上电为关\"是设计，不是巧合。"]),
    ("relay.h", "relay_on"): ("🔴 L0", [
        "生死线：把 GPIO0 拉离危险电平。任何要加重启的地方都",
        "必须先调它。"]),
    ("relay.h", "relay_off"): ("🔴 L0", [
        "生死线：执行完这根线就是低的 —— 此刻复位，芯片直接进",
        "UART 下载模式，程序根本不跑。"]),

    ("link.h", "link_init"): ("🟡 L1", [
        "架构：开机默认走哪条链路，全工程只有这一处；调它之前两个接收",
        "回调必须都已注册 —— 它一返回链路就开了。"]),
    ("link.h", "link_current"): ("🟢 L2", [
        "工具：纯查询，读一个 static 变量，黑盒测即可。"]),
    ("link.h", "link_send"): ("🟢 L2", [
        "工具：按当前链路选路转发。规矩是\"所有回执都走它\"，",
        "但函数本身只有几行。"]),
    ("link.h", "link_switch"): ("🟡 L1", [
        "架构：先用旧链路发回执、再切，反过来对方就永远等不到回复；",
        "而且只能置标志、不能等任务退出（会在自己的栈上死锁）。"]),

    ("cmd.h", "cmd_on_tcp_rx"): ("🟢 L2", [
        "工具：薄包装，判完来源就转给同一份解析逻辑。"]),
    ("cmd.h", "cmd_on_udp_rx"): ("🟢 L2", [
        "工具：薄包装，和 cmd_on_tcp_rx() 只差来源判断。"]),

    ("ap_prov.h", "ap_prov_run"): ("🟡 L1", [
        "架构：整段兜底流程（停射频→起热点→等人填表→收摊），",
        "阻塞几分钟；返回时 WiFi 一定停着且是 STA 模式。"]),

    ("wifi_sta.h", "wifi_sta_init"): ("🟡 L1", [
        "架构：注册事件回调 + 起管理任务，6144 那个栈大小在这里定；",
        "异步，返回时还没连上。"]),
    ("wifi_sta.h", "wifi_sta_wait_ip"): ("🟡 L1", [
        "架构：\"真的连上了\"的唯一标志；不清事件位，所以要照顾",
        "同时在等的其它任务。"]),
    ("wifi_sta.h", "wifi_sta_set_credentials"): ("🟡 L1", [
        "架构：先存 NVS 再重连，顺序和字节长度检查都不能省；",
        "失败时当前连接不受影响。"]),
    ("wifi_sta.h", "wifi_sta_get_ssid"): ("🟢 L2", [
        "工具：返回内部静态缓冲区，别 free、别长期保存。"]),
    ("wifi_sta.h", "wifi_sta_set_giveup_handler"): ("🟢 L2", [
        "工具：存下一个函数指针，什么时候用它由重试计数决定。"]),

    ("tcp_client.h", "tcp_client_set_rx_handler"): ("🟢 L2", [
        "工具：一次赋值。\"必须在 start() 之前调\"是调用方的规矩。"]),
    ("tcp_client.h", "tcp_client_start"): ("🟡 L1", [
        "架构：任务只建一次，重复调等于\"确保它开着\"；内部先等",
        "IP 再连，连不上每 2 秒重来。"]),
    ("tcp_client.h", "tcp_client_stop"): ("🟡 L1", [
        "架构：不阻塞、不等任务退出，代价是最长 5 秒的窗口里旧",
        "连接还在收 —— 挡包是 cmd 那边的事。"]),
    ("tcp_client.h", "tcp_client_send"): ("🟡 L1", [
        "架构：没连上就直接丢并打日志，不等 —— 拿\"发送可能静默",
        "失败\"换\"不拖住命令解析\"。"]),

    ("udp_client.h", "udp_client_set_rx_handler"): ("🟢 L2", [
        "工具：一次赋值。\"必须在 start() 之前调\"是调用方的规矩。"]),
    ("udp_client.h", "udp_client_start"): ("🟡 L1", [
        "架构：任务只建一次；内部先等 IP 再 bind，bind 失败每",
        "2 秒重试（比如端口被占）。"]),
    ("udp_client.h", "udp_client_stop"): ("🟡 L1", [
        "架构：不阻塞、不等任务退出，真正关 socket 要等 recvfrom()",
        "那一轮超时（最多 5 秒）。"]),
    ("udp_client.h", "udp_client_send"): ("🟢 L2", [
        "工具：往写死的地址 sendto。地址为什么写死，见文件头。"]),

    # ---- 静态函数：文档块在 .c 里 ----
    ("ap_prov.c", "copy_str"): ("🟢 L2", [
        "工具：定长拷贝，超长截断并补 \\0。"]),
    ("ap_prov.c", "hex_val"): ("🟢 L2", [
        "工具：一个字符换个数字，非法返回 -1。"]),
    ("ap_prov.c", "url_decode"): ("🟢 L2", [
        "工具：表单解码，就地把 %XX 和 + 还原。"]),
    ("ap_prov.c", "escape_html"): ("🟢 L2", [
        "工具：四个字符换实体 —— 防的是把 SSID 里的尖括号当标签用。"]),
    ("ap_prov.c", "recv_body"): ("🟡 L1", [
        "架构：按 Content-Length 收满正文，缓冲区上限和读超时都在",
        "这几行里定；它一阻塞，整个 httpd 任务都得跟着等。"]),
    ("ap_prov.c", "send_notice"): ("🟢 L2", [
        "工具：拼一条状态提示页发出去。"]),
    ("ap_prov.c", "root_get_handler"): ("🟢 L2", [
        "工具：分三段发出表单页，中间插当前 SSID（已转义）。"]),
    ("ap_prov.c", "save_post_handler"): ("🟡 L1", [
        "架构：必须先发回执、再置配网完成位。反过来的话 ap_prov_run()",
        "会抢先把网页服务停掉，浏览器只看到连接被掐断。"]),
    ("ap_prov.c", "start_ap_and_web"): ("🟡 L1", [
        "架构：停射频→切 AP 模式→下发配置→起射频→起网页服务，",
        "这个顺序换不得（和 wifi_sta_init() 是同一套路）。"]),

    ("cmd.c", "cmd_do_wifi"): ("🟢 L2", [
        "工具：切出逗号两边的 SSID 和密码。真正的坑（半条命令不能",
        "动执行表）在 cmd_try_one() 那一段。"]),
    ("cmd.c", "cmd_try_one"): ("🟡 L1", [
        "架构：挑缓冲区里位置最靠前的那条命令执行。抠掉命令和清",
        "缓冲区的先后、半条 wifi 行不许动执行表，都是这里定的。"]),
    ("cmd.c", "dump_hex"): ("🟢 L2", [
        "工具：调试用，最多打 16 个字节。"]),
    ("cmd.c", "cmd_on_rx"): ("🟡 L1", [
        "架构：追加新字节、循环取出所有完整命令。半条命令要留在",
        "缓冲区里等下一包 —— 缓冲区多大、留多久，是这里定的。"]),
    ("cmd.c", "cmd_on_rx_from"): ("🟢 L2", [
        "工具：不是当前链路来的直接丢，是就转 cmd_on_rx()。"]),

    ("main.c", "print_chip_info"): ("🟢 L2", [
        "工具：开机打几行芯片信息。"]),
    ("main.c", "app_main"): ("🟡 L1", [
        "架构：全工程的开机顺序就是这几行 —— 先 relay_init()",
        "（从此刻起继电器才可控），再 WiFi，再注册回调和兜底，",
        "最后 link_init()。"]),

    ("tcp_client.c", "tcp_client_task"): ("🟡 L1", [
        "架构：等 IP→连接→recv→交回调→断了重连，永不退出。",
        "s_sock 必须先置 -1 再 close，否则别的任务会往已关的 fd 写。"]),

    ("udp_client.c", "udp_client_task"): ("🟡 L1", [
        "架构：等 IP→bind→加入组播组→recvfrom→交回调，永不退出；",
        "recvfrom() 的超时值只决定 stop 之后多久真正关 socket。",
        "30 秒周期的状态上报已交给 cmd.c 的定时器任务，本模块不再发心跳。"]),

    ("wifi_sta.c", "copy_str"): ("🟢 L2", [
        "工具：定长拷贝，超长截断并补 \\0。"]),
    ("wifi_sta.c", "nvs_load_credentials"): ("🟡 L1", [
        "架构：从 NVS 读凭据；读不到（第一次上电）就退回编译进去",
        "的默认值 —— 所以第一次开机不配网也能连上。"]),
    ("wifi_sta.c", "nvs_save_credentials"): ("🟡 L1", [
        "架构：写 NVS 再 commit；断电之后凭据还在不在，就看这一步。"]),
    ("wifi_sta.c", "apply_credentials"): ("🟡 L1", [
        "架构：把 s_ssid/s_pass 灌进 wifi_config_t 下发。只发配置",
        "不发起连接 —— threshold.authmode 那行也在这里，它就是从机",
        "连不上开放式热点的原因。"]),
    ("wifi_sta.c", "wifi_event_handler"): ("🟡 L1", [
        "架构：STA 启动/断开/拿到 IP 三个分支；重试计数和给别的任务",
        "等的事件位都在这里改。"]),
    ("wifi_sta.c", "sta_restart_from_prov"): ("🟡 L1", [
        "架构：配网回来这一趟 —— 先存新凭据（存失败就放弃），再切回",
        "STA、重下配置、起射频，最后交给事件回调去连。"]),
    ("wifi_sta.c", "wifi_mgr_task"): ("🟡 L1", [
        "架构：没连上就定时上报，失败满 WIFI_RETRY_BEFORE_AP 次就",
        "叫 ap_prov_run()。等 IP 时只能用 pdFALSE —— 清了事件位，",
        "wifi_sta_wait_ip() 就永远等不到。"]),
}


def docblock(lines, idx):
    """lines[idx] 是函数定义/原型那一行；返回 (起, 止, brief 行) 或 None。"""
    end = idx - 1
    while end >= 0 and not lines[end].strip():
        end -= 1
    if end < 0 or not lines[end].rstrip().endswith("*/"):
        return None
    start = end
    while start >= 0 and "/**" not in lines[start]:
        start -= 1
    if start < 0:
        return None
    brief = None
    for i in range(start, end + 1):
        if "@brief" in lines[i]:
            brief = i
            break
    return start, end, brief


def find(lines, name):
    for i, s in enumerate(lines):
        t = s.strip()
        if not t.startswith(("static ", "void ", "bool ", "int ", "const ", "link_mode_t ",
                             "esp_err_t ", "uint32_t ", "size_t ", "char ")):
            continue
        seg = t.split("(")[0]
        if not seg.endswith(name):
            continue
        # 定义或原型：往后六行里出现 { 或 );
        tail = " ".join(x.strip() for x in lines[i:i + 6])
        if "{" in tail or ");" in tail:
            return i
    return None


out = io.open("_tag_out.txt", "w", encoding="utf-8")
for (fn, name), (lvl, why) in sorted(TAG.items()):
    text = io.open(fn, encoding="utf-8").read()
    lines = text.split("\n")
    idx = find(lines, name)
    if idx is None:
        out.write("!! %s: 找不到 %s\n" % (fn, name))
        continue
    db = docblock(lines, idx)
    if db is None:
        out.write("!! %s:%d %s 上面没有 /** */ 块\n" % (fn, idx + 1, name))
        continue
    start, end, brief = db
    if brief is None:
        out.write("!! %s:%d %s 的块里没有 @brief\n" % (fn, idx + 1, name))
        continue

    # `*` 那几行缩进多少；正文从 `* ` 之后开始对齐
    star_ind = len(lines[brief]) - len(lines[brief].lstrip())
    star = " " * star_ind + "* "
    cont = " " * (len(star) + len(lvl) + 5)     # +5：emoji 在编辑器里占两格
    blank = " " * star_ind + "*"
    body_lines = [star + lvl + " —— " + why[0]] + [cont + c for c in why[1:]]

    if start == end:                      # /** @brief xxx */ 挤成一行，先摊开
        old = lines[start]
        tail = old[old.rindex("*/") + 2:].rstrip()
        if tail:
            out.write("!! %s:%d %s 的单行块后面还有别的字符\n" % (fn, start + 1, name))
            continue
        body = old[old.index("@brief"):old.rindex("*/")].rstrip()
        open_ind = len(old) - len(old.lstrip())
        star_ind = open_ind + 2
        star = " " * star_ind + "* "
        cont = " " * (len(star) + len(lvl) + 5)
        blank = " " * star_ind + "*"
        new = [" " * open_ind + "/**", star + body, blank]
        new += [star + lvl + " —— " + why[0]] + [cont + c for c in why[1:]]
        new.append(" " * star_ind + "*/")
        if DRY:
            out.write("%s:%d %s  【单行块展开】\n" % (fn, idx + 1, name))
            for l in new:
                out.write("     + %s\n" % l)
        else:
            lines[start:start + 1] = new
            io.open(fn, "w", encoding="utf-8").write("\n".join(lines))
        continue

    # 插在 brief 那一段之后：往后找到第一个空 `*` 行、下一个 @ 命令、或块尾
    ins = brief + 1
    while ins < end:
        t = lines[ins].strip()
        if t == "*" or t.startswith("* @"):
            break
        ins += 1
    ins_lines = body_lines
    if lines[ins].strip() == "*":          # brief 后面本来就有空行：插到它后面
        pos = ins + 1
        if pos <= end and lines[pos].strip().startswith("* @"):
            ins_lines = ins_lines + [blank]
    else:                                  # 紧跟着 @ 命令：前后都补空行
        pos = ins
        ins_lines = [blank] + ins_lines + [blank]
    if DRY:
        out.write("%s:%d  %s  [块 %d-%d]\n" % (fn, idx + 1, name, start + 1, end + 1))
        for k in range(max(start, pos - 2), min(end + 1, pos + 3)):
            out.write("   %s%4d| %s\n" % ("=>" if k == pos else "  ", k + 1, lines[k]))
        for l in ins_lines:
            out.write("     + %s\n" % l)
    else:
        lines[pos:pos] = ins_lines
        io.open(fn, "w", encoding="utf-8").write("\n".join(lines))
out.close()
print("done")
