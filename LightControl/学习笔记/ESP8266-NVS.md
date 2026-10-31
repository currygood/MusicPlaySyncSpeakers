# ESP8266 的 NVS 笔记

> 环境：ESP-01S（**1MB** flash）· ESP8266_RTOS_SDK v3.4（`v3.4-115-g858c7c2e`）· 本项目命名空间 `app_cfg`
>
> SDK 的结论都回查了源码，文中标到 `文件 + 函数/宏名`（行号会随代码改动腐烂，符号名不会）；
> 项目自己的用法直接给文件名。
>
> ⚠️ **§3.3 推翻了项目里现有的一条注释。** 不是补充，是那条注释写错了。
> 如果只读一节，读那一节。

## 1. NVS 是什么

**NVS = Non-Volatile Storage。一块掉电不丢的键值存储。**

这句话里三个词都得看：

- **掉电不丢** —— 它在 flash 里，不是 RAM
- **键值** —— 存的是 `键 → 值`，不是字节流
- **存储** —— 不是文件系统

### 它不是文件系统

没有目录、没有文件、没有偏移量、没有"打开一个文件往里写"。

只有一个映射：

```text
(命名空间, 键)  →  值
 ("app_cfg", "ssid")  →  "DESKTOP-HTLNPUV 4127"
 ("app_cfg", "pass")  →  "88888888"
```

用起来像个小字典。所以它**不能**拿来存日志、存网页、存图片 ——
那些该用 SPIFFS / LittleFS（本项目没用，1MB 的板子也没地方划）。

### 值是带类型的

不是"什么都当字节存"。可用的类型：整数（u8/u16/u32/u64）、字符串、blob。
存和取用的函数必须是同一类，**类型对不上会直接报错**，不会帮你转换
（`nvs.h` 里的 `nvs_get_str()`：*the requested variable type doesn't match the type which was
used when setting a value, an error is returned*）。

### 名字有长度上限

```c
#define NVS_KEY_NAME_MAX_SIZE   16   /* 含结尾的 '\0' */   /* nvs.h */
```

所以**命名空间名和键名都最多 15 个字符**（`nvs.h` 里的 `nvs_open()`，以及 `nvs_set_i8()` 等函数的 `key` 参数说明）。

> 头文件的原话是 "characters"，但**底层卡的是字节数**：限制来自一个定长的
> `char` 数组，比较用的是 `strlen()` / `strncmp()`，都是字节。
> ASCII 下字符和字节一样，`"app_cfg"`、`"ssid"` 都不受影响；但中文一个字
> 3 字节，`"热点名"` 就是 9 字节，再长一点就超了 —— **别用非 ASCII 当键名**。

这个限制容易撞上：`"app_cfg"` 是 7 个字符，安全；`"wifi_credentials"`（16 个）
就刚好超了。但**超了之后发生什么，读和写是两回事**：

**① 写：明确拒绝，报 `ESP_ERR_NVS_KEY_TOO_LONG`。**

```c
/* components/nvs_flash/src/nvs_page.cpp 里 `Page::writeItem()` */
const size_t keySize = strlen(key);          // strlen 数的是字节
if (keySize > Item::MAX_KEY_LENGTH) {
    return ESP_ERR_NVS_KEY_TOO_LONG;
}
```

`Item::MAX_KEY_LENGTH` 就是 `sizeof(key) - 1`（`nvs_types.hpp`），而 `key` 是
`char key[NVS_KEY_NAME_MAX_SIZE]`（`nvs_types.hpp`）——16 个 char，所以是
**15 字节**，不是什么抽象的"字符数"。

> ⚠️ 报的**不是**头文件里写的那个错误码。`nvs.h` 里那些 `nvs_set_*()` 的 `@return` 列表写的是
> `ESP_ERR_NVS_INVALID_NAME`（"key name doesn't satisfy constraints"），
> 但在整个 `nvs_flash` 组件里搜这个宏，命中的**全部是头文件的 `@return`
> 注释**（`nvs.h`、`nvs_handle.hpp`），没有任何一行代码 return 它 ——
> **这个 SDK 实际给的是 `ESP_ERR_NVS_KEY_TOO_LONG`（`nvs.h`）**。
> 写错误处理时按实际返回的来，别照抄头文件注释。

**② 读：没有任何长度检查，超长的键会被静默截断成前 15 字节。**

读路径从 `Storage::readItem()` 走到 `findItem()`，中间没有 `strlen` 比较。键是在
被收进那个 16 字节数组时截断的：

