# PSRAM 基础概念：SRAM、DRAM 与 STM32 对比

> 学习笔记 · 对应工程：`Play_Slave`（ESP32-S3 N16R8 从节点）
>
> 姊妹篇：[ESP32-S3 N16R8 板级配置 - Flash、PSRAM、CPU与分区表](./ESP32-S3%20N16R8%20板级配置%20-%20Flash、PSRAM、CPU与分区表.md) —— 那篇讲"怎么配"，这篇讲"配的到底是什么"。

---

## 一、PSRAM 是什么

**PSRAM = Pseudo-Static RAM，伪静态随机存储器。**

要理解那个"伪"字，得先看 SRAM 和 DRAM 这对老冤家：

| | SRAM | DRAM |
| --- | --- | --- |
| 全称 | Static RAM | Dynamic RAM |
| 存储单元 | **6 个晶体管**（触发器电路） | **1 个晶体管 + 1 个电容** |
| 靠什么存数据 | 电路状态，通电就一直保持 | 电容上的电荷，**会漏电，必须定期刷新** |
| 速度 | 快 | 慢一些 |
| 密度 / 成本 | 密度低、**贵** | 密度高、**便宜** |
| 接口 | 简单，地址线直接给 | 复杂，需要 RAS/CAS 复用 + 刷新控制器 |
| 典型用途 | CPU 缓存、MCU 内部 RAM | 电脑内存条、手机内存 |

**PSRAM = 用 DRAM 的存储单元，但内置了刷新控制电路，对外伪装成 SRAM 那样好用的接口。**

一句话概括它的定位：**拿 DRAM 的便宜，换 SRAM 的好用。**

名字里的 Pseudo（伪）说的就是这件事——**看起来是 SRAM，里面其实是 DRAM**。

> 📌 另一个常见名字是 **CellularRAM / CRAM**，功能机时代大量使用，和 PSRAM 是同一个东西。

---

## 二、ESP32 上的 PSRAM 有个特殊之处

### 2.1 它是"串行"的

传统 PSRAM 是**并行**接口（一堆地址线 + 数据线，像 FMC 挂的那种）。

**ESP32 用的是 SPI PSRAM —— 走串行总线的**。ESP32 为了省引脚，把 PSRAM 做成和 Flash 共用同一组 MSPI 引脚。

这就是你在配置里看到的两个选项的由来：

| 选项 | 含义 |
| --- | --- |
| `Quad Mode PSRAM` | 4 根数据线 |
| `Octal Mode PSRAM` | 8 根数据线 ← **N16R8 的 `R8` 就是这个** |

**线数直接决定带宽。** 所以模式选错不只是"能不能初始化"的问题——即使芯片两种模式都支持，选成 Quad 速度也会差一倍。

> ⚠️ 如果你在菜单里找不到 `Octal` 这一项，通常是两个原因：① 父项 `Support for external, SPI-connected RAM` 没勾上，整个子菜单是隐藏的；② 它是**单选组里的一个圆点**，菜单上显示的文字是 `Octal Mode PSRAM`，**搜符号名 `SPIRAM_MODE_OCT` 是搜不到的**，要搜 `Octal` 或 `PSRAM`。

### 2.2 它是焊在模组里的

物理上，ESP32 模组是**把 Flash die 和 PSRAM die 堆叠封装在同一颗芯片里**的。所以型号 `N16R8` 才既描述了 Flash 又描述了 PSRAM。

**这带来一个根本性的差别**（对比 STM32，见第三节）：

> 在 ESP32 上，PSRAM 是**模组规格的一部分**，你只需要在 Kconfig 里告诉 IDF 它是几线的。
> 在 STM32 上，PSRAM 是**你板子上的一个器件**，从选型、布线到驱动全是你的事。

### 2.3 它要经过 cache

PSRAM 走 SPI，CPU 不能像访问内部 SRAM 那样直接寻址，**必须经过 cache**。由此带出三个必须记住的限制：

| 限制 | 原因 |
| --- | --- |
| 速度比内部 SRAM 慢不少 | 要过 SPI + cache，有 cache miss 开销 |
| **中断上下文 / DMA 描述符 / `esp_timer` 回调里的缓冲不能放 PSRAM** | cache 被关掉（或高负载）时访问不了，会崩 |
| 热路径上频繁访问的小块要慎重 | cache 抖动 |

---

## 三、STM32 上有没有 PSRAM

分三层回答。

### 3.1 芯片内部：没有

STM32 内部的 RAM **全是 SRAM**。ST 从来没在 MCU 里面集成过 PSRAM。

### 3.2 你的 STM32F401：加都加不了

`stm32-freertos-smartlock` 用的是 **STM32F401**，这颗芯片**连外部存储器控制器都没有**——FMC/FSMC 是 F407 / F429 / F7 / H7 那个级别才有的外设。F401 只有 **64~96KB 内部 SRAM**（按具体型号），想要更多内存只能换芯片。

