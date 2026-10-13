# ESP32-S3 N16R8 板级配置：Flash、PSRAM、CPU、分区表

> 学习笔记 · 对应工程：`Play_Slave`（ESP32-S3 从节点，UDP 组播音频同步音箱）

拿到一块 ESP32-S3 N16R8，SDK 配置编辑器里到底要改什么？

**结论先说：只要改 4 处**，剩下几千项保持默认。但这 4 处每一处都有明确的硬件原因，不是"照着抄"。

---

## 一、先搞清楚：SDK 配置编辑器是什么

### 它不是一个"分区表工具"

ESP-IDF 的配置系统叫 **Kconfig**，SDK 配置编辑器是它的**图形前端**。终端里敲 `idf.py menuconfig` 出来的蓝底菜单，和它是同一棵树。

VSCode 里的入口：`Ctrl+Shift+P` → 搜 `menuconfig` → **"SDK Configuration Editor (Menuconfig)"**。旁边还有个 "Open Classic Menuconfig Terminal"，是同一个东西的终端版。

它管的是整个 ESP-IDF 的**编译期开关**：FreeRTOS 调度参数、WiFi/lwIP、mbedTLS、日志等级、编译器优化、Flash/PSRAM 时序、分区表……**分区表只是里面一个小节**，而且分区表那一节其实只有"用哪个方案 + 用哪个 CSV 文件"两个开关，真正的分区内容在 `partitions.csv` 这个文本文件里。

### 点一下会发生什么

```text
你在编辑器里勾选
      ↓
sdkconfig              （纯文本，被 .gitignore 忽略）
      ↓  编译时生成
build/config/sdkconfig.h   （一堆 CONFIG_XXX 宏）
      ↓
代码里 #ifdef CONFIG_XXX 走不同分支
      ↓
连 IDF 自己的组件源码也按这些宏重新编译 → 触发大量重编
```

⚠️ **最后一行是关键**：改一项配置不是"改个头文件"，而是可能让 IDF 的 WiFi 协议栈、FreeRTOS 等一大堆源码重新编译。所以改配置后编译变慢是正常的。

### 选项之间有依赖关系

这是 Kconfig 和 Keil 那种固定对话框最不一样的地方：

- **父项不开，子项根本不出现**。比如 `ESP PSRAM` 的第一项不勾上，下面整个 PSRAM 子菜单都看不到。
- **选项还会随目标芯片变化**。`SPIRAM_MODE_OCT`（八线 PSRAM）这个选项只有 ESP32-S3、ESP32-C5 之类的目标才有；ESP32 老款就没有。
（SPI RAM config
Mode (QUAD/OCT) of SPI RAM chip in use ->
Octal Mode PSRAM
）
- 
👉 **所以"在菜单里找不到某个选项"通常不是你的问题，是父项没开，或者当前目标芯片不支持。**

### 和 Keil5 / STM32 的类比

| STM32 / Keil 里对应什么 | ESP-IDF |
| --- | --- |
| **手改 scatter 文件 / IAP 的 Flash 地址规划** | **`partitions.csv`** |
| STM32CubeMX 的配置树 + `.ioc` 文件 | **Kconfig 树 + `sdkconfig`**（编辑器就是这棵树的 GUI） |
| CubeMX 生成的 `MX_xxx_Init()` 初始化代码 | 编译宏 `CONFIG_XXX`，IDF 组件自己 `#ifdef` 分支 |
| Options for Target → C/C++ → Define | 同名宏，但由 sdkconfig 自动生成，不用手填 |
| Options for Target → 优化等级、调试信息 | `Compiler options` 子菜单 |
| CubeProgrammer 里手填 Flash 下载地址 | `idf.py flash` 读分区表自动算 |

**最容易类比的其实是分区表**——它和 Keil 里手改 scatter 文件做 IAP 地址规划是一个性质的事。但**整个编辑器更像 CubeMX 的配置树**，比 Options for Target 宽得多。

---

## 二、N16R8 是什么

先把型号拆开看，后面的配置全是从这里推出来的：

| 型号片段 | 含义 |
| --- | --- |
| `N16` | **16 MB** SPI Flash。N 后面的数字就是 Flash 容量 |
| `R8` | **8 MB** SPI PSRAM。R 后面的数字就是 PSRAM 容量，且 **R8 是八线（Octal）** |

所以这块模组是：**16MB Flash + 8MB Octal PSRAM**。

对比一下同系列其他型号，能看出 R8 的特殊：

| 型号 | Flash | PSRAM | PSRAM 线数 |
| --- | --- | --- | --- |
| N8 | 8MB | 无 | — |
| N8R2 | 8MB | 2MB | Quad（四线） |
| N16R2 | 16MB | 2MB | Quad（四线） |
| **N16R8** | **16MB** | **8MB** | **Octal（八线）** |

👉 **记住"R8 = 八线"这一条**，第二节 PSRAM 的模式选择全靠它。

---

## 三、必须改的 4 项

### 3.1 Flash 容量

```text
Serial flasher config → Flash size → 16 MB
```

