# ESP-01S 远程继电器

## 简介

一个跑在 ESP-01S（ESP8266）上的**从节点固件**：上电后连到主节点 ESP32 开的
SoftAP，通过 TCP 或 UDP 接收命令，控制继电器吸合 / 释放。

节点不等着被"找到"—— 它主动连出去，所以主节点的地址是编译期写死的常量，
运行期不用谁去查 IP。

## 功能特性

- **双链路，运行期可切**：UDP（默认，灯控走组播 `239.0.0.2:8889`）和 TCP 二选一，
  `net tcp` / `net udp` 两条命令随时切换；同一时刻只有一条在收。
  UDP 模式下还额外按灯控协议规范，每 30 秒往组播组回一条 `status` 状态。
- **中文命令，两种编码都认**：`开灯` / `关灯` 的 UTF-8 和 GBK 字节都能匹配，
  另外提供纯 ASCII 的 `on` / `off`。
- **凭据存在 NVS**：热点名和密码写在 NVS 里，发一条 `wifi <SSID>,<密码>`
  就能换热点，**不用重新烧录**。
- **断线自动重连**：WiFi 掉了自己重连，连上后把 IP 打到串口。
- **连不上会自己开热点**：连续 30 次重连失败后进**配网模式** —— 开一个热点，
  手机连上去打开网页就能重填要连的 WiFi 和密码，**不用拆下来重烧**。
- **上电默认关**：继电器接在 GPIO0，上电是释放状态，给高电平才吸合。

## 技术栈

| 类别 | 用的什么 |
| --- | --- |
| 芯片 | ESP-01S（ESP8266，**1MB** flash） |
| 主节点 | ESP32，SoftAP + TCP Server（另一个工程，不在本仓库） |
| SDK | ESP8266_RTOS_SDK **v3.4**（`v3.4-115-g858c7c2e`，最终版，已冻结） |
| 构建 | `make`（SDK 自带流程）/ CMake（VSCode 插件） |
| 工具链 | `xtensa-lx106-elf` 交叉编译器 + MSYS2 `mingw32` 环境 |

## 快速开始

1. **环境要求**

   - Windows；
   - `xtensa-lx106-elf/` 和 `msys32/` —— 两个解压即用的绿色包，**在仓库之外**，
     放在上一级目录 `esp8266_toochain/` 下；
   - **32 位** Python（`Python313-32`），且 `setuptools` 必须锁在 `<71`。

2. **安装步骤**

   解压工具链，然后在 `mingw32.exe` 里导出环境变量 —— `IDF_PATH` 和 `PATH`
   在每个新开的 shell 里都要重新导一遍（`mingw32.exe` 不继承 Windows 的环境变量）。
   只有这个 shell 能编译本工程，Git Bash / PowerShell 里没有工具链。

3. **运行示例**

   ```bash
   make menuconfig                           # 串口在这里设，填 //./COM8
   make -j10                                 # 编译
   make ESPPORT=COM8 ESPBAUD=921600 flash    # 烧录
   make monitor                              # 串口监视器，Ctrl+] 退出
   ```

   监视器波特率是 **74880**（ROM bootloader 的固定输出），和烧录波特率是两回事。
   硬件上：**下载模式** GPIO0 接地，**运行模式** GPIO0 悬空。

   > 上面每一条的来龙去脉（为什么是这两个波特率、环境变量导什么、
   > 启动日志怎么读、分区表怎么改）都在
   > [学习笔记/ESP8266开发流程.md](学习笔记/ESP8266开发流程.md) 里。

## 项目结构

| 路径 | 说明 |
| --- | --- |
| `main/` | 固件源码，按职责分成几个模块：`relay.c` 继电器、`cmd.c` 命令解析、`link.c` 链路选择、`wifi_sta.c` 联网、`ap_prov.c` 配网兜底（连不上时开热点出网页）、`tcp_client.c` / `udp_client.c` / `mqtt_link.c` 传输，`main.c` 只负责把它们接起来 |
| `学习笔记/` | 中文笔记：[开发流程](学习笔记/ESP8266开发流程.md)、[NVS](学习笔记/ESP8266-NVS.md)、[分区表](学习笔记/ESP8266分区表.md)、[TCP/UDP/WiFi-STA](学习笔记/ESP8266-TCP-UDP-WiFi-STA.md)、[配网踩坑](学习笔记/ESP8266配网踩坑(SmartConfig).md) |
| `partitions_1mb.csv` | 分区表（**必须保持纯 ASCII**） |
| `Doxyfile` | Doxygen 配置，用来渲染 `main/` 里的注释 |
| `Makefile` / `CMakeLists.txt` | 两个构建前端，都可用：前者给 `make`，后者给 VSCode 插件 |

## 待办事项

- **ESP-NOW** —— 已评估，**暂缓而不是否决**：连 IP 都不需要，但要求两端同信道。
  当作 TCP 这条路跑稳之后的第二步，见
  [学习笔记/ESP8266开发流程.md](学习笔记/ESP8266开发流程.md) §9.2。
- **OTA** —— 本板是 1MB flash，两个 app 槽放不下，所以本工程没有 OTA。
  相关模块已移出到 `../ota-for-larger-flash/`，要换到更大 flash 的模块才谈得上。