```c
/* components/nvs_flash/src/nvs_types.hpp 里的 Item::Item() */
strncpy(key, key_, sizeof(key) - 1);   // 第 16 个字节起，直接丢掉
key[sizeof(key) - 1] = 0;
```

比较也是定长 15 字节（`nvs_page.cpp` 里的 `Page::findItem()`）：

```c
if (key != nullptr && strncmp(key, item.key, Item::MAX_KEY_LENGTH) != 0) { ... }
```

**后果**：假设 flash 里有一条键叫 `"0123456789abcde"`（正好 15 字节），你用
`"0123456789abcdef"`（16 字节）去读 —— **读得到**，因为前 15 字节一样，
`strncmp` 就判定相等了。但用同一个 16 字节的键去写，会被 ① 拒掉。
**读写不对称**：写得进去的一定读得出来，读得出来的却未必是你以为的那条。

> 本项目碰不到这个坑（`"ssid"` / `"pass"` 都只有 4 字节，而且是同一份代码
> 自己写、自己读）。但**别拿拼出来的字符串当键名** —— 一旦两个键的前 15 字节
> 相同，它们就会互相顶掉，而且不会有任何报错。

---

## 2. 它住在哪：分区表里的 24KB

`partitions_1mb.csv`：

```text
0x009000  nvs        24K   <- 就是它
0x00F000  phy_init    4K
0x010000  factory/app 960K
```

**这 24KB 里住着两家人**，靠命名空间隔开，互不干扰：

| 住户 | 命名空间 | 谁写的 |
| --- | --- | --- |
| WiFi 驱动的 PHY 校准数据 | 驱动自己的（不是 `app_cfg`） | `esp_wifi_init()` 内部 |
| 本项目的 SSID / 密码 | `app_cfg` | 我们自己的代码 |

这不是巧合，是**必须知道**的一条：所以擦 NVS 会把校准数据一起擦掉（见 §5）。

### 谁在管这块分区

`nvs_flash_init()`。它在 `wifi_sta_init()` 里被调用（`wifi_sta.c`），
而且**必须在 `esp_wifi_init()` 之前** —— WiFi 驱动要把校准数据写进 NVS，
NVS 没挂上，驱动直接启动失败。

第一次上电时这块分区是空的，`nvs_flash_init()` 会自己把它初始化好，不用你做任何事。

两种情况它会拒绝初始化，项目里的处理是擦掉重来：

| 错误码 | 什么时候出现 |
| --- | --- |
| `ESP_ERR_NVS_NO_FREE_PAGES` | 没有空页了（写满了，或者分区被写坏了） |
| `ESP_ERR_NVS_NEW_VERSION_FOUND` | 分区里是**另一种 NVS 格式**的数据（换过 SDK 版本、或上一版固件用的别的格式） |

---

## 3. 读写它的 API

全部声明在 `nvs_flash/include/nvs_flash.h` 和 `nvs_flash/include/nvs.h`。

### 3.1 一张表

| 时机 | API | 声明在哪 | 干什么 |
| --- | --- | --- | --- |
| 用之前（一次） | `nvs_flash_init()` | `nvs_flash.h` | 挂载默认分区（标签 `"nvs"`） |
| 用之前 | `nvs_open(ns, 模式, &句柄)` | `nvs.h` 里的 `nvs_open()` | 拿到一个句柄 |
| 读 | `nvs_get_str(h, key, buf, &len)` | `nvs.h` 里的 `nvs_get_str()` | 读字符串 |
| 读 | `nvs_get_u8/16/32/64(h, key, &值)` | `nvs.h` | 读整数 |
| 写 | `nvs_set_str(h, key, 值)` | `nvs_api.cpp` 里的 `nvs_set_str()` | 写字符串 |
| 写 | `nvs_set_u8/16/32/64(h, key, 值)` | `nvs_api.cpp` | 写整数 |
| "落盘" | `nvs_commit(h)` | `nvs_api.cpp` 里的 `nvs_commit()` | ⚠️ **见 §3.3，在这个版本上是空操作** |
| 收尾 | `nvs_close(h)` | `nvs_api.cpp` | 把句柄从 NVS 的句柄表里摘掉 |
| 擦除 | `nvs_flash_erase()` | `nvs_flash.h` | 擦掉整个 nvs 分区 |

**顺序是死的**：`init` → `open` → `get`/`set` → `close`。

### 3.2 最小用法