**为什么要改**：ESP-IDF 默认按 **2 MB** 配置（`choice ESPTOOLPY_FLASHSIZE` 的 default 就是 2MB），它并不知道你手上的模组实际有多大。这个值会被 esptool 用来校验烧录地址、生成烧录参数，也会被 `check_sizes.py` 用来检查分区表有没有超出 Flash。

**不改会怎样**：

- 分区表稍大一点就报 `partition table ... doesn't fit`，编译直接失败
- 就算侥幸编过，烧录时地址计算也可能出错
- 症状很迷惑——看起来是"分区表写错了"，其实根因在这里

**顺带**：同一菜单里的 `Flash SPI mode` 和 `Flash SPI speed` S3 默认就是 `DIO` / `80MHz`，能用，初学阶段不用动。

---

### 3.2 PSRAM（N16R8 的重点）

```text
Component config → ESP PSRAM → Support for external, SPI-connected RAM   ← 先勾这个
```

**勾上之后子菜单才会出现**（Kconfig 的依赖关系）：

| 子选项 | 选什么 | 为什么 |
| --- | --- | --- |
| `Mode (QUAD/OCT) of SPI RAM chip in use` | **Octal Mode PSRAM** | R8 = 八线 PSRAM，见第二节 |
| `Set RAM clock speed` | **80MHz** | R8 的 Octal PSRAM 标称就是 80MHz；默认是 `SPIRAM_SPEED_40M`，能用但慢一倍；`120MHz` 是实验特性，官方文档明说温度变化约 20℃ 就会随机崩溃 |
| `Type of SPIRAM chip in use` | 保持 `Auto-detect` | 让 IDF 自己去读 PSRAM 芯片的 ID |
| `SPI RAM access method` | 保持默认 | 默认是 `Make RAM allocatable using malloc() as well`，即 `heap_caps_malloc()` 和普通 `malloc()` 都能拿到 PSRAM |

**为什么模式必须选对**：PSRAM 是挂在 MSPI 控制器上的独立芯片，Quad / Octal 是**硬件接线方式**——八线芯片有 8 根数据线，四线只有 4 根。选成 Quad，控制器就会按错误的协议去和芯片通信。

**不改会怎样**：

- 最直接：PSRAM 初始化失败，启动日志里报错，那 8MB 完全用不上
- 但**更麻烦的是它可能照样能编译通过**——因为 `esp_psram.h` 里的函数声明不受这个宏控制。于是你得到一个能跑、但只能用小得可怜的内部 RAM 的固件
- 编译期能发现的版本：链接报 `undefined reference to esp_psram_is_initialized`（见第七节）

---

### 3.3 CPU 频率

```text
Component config → ESP System Settings → CPU frequency → 240 MHz
```

**为什么要改**：S3 是双核，最高 240MHz，但 IDF 的默认值是 **160MHz**（`choice ESP_DEFAULT_CPU_FREQ_MHZ` 的 default）。

**代价**：频率越高功耗和发热越大。如果是电池供电并且对续航敏感，留在 160MHz 是合理的；音频同步这种要跑协议栈 + 解码的场景，直接拉满换余量。

---

### 3.4 分区表

```text
Partition Table → Partition Table
```

| 你的需求 | 选哪个 |
| --- | --- |
| 初学、不做 OTA | `Single factory app (large), no OTA` —— 单个 app 分区尽量大 |
| 要做 OTA 双备份 | `Custom partition table CSV`，下面填文件名（如 `partitions.csv`） |

**为什么要改**：默认方案在 2MB Flash 的假设下调小了 app 分区。你现在有 16MB，不改等于白白浪费空间。

⚠️ **重点：具体有哪些分区、各多大，不在编辑器里改**，而是直接编辑 `partitions.csv` 文本文件。编辑器里只决定"用哪个方案 / 用哪个 CSV 文件"。

本工程用的 [partitions.csv](../partitions.csv) 就是 16MB 的双备份 OTA 布局：

```text
# Name,    Type, SubType, Offset,   Size,     Flags
nvs,       data, nvs,     0x9000,   0x4000,
otadata,   data, ota,     0xD000,   0x2000,
factory,   app,  factory, 0x10000,  0x200000,
ota_0,     app,  ota_0,   0x210000, 0x400000,
ota_1,     app,  ota_1,   0x610000, 0x400000,
storage,   data, spiffs,  0xA10000, 0x4F0000,
model,     data, spiffs,  0xF00000, 0x100000,
```

最后一行的末尾是 `0xF00000 + 0x100000 = 0x1000000 = 16MB` —— **正好用满 16MB**。这也是为什么 3.1 里 Flash 容量必须改成 16MB：改成别的值，这张表要么越界要么浪费。

---

## 四、S3 特有的坑：控制台输出通道

```text
Component config → ESP-STDIO → Channel for console output
```

| 你的接法 | 选什么 |
| --- | --- |
| 用板载 **CH340 / CP2102** 那个 USB 口看日志 | 保持 `Default: UART0` |
| 直接用 **S3 芯片自带**的 USB 口看日志 | 改成 `USB Serial/JTAG Controller` |