对比一下就非常直观：

| | STM32F401 | ESP32-S3 N16R8 |
| --- | --- | --- |
| 内部 SRAM | 64~96 KB | 512 KB |
| 可扩展内存 | **没有**（无 FMC） | 8 MB PSRAM |
| 总可用 | ≈ 96 KB | ≈ 8.5 MB |

差了大约 **90 倍**。这不是 ESP32 "内存大"，而是它**把 PSRAM 当标配焊进模组了**。

### 3.3 但 STM32 家族确实能挂 PSRAM

只是需要你自己画板子：

| 挂法 | 用哪个外设 | 典型型号 | PSRAM 类型 |
| --- | --- | --- | --- |
| **并行** | **FMC**（Flexible Memory Controller） | F407 / F429 / F7 / H7 | 并行 PSRAM / CellularRAM |
| **串行** | **QUADSPI / OCTOSPI** | F7 / H7 / H5 / U5 | SPI PSRAM（**和 ESP32 同款**）、HyperRAM |

👉 **H7 的 OCTOSPI 挂的就是 ESP32 用的那种八线 SPI PSRAM。** 所以两边到硬件层面其实是同一类器件，区别在于**谁来把它接起来、谁来写驱动**。

---

## 四、本质区别：这是"芯片特性"还是"板级设计"

| | STM32（传统用法） | ESP32-S3 |
| --- | --- | --- |
| PSRAM 在哪 | 芯片外面，**在你画的板子上** | **模组封装内部**，出厂就焊好 |
| 怎么启用 | 写 FMC/OCTOSPI 初始化代码，调时序、配地址映射，还得自己做读写测试 | Kconfig 里**勾一个选项** |
| 谁来负责 | **你** | 乐鑫 |
| 能不能"没有" | 忘了画就是没有 | 按型号选（`N8` = 无 PSRAM，`N16R8` = 8MB） |

**所以"STM32 上有没有 PSRAM"这个问题，准确答案是：取决于你的板子是谁画的。**
**而在 ESP32 上，它是模组规格的一部分，你只需要配置。**

---

## 五、代价：PSRAM 不是白给的

这是最容易被初学者忽略的一节。PSRAM 便宜大碗，但**它解决的是"容量"问题，不是"速度"问题**。

本工程的 [note/MusicPlay调试笔记.md](../../note/MusicPlay调试笔记.md) 里有两个很典型的真实教训：

### 教训 1：内部 DRAM 和 PSRAM 是两回事

调试时观察到：

```text
int free=163511 largest=110592, psram free=4192024
```

**内部 DRAM 紧张到只剩几百字节的余量，PSRAM 却空着 4.1MB。**

结论：**"内存不够"往往是"内部 RAM 不够"**，PSRAM 再多也替不了它——因为任务栈、队列、中断里访问的缓冲都必须待在内部 RAM。

### 教训 2：PSRAM 也会把人搞崩

`mp3dec_ex` 的内部约 128KB 缓冲被 malloc 到 PSRAM 后，解码热循环触发 **cache-livelock → WDT 复位**。

这个坑的成因：解码热循环同时高频访问 Flash（取指）和 PSRAM，两者共用 MSPI 通路，cache 反复颠簸，最终看门狗超时。

**所以不是"放 PSRAM 就一定好"**——大块、低频访问的数据放 PSRAM 是赚的；热路径上的、中断要用的，放进去反而出事。

### 一句话总结

| | 放哪 |
| --- | --- |
| 大块、低频访问的缓冲（音频包、解码 scratch） | ✅ PSRAM |
| 任务栈、队列、小对象、频繁访问的热数据 | ✅ 内部 RAM |
| 中断上下文 / DMA 描述符 / `esp_timer` 回调访问的缓冲 | ✅ **必须**内部 RAM |

---

## 六、相关延伸

- **`heap_caps_malloc(size, MALLOC_CAP_SPIRAM)`**：显式把一块内存分配到 PSRAM
- **`heap_caps_get_free_size(MALLOC_CAP_SPIRAM)`** / **`MALLOC_CAP_INTERNAL`**：运行时分别查 PSRAM 和内部 RAM 的剩余量，用来定位"到底是哪种内存不够"
- **`idf.py size` / `idf.py size-components`**：看各组件占了多少 Flash 和 RAM
- **HyperRAM**：H7 OCTOSPI 常挂的另一类 PSRAM 变体，走 HyperBus，本质也是"DRAM 单元 + 自刷新逻辑"
- **为什么不直接在 MCU 里放 DRAM**：DRAM 需要持续刷新，功耗和工艺复杂度对 MCU 不划算；MCU 内部用 SRAM 换简单和低功耗，容量不够就外挂