```c
#include "nvs_flash.h"
#include "nvs.h"

/* ① 挂载。整个程序只做一次，而且要在 esp_wifi_init() 之前 */
esp_err_t err = nvs_flash_init();
if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());     /* 擦掉重来 */
    err = nvs_flash_init();
}
ESP_ERROR_CHECK(err);

/* ② 打开。句柄是个"这次访问的凭据"，用完要还 */
nvs_handle_t h;
if (nvs_open("app_cfg", NVS_READWRITE, &h) != ESP_OK) {
    return;                                  /* 打不开，别硬来 */
}

/* ③ 写 */
nvs_set_str(h, "ssid", "我的热点");
/* ④ 读。注意 len 是【传入缓冲区大小、传出实际长度】，每次都要重赋 */
char buf[33];
size_t len = sizeof(buf);
nvs_get_str(h, "ssid", buf, &len);

/* ⑤ 还 */
nvs_close(h);
```

本项目的真实例子见 `main/wifi_sta.c` 的 `nvs_load_credentials()` 和 `nvs_save_credentials()`。

---

### 3.3 ⚠️ `nvs_commit()` 在这个 SDK 上是空操作

**这一节从一条写错的注释开始。** 项目里原先的注释是这么写的：

```c
/* ⚠ 前面两个 set 只是写进内存缓存，这一句才真正落到 flash。
   漏掉 commit 的话，掉电就没了 —— 而且当场看不出任何异常。 */
err = nvs_commit(h);
```

**这句话在 v3.4 上不成立。** 追一遍源码：

```text
nvs_set_str()                          nvs_api.cpp
    └─ handle->set_string(key, value)  nvs_handle_simple.cpp
         └─ mStoragePtr->writeItem(...) ← 【当场写进 flash，没有中间缓存】

nvs_commit()                           nvs_api.cpp
    └─ handle->commit()                nvs_handle_simple.cpp 里的 NVSHandleSimple::commit()
         └─ if (!valid) return ERR;
            return ESP_OK;              ← 【只检查句柄有效性，什么都不写】
```

而且 SDK 自己在 `nvs_commit()` 里写了注释（`nvs_api.cpp`）：

```c
// no-op for now, to be used when intermediate cache is added
```

**"for now" 的意思是：缓存本来是要加的，这一版还没有。** 所以：

- `nvs_set_str()` 返回 `ESP_OK` 时，数据**已经在 flash 里了**，掉电不会丢
- `nvs_commit()` 除了检查句柄有效，不做任何事
- 漏掉它，在这个版本上**不会有任何后果**

#### 那还要不要写这一句？

**要写，但注释得改。** 两个理由：

1. **它是 API 契约的一部分。** ESP-IDF v4+ 的 NVS 真的加了缓存，那时候
   漏掉 `commit()` 就是"掉电丢失、当场看不出异常"。代码是要移植的，
   习惯要在没有代价的时候就养对。
2. **删掉它，将来没人知道这里曾经需要它。** 留着一句空操作，配合一句
   说清"为什么留着"的注释，比删掉更安全。

按这两条改完之后，代码里现在的样子是（`wifi_sta.c` 里的 `nvs_save_credentials()`）：

```c
/* 这一句在本 SDK（v3.4）上是空操作 —— set 已经当场落盘了。
   留着是因为 ESP-IDF v4+ 的 NVS 真的加了缓存，到那边漏掉它就是
   "掉电丢失，而且当场看不出任何异常"。 */
err = nvs_commit(h);
```

> ★ 这件事本身就是一条经验：**"这个函数是干什么的"不能靠名字猜，也不能靠
> 上一版 SDK 的记忆。** ESP8266_RTOS_SDK 冻在 v3.4，但 ESP-IDF 还在动，
> 两边的同名函数行为可以不一样。名字一样 ≠ 行为一样。

---

### 3.4 另外三个语义坑

这几个是**读/写的时候当场就会撞上**的，项目里已经踩过。

#### 坑一：`length` 是"传入缓冲区大小，传出实际长度"

`nvs_api.cpp` 里的 `nvs_get_str_or_blob()` 里三种走法：

```text
进来时     = 我这边能放多少字节
成功时     = 这个值实际占了多少字节（含结尾的 '\0'）
装不下时   = 它需要多少字节，同时返回 ESP_ERR_NVS_INVALID_LENGTH
```

两个后果：