**为什么会有这个选项**：ESP32-S3 内部集成了一个 USB Serial/JTAG 控制器，和传统的外挂 USB 转串口芯片是两套完全不同的通路。开发板通常两个口都有。

**选错的表现**：程序明明在跑，串口监视器却**一片空白**。这个坑很常见，而且看起来像"程序没启动"，容易往错的方向排查。

> 📌 在 IDF 6.0 里这个选项从 `ESP System Settings` 挪到了新的 `ESP-STDIO` 菜单下。网上老教程给的路径可能对不上。

---

## 五、不用动的

FreeRTOS 内核参数、lwIP、mbedTLS、蓝牙这些，IDF 对 S3 的默认值都是经过验证的合理值。初学阶段改了容易出玄学问题，等真遇到具体需求（比如要更高精度的延时、要调 TCP 缓冲）再回来。

同理，编译器的优化等级也别动——除非你在做尺寸/速度的专项优化。

---

## 六、改完必须做：抄进 sdkconfig.defaults

这是**最容易漏、后果最隐蔽**的一步。

### sdkconfig 和 sdkconfig.defaults 的区别

| 文件 | 是什么 | 会不会进 git |
| --- | --- | --- |
| `sdkconfig` | 编辑器直接改的文件，当前这台机器上的**实际配置** | **不会**（在 `.gitignore` 里） |
| `sdkconfig.defaults` | 你手写的**配置模板**，用于生成全新的 sdkconfig | 会 |

**生成规则**：只有在 **`sdkconfig` 不存在**时，IDF 才会读 `sdkconfig.defaults` 去生成它。`sdkconfig` 已经存在的话，改 `sdkconfig.defaults` **完全没效果**。

### 所以正确的工作流是

1. 用编辑器勾选项（它有搜索框，比翻菜单快得多）
2. 构建验证
3. 把需要在**其他机器上也能复现**的项，手动补进 `sdkconfig.defaults`
4. **删掉 `sdkconfig`**，重新构建一次，确认新配置确实生效

第 3、4 步不做的话，换一台电脑（或者别人 clone 你的仓库）就会退回 IDF 默认值——**Flash 变 2MB、PSRAM 关闭、分区表变默认**，然后你就会遇到下面第七节那种看不出根因的报错。

---

## 七、这个工程踩过的真实一例

`Play_Slave` 从另一台电脑搬到新机器后，构建报：

```text
undefined reference to `esp_psram_is_initialized'
undefined reference to `esp_psram_get_size'
```

**根因**：`sdkconfig` 被 `.gitignore` 忽略，没跟着工程过来。新机器上 IDF 重新生成了默认配置，`CONFIG_SPIRAM` 是关的。

**为什么关掉 SPIRAM 会导致"未定义"**：看 `components/esp_psram/CMakeLists.txt` 的逻辑：

```cmake
set(srcs "system_layer/esp_psram_mspi.c")   # 总是编译

if(CONFIG_SPIRAM)
    list(APPEND srcs "system_layer/esp_psram.c")   # 只有开了才编译 ← 函数在这里面
    # ... 后面还按目标芯片追加 esp_psram_impl_octal.c 等，此处省略
endif()
```

`esp_psram_is_initialized()` 和 `esp_psram_get_size()` 定义在 `esp_psram.c` 里，SPIRAM 一关这个文件根本不参与编译，于是：

| | SPIRAM 关 | SPIRAM 开 |
| --- | --- | --- |
| `build/esp-idf/esp_psram/libesp_psram.a` | **4240 字节** | **112570 字节** |

同目录下编译出来的 .a 文件大小差 26 倍，一眼就能看出来是"整个文件没编进去"，而不是"函数签名对不上"。

👉 **经验**：遇到"某个 IDF 自带函数未定义"，先别怀疑 API 改名——**先去 `build/esp-idf/<组件名>/` 看对应的 .a 是不是小得离谱**。是的话，八成是某个 `CONFIG_` 开关没开导致源文件根本没参与编译。

修复方式就是在 [sdkconfig.defaults](../sdkconfig.defaults) 里把 PSRAM 那几项写进去，删掉 `sdkconfig` 重新生成。

---

## 八、相关延伸

- **`idf.py size` / `idf.py size-components`**：看各个组件实际占了多少 Flash 和 RAM。配完 PSRAM 后用这个确认 8MB 真的进了堆
- **`heap_caps_get_free_size(MALLOC_CAP_SPIRAM)`**：代码里运行时查询 PSRAM 剩余量
- **`.gitignore` 里的 `sdkconfig` 是好意也是陷阱**：它避免了"不同机器配置互相覆盖"，代价是配置默认不随仓库走。团队协作时 `sdkconfig.defaults` 是必须的
- **Kconfig 语法**：`sdkconfig.defaults` 里写的是 `CONFIG_XXX=y` 或 `CONFIG_XXX=123`，和 `sdkconfig` 里生成的格式完全一样。选项名可以直接从 `sdkconfig` 里复制