- **每次调用前都要重新赋 `sizeof(...)`**，不能把上一次写回来的值再用一遍
  —— 它是**实际大小**，不是缓冲区大小，第二次传进去就成了"我只有这么点地方"
- **缓冲区给大了没关系**，多出来的字节不会被动

⚠️ "装不下"时**不是截断**，是整个读取失败、一个字节都不给你。所以缓冲区
一定要 >= 可能的最大值。

#### 坑二：出错时 `out_value` 不被修改

`nvs.h` 里的 `nvs_get_str()` 的原文：

```text
In case of any error, out_value is not modified.
```

意思是：**失败时缓冲区里留着上一次的旧值**，函数不会帮你清零。所以调用方
必须自己决定"读到一半失败"算什么 —— 本项目是直接整个放弃，退回出厂默认值
（`main/wifi_sta.c` 的注释里写了这件事，以及不这么做会出什么鬼）。

#### 坑三：只读打开 / 读写打开的差别不只是"能不能写"

`nvs_open()` 的第二个参数会一直传到底（`nvs_partition_manager.cpp` 里的 `NVSPartitionManager::open_handle()`）：

```c
sHandle->createOrOpenNamespace(ns_name, open_mode == NVS_READWRITE, nsIndex);
                                  /* ↑ 这个布尔叫 canCreate */
```

- **`NVS_READONLY`** → `canCreate = false` → 命名空间不存在就直接返回
  `ESP_ERR_NVS_NOT_FOUND`（`nvs_storage.cpp` 里的 `Storage::createOrOpenNamespace()`）
- **`NVS_READWRITE`** → `canCreate = true` → **顺手把命名空间建出来，并写进
  flash**（`nvs_storage.cpp` 里的 `Storage::createOrOpenNamespace()` 的 `writeItem(Page::NS_INDEX, ...)`）

所以下面这件事值得记住：**一个纯读的函数，如果用 READWRITE 打开，它就有了
写副作用。** 第一次上电时，它会在 flash 里留下一个空命名空间 —— 而且
"命名空间在不在"这个信息被抹掉了，而那正好是"配过 / 没配过"最干脆的判据。

---

## 4. 为什么把 SSID / 密码写进 NVS

### 理由一：改了不用重烧固件（这才是主要理由）

热点名和密码是**部署时才确定**的东西，不是编译期常量。

如果只靠 `#define`：

```text
换一次热点 → 改代码 → 重新编译 → 拆下来接线 → 进下载模式 → 烧录 → 装回去
```

有 NVS：

```text
换一次热点 → 隔着网络发一条 TCP 命令：wifi <新SSID>,<新密码>
```

对一个已经装在墙里、接着继电器的节点来说，这两条路的差别不是"快慢"，
是"要不要动硬件"。

### 理由二：值本来就属于 NVS 的适用范围

小、不常改、要掉电不丢。三条全中。

### 理由三：它能和"出厂默认值"叠成三层

这是本项目实际用的结构（`wifi_sta.h` 的注释）：

| 优先级 | 来源 | 什么时候生效 |
| --- | --- | --- |
| 1（最高） | NVS 里的 `app_cfg/ssid`、`app_cfg/pass` | 只要存过就一直生效 |
| 2 | `WIFI_SSID_DEFAULT` / `WIFI_PASSWORD_DEFAULT`（`wifi_sta.c` 顶部） | 第一次上电（NVS 空） |
| 3 | —— 写进 NVS 后就不再看了 | —— |

实际动作是：开机读 NVS，**读到了就用**；**没读到就用默认值，并顺手写进 NVS**。
所以第 2 层只在"第一次上电"或"刚擦完 NVS"这两种情况下有机会出场。

### ⚠️ 代价：默认值从此变成"只生效一次"

这条是本项目最容易吃亏的地方，值得单独记：

> **一旦 NVS 里存过凭据，改 `wifi_sta.c` 顶部的 `WIFI_SSID_DEFAULT`
> 就再也不会生效了。**

现象很迷惑：明明改了代码、明明烧进去了、串口打印的 SSID 还是旧的。
原因就是三层优先级 —— 你改的是第 2 层，而第 1 层有货。

**要让新的默认值生效，必须先擦 NVS**（见 §5）。

### ⚠️ 还有一条操作纪律（顺序不能反）

因为凭据在 NVS 里，"换热点"这个动作有**两个**受影响的端：

```text
① 先发 TCP 命令 wifi <新SSID>,<新密码>   ← 趁节点还连得上现在这个热点
② 再改主节点那边的热点
```

反过来做（先改主节点），节点就**再也联系不上了** —— 它还在用旧凭据找旧热点，
而能给它发命令的那条链路已经不存在了。

这时候还有一条路，但要**人到现场**：节点连续 30 次连不上会自己开热点、
出一个网页，手机连上去重填（`ESP8266开发流程.md` §10）。
所以顺序写反的后果从"拆下来重烧"降成了"跑一趟"。

### 反面：NVS 不是 OTA

`partitions_1mb.csv` 里专门写过一句，因为这两件事经常被混：

```text
OTA is a way to replace the firmware over the network;
it is NOT how you change WiFi credentials.
```

- **换固件** → OTA（本项目没有，1MB 放不下两个槽，见 ESP8266开发流程.md §9.9）
- **换凭据** → NVS，隔着网络发一条命令

---

## 5. 怎么擦除

四种手段，**从最粗到最细**。选哪个取决于你想保住什么。

| 手段 | 命令 | 擦掉什么 | 保住什么 |
| --- | --- | --- | --- |
| 全片擦除 | `make erase_flash`<br>`esptool.py ... erase_flash` | **整片 1MB** | 什么都不剩 |
| 只擦 nvs 分区 | `esptool.py ... erase_region 0x9000 0x6000` | nvs 那 24KB | bootloader、分区表、app |
| 程序内擦 | `nvs_flash_erase()` | nvs 分区 | 同上 |
| 单个键 | `nvs_erase_key(h, key)` | 那一个键 | 其它键 |

> ⚠️ **前两行（esptool 那两条）都需要先把 stub 传进芯片。**
> 它们在源码里都挂着 `@stub_function_only`（`esptool.py` 里的 `erase_flash()` 和 `erase_region()`）——
> 也就是**要先成功上传 stub 才能跑**。
>
> 这件事在本项目上不是理论：这块板子就在"上传 stub"这一步失败过
> （`Failed to write to target RAM`，长线的杜邦线 + 460800 波特率，
> 降到 115200 就好了）。所以 `erase_region` 发不出去的时候，
> 先去看 esptool 有没有走到 `Uploading stub...` 那一步，
> 别急着怀疑分区地址填错了。
>
> 顺带一提：`--no-stub` 在这里**帮不上忙** —— 它就是用来跳过 stub 的，
> 而这两条命令偏偏只能在有 stub 的时候跑。

### 5.1 全片擦除

```bash
make erase_flash                     # make 流程，走的是 $(ESPTOOLPY_SERIAL) erase_flash
                                     # 定义在 components/esptool_py/Makefile.projbuild
```

⚠️ **它擦的是整片**：bootloader、分区表、app、nvs 全没。
**擦完芯片不会跑任何程序**，串口上只剩 ROM 的启动信息。
必须再烧一次（`make flash` / VSCode 那边的 flash）。

### 5.2 只擦 nvs（推荐）

换了默认凭据、想让新默认值生效时，**其实只需要擦 nvs**：

```bash
esptool.py --port COM8 --baud 115200 erase_region 0x9000 0x6000
```

- 地址和长度来自 `partitions_1mb.csv` 那一行：`nvs, data, nvs, 0x9000, 0x6000`
- ⚠️ **两个数都必须是 4096 的倍数**（`esptool.py` 里 `erase_region` 子命令声明这两个参数时就写死了），
  `0x9000` 和 `0x6000` 都是
- 擦完直接复位就行，**app 还在，不用重烧**

这是最省事的一条路，因为它精确命中了"凭据"这一样东西。

### 5.3 程序内擦

代码里那条路径（`wifi_sta.c` 里的 `wifi_sta_init()`）：

```c
if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
}
```

`nvs_flash_erase()` 的行为（`nvs_flash.h`）：擦掉整个默认 nvs 分区；
**如果分区已经初始化过，它会先自动 de-init**，擦完需要重新 `init` 才能再用。

顺带一句：这条路径本身就是一个**"恢复出厂设置"**的办法 —— 走完之后节点会退回
`wifi_sta.c` 顶部那套默认凭据。想手动做这件事，就是 §5.2 那条命令。

### ⚠️ 5.4 擦 nvs 会连 PHY 校准一起擦掉

记得 §2 那张表：24KB 里住着两家人。

所以擦完之后，**WiFi 驱动的校准数据也没了**。这不是问题 —— 下次
`esp_wifi_init()` 时驱动会自己重新校准并重写一遍（`wifi_sta.c` 里的 `wifi_sta_init()` 记了这件事）。

代价只是"第一次上电慢一点"，不是"坏了"。但如果你不知道这件事，
看到擦完之后启动变慢会以为搞砸了。

---

## 6. 一页速查

```text
NVS 是什么：
  掉电不丢的键值存储。没有目录、没有文件，只有 (命名空间, 键) → 值
  值是带类型的：整数 / 字符串 / blob。类型对不上直接报错，不转换
  命名空间名和键名都最多 15 字节（NVS_KEY_NAME_MAX_SIZE = 16，含 '\0'）
    写超了报 ESP_ERR_NVS_KEY_TOO_LONG；读超了【不报错】，静默按前 15 字节匹配

它住在哪：
  partitions_1mb.csv 里 nvs 那一行：0x9000，0x6000（24KB）
  这 24KB 住两家：WiFi 驱动的 PHY 校准 + 我们的 app_cfg
  nvs_flash_init() 必须在 esp_wifi_init() 【之前】

API 顺序（死的）：
  nvs_flash_init() → nvs_open(ns, 模式, &h) → nvs_get_* / nvs_set_* → nvs_close(h)
  擦除：nvs_flash_erase()（整个分区）/ nvs_erase_key(h, key)（单个键）

★ nvs_commit() 在 v3.4 上是【空操作】（nvs_api.cpp 自己写了 no-op for now）
  nvs_set_str() 当场就落盘，没有中间缓存
  → 但这一句【要留着】：ESP-IDF v4+ 真有缓存，漏了就是掉电丢数据

三个语义坑：
  ① nvs_get_str 的 length 是【传入缓冲区大小、传出实际长度】
     → 每次调用前重新赋 sizeof；装不下是【整个失败】，不是截断
  ② 出错时 out_value 不被修改（nvs.h 里的 nvs_get_str()）→ 缓冲区里是旧值，调用方自己处理
  ③ 只读打开不存在的命名空间 → 直接 NOT_FOUND
     读写打开 → 顺手把命名空间建出来【并写 flash】（nvs_storage.cpp 里的 Storage::createOrOpenNamespace()）
     ⇒ 纯读的函数用 READWRITE 就有写副作用

为什么写进 NVS：
  ① 换热点不用重烧固件 —— 这才是主要理由（部署时才确定的值，不该是编译期常量）
  ② 小、不常改、掉电不丢，正中适用范围
  ③ 和默认值叠成三层：NVS > 编译期默认值 > （写进 NVS 后就不再看了）

⚠️ 代价：存过 NVS 之后，改 wifi_sta.c 顶部的默认值【不再生效】
   表现为"改了代码、烧进去了、串口还是旧 SSID"
   要生效必须先擦 NVS

⚠️ 纪律：换热点的顺序不能反
   ① 先发 TCP 命令 wifi <新SSID>,<新密码>   ② 再改主节点
   反了 → 节点失联 → 跑一趟现场（节点自己开热点，见 开发流程.md §10）
                    ↑ 曾经是"只能拆下来重烧"，配网模式补上了这一条

擦除怎么选：
  esptool.py erase_region 0x9000 0x6000   ← 想只恢复出厂凭据，用这条
  make erase_flash                        ← 整片，连 bootloader 和 app 一起没，擦完必须重烧
  两个地址都必须是 4096 的倍数
  ⚠️ 擦 nvs 会连 PHY 校准一起擦 —— 无害，驱动自己会重做

一句话：
  NVS 是"让凭据可以隔着网络改"的那块地。
  它把"改凭据"从【动硬件】变成了【发一条命令】——
  代价是默认值只生效一次，而且擦它得知道擦的到底是哪一块。
```

---

相关文档：

- `ESP8266开发流程.md` §9.6 —— 换热点时的两种情形和那条操作纪律
- `ESP8266开发流程.md` §9.7 —— 连不上了怎么查 + 为什么拆掉了 SmartConfig
- `ESP8266开发流程.md` §10 —— 配网模式：连不上时怎么把新凭据送进 NVS
- `学习笔记/ESP8266分区表.md` —— 分区表本身（nvs 那 24KB 是怎么划出来的）
- `学习笔记/ESP8266配网踩坑(SmartConfig).md` —— 另一个"怎么把凭据送进 NVS"的方案，以及为什么放弃它
- `main/wifi_sta.c` —— 本项目对 NVS 的全部用法（`nvs_load_credentials()` / `nvs_save_credentials()`）
