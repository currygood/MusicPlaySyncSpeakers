# 从节点换 IDF 6.0.3：烧录失败 + OpenOCD 打不开（三个独立故障叠加）

> 工程：`Play_Slave`（ESP32-S3 N16R8，Xtensa）
> 环境切换：IDF `v6.0.1` → `v6.0.3`（2026-10-29 晚）
> 记录日期：2026-10-29 ~ 2026-10-30
> 工具链位置：2026-10-30 从 `C:\Espressif` 整体搬到 **`F:\Espressif603`**（步骤见第八节），
> 本文所有路径已按新位置书写。
> **结论先说**：这次一共踩了三个互不相干的坑 —— ①OpenOCD 配置路径是扩展的过期默认值；
> ②EIM 装出来的 esptool 是个只有 `__init__.py` 的残缺包（下载被截断）；
> ③S3 的 USB-JTAG 接口被 libwdi 的 usbser 驱动抢占。
> 三者叠加，表现出来的却都像"OpenOCD 打不开"。

---

## 〇、当前环境速查（别和主节点搞混）

| 项 | Play_Slave（从节点） | Play_Master（主节点，**不要动**） |
|---|---|---|
| ESP-IDF | `F:\.espressif\v6.0.3\esp-idf`（注意目录名带点） | `F:\esp_idf_6.0.1\esp_tool\v6.0.1\esp-idf` |
| 工具链目录（IDF_TOOLS_PATH） | `F:\Espressif603\tools` | `F:\Espressif` |
| Python venv | `F:\Espressif603\tools\python\v6.0.3\venv` | （由 6.0.1 那套激活脚本给出） |
| OpenOCD | `v0.12.0-esp32-20260703` | `v0.12.0-esp32-20260304` |
| xtensa-esp-elf-gdb | `17.1_20260402` | `16.3_20250913` |
| 烧录口 | CH343 → COM32 | 视接线而定 |

两套环境完全独立，改从节点不会影响主节点。EIM 记录在 `F:\Espressif603\tools\eim_idf.json`。

IDF 6.x 的入口是 **`tools/idf.py`**，根目录已经没有 `idf.py` 了 —— 手动跑命令时别照 5.x 的路径写。

---

## 一、故障 1：OpenOCD 找不到 `interface/ftdi/esp32_devkitj_v1.cfg`

### 现象

```
embedded:startup.tcl:88: Error: Can't find interface/ftdi/esp32_devkitj_v1.cfg
OpenOCD Exit with non-zero error code 1
```

### 根因

ESP-IDF 扩展的 `idf.openOcdConfigs` **默认值**是写死的：

```json
"default": ["interface/ftdi/esp32_devkitj_v1.cfg", "board/esp32-wrover.cfg"]   // 扩展 1.11.1 package.json 第 740 行
```

而 openocd-esp32 从 2025 年后的版本（本机 20260304 / 20260703）已经**删掉了** `scripts/interface/ftdi/esp32_devkitj_v1.cfg`，只剩 `esp_ftdi.cfg`、`esp32s2_kaluga_v1.cfg` 等。扩展按默认值 `find` 一个不存在的文件，连配置都读不进去，根本没走到连芯片那一步。

这个报错跟芯片型号、GDB、驱动都无关，纯粹是**配置文件名过期**。

### 修复

`Play_Slave/.vscode/settings.json` 显式指定板级配置（一行顶两个文件）：

```json
"idf.openOcdConfigs": ["board/esp32s3-builtin.cfg"]
```

`board/esp32s3-builtin.cfg` 的内容就是 `interface/esp_usb_jtag.cfg` + `target/esp32s3.cfg`，
对应"S3 芯片自带的 USB-JTAG 口"这条通路。

> 如果以后改用 ESP-Prog / 独立 FTDI 调试器，换成 `board/esp32s3-ftdi.cfg`
> （= `interface/ftdi/esp_ftdi.cfg` + `target/esp32s3.cfg`），排针接 GPIO39/40/41/42 + GND。

---

## 二、故障 2：esptool 包残缺，导致编译产物后无法烧录

### 现象

```
Warning: DEPRECATED: 'esptool.py' wrapper is deprecated...
  File "F:\Espressif603\tools\python\v6.0.3\venv\lib\site-packages\esptool\__init__.py", line 58, in <module>
    from esptool.cli_util import (
ModuleNotFoundError: No module named 'esptool.cli_util'
```

### 排查

```text
site-packages/esptool/                     ← 整个包目录只有 1 个文件
    __init__.py
esptool-5.6.0.dist-info/RECORD             ← 只登记了 1 个源文件

pip 缓存里的 wheel:  21,599 字节 / 7 个条目     ← 坏的
正常构建的 wheel:   657,466 字节 / 300 个条目   ← 好的
```

`esptool/__init__.py` 第 58 行就 `from esptool.cli_util import (...)`，而 `cli_util.py`
根本不存在 —— 不是路径问题，是文件没装进去。

### 根因

esptool 在 PyPI 上**只发 `esptool-5.6.0.tar.gz` 源码包，不发 wheel**，pip 需要现场编译
（`pip.ini` 里 `use-pep517 = true`）。当时 EIM 装 6.0.3 时下载到的 tar.gz 被截断了，
但**截断的 tar 仍然能"编译成功"**，于是产出一个只含 `__init__.py` 的假 wheel，pip 老老实实装上了。

旁证：`RECORD` 里出现了 `F:/workbuddy-data/cache/pycache/...` 的 pyc 路径，说明那次安装是在
带着 `PYTHONPYCACHEPREFIX` 的 Git Bash 环境里跑的。

### 修复

```bash
# 1. 重新拉源码包（--no-cache-dir 绕开坏缓存，--no-binary 强制源码）
pip download esptool==5.6.0 --no-deps --no-binary :all: --no-cache-dir -d F:\workbuddy-data\downloads\esptool

# 2. 校验完整性：应有 209 个条目 / 124 个 .py（正常 sdist 约几百 KB）
python -c "import tarfile;n=tarfile.open(r'F:\workbuddy-data\downloads\esptool\esptool-5.6.0.tar.gz').getnames();print(len(n),len([x for x in n if x.endswith('.py')]))"

# 3. 本地重装
"F:\Espressif603\tools\python\v6.0.3\venv\Scripts\python.exe" -m pip install --no-deps --force-reinstall --no-cache-dir "F:\workbuddy-data\downloads\esptool\esptool-5.6.0.tar.gz"
```

装完验证（包目录应有 12 个顶层条目）：

```bash
"F:\Espressif603\tools\python\v6.0.3\venv\Scripts\python.exe" -m esptool version
# esptool v5.6.0 / 5.6.0
```

最后删掉缓存里的坏 wheel：
`F:\workbuddy-data\cache\pip\wheels\e3\b0\2c\...\esptool-5.6.0-py3-none-any.whl`
（不删的话，下次 pip 会优先复用这个 21KB 的残缺 wheel）。

> 其余包逐个 import 过，`esp_idf_monitor` / `esp_idf_size` / `idf_component_manager` /
> `rich_click` / `serial` / `cryptography` / `yaml` / `elftools` 全部正常，只有 esptool 一个中招。
> 所以**不用重装整个 Python 环境**。

---

## 三、故障 3：USB-JTAG 接口被 usbser 抢占（最隐蔽）

### 现象

配置改成 `builtin` 后，日志变成：

```
Info : esp_usb_jtag: VID set to 0x303a and PID to 0x1001
Info : esp_usb_jtag: capabilities descriptor set to 0x2000
Error: libusb_open() failed with LIBUSB_ERROR_NOT_FOUND
Error: esp_usb_jtag: could not find or open device!
```

### 知识前提：S3 的 USB Serial/JTAG 有三个接口

| 接口 | 用途 | Windows 里应该绑什么 |
|---|---|---|
| Interface 0 | CDC 控制 | `usbser.sys` → 一个 COM 口 |
| Interface 1 | CDC 数据 | 随 Interface 0 走，单独不建节点 |
| **Interface 2** | **JTAG** | **WinUSB**（OpenOCD 走 libusb→WinUSB 通路） |

官方驱动包 `USB_JTAG_debug_unit.inf` 里写得很清楚：

```ini
DeviceName = "USB JTAG debug unit"
DeviceID   = "VID_303A&PID_1001&MI_02"
Class      = "USBDevice"        → Include = winusb.inf / Needs = WINUSB.NT
```

### 根因

本机 MI_02 被 libwdi 生成的 `oem173.inf`（`Class=Ports`, `DriverVer 03/19/2026`）绑成了 **usbser**，
多了个 COM 口（先是 COM12，后是 COM31）。libusb 只认绑了 WinUSB 的接口，于是"设备在、但看不见"。

驱动库里 MI_02 的竞争者（`pnputil /enum-drivers` + `C:\Windows\INF\oem*.inf`）：

```
oem163/164/167/169/171.inf   Class=USBDevice   DriverVer 06/02/2012   (老 WinUSB)
oem165.inf                   Class=USBDevice   DriverVer 07/14/2021   (官方 USB_JTAG_debug_unit.inf)
oem173.inf                   Class=Ports       DriverVer 03/19/2026   (libwdi usbser) ← 生效的是它
```

同类驱动里 **Windows 按"驱动日期更新者优先"**，2026 的 usbser 压过了 2021 的 WinUSB，
所以 `pnputil /add-driver /install` 加了 INF 也抢不回来。而且 Win11 26200 的 `pnputil`
**没有 `/update-device`**，只能删 INF 或手动强装。

### 诊断命令

```powershell
Get-PnpDevice -PresentOnly | ? InstanceId -like '*VID_303A*' |
  Select-Object Status,Class,FriendlyName,InstanceId | ft -AutoSize
```

判断标准：`MI_02` 那行必须是 `Class=USBDevice`、名字 `USB JTAG debug unit`；
如果显示 `Ports` + `(COMxx)`，就是被 usbser 占了。

### 修复（需要管理员）

脚本：**`Play_Slave/tools/fix-usb-jtag-driver.cmd`** —— 右键「以管理员身份运行」，四步：

```cmd
:: 1) 官方 WinUSB 驱动包加进驱动库
pnputil /add-driver "F:\Espressif\tools\idf-driver\idf-driver-esp32-usb-jtag-2021-07-15\USB_JTAG_debug_unit.inf" /install

:: 2) 先拔掉板子上标 USB 的那根线，再删掉绑错的串口 INF
pnputil /delete-driver oem173.inf /uninstall /force

:: 3) 插回那根线，重新扫描
pnputil /scan-devices
```

修复后：

```
OK  USBDevice  USB JTAG/serial debug unit     inf=oem165.inf  svc=WinUSB     ← 对了
OK  Ports      USB JTAG/serial debug unit (Interface 0) (COM30)              ← CDC 串口保留，无害
```

### 备选：设备管理器强装（抢得过日期）

端口(COM 和 LPT) → 右键 `USB JTAG/serial debug unit (Interface 2) (COMxx)` → 更新驱动程序 →
浏览我的电脑 → 让我从列表中选取 → **取消勾选「显示兼容硬件」** → 从磁盘安装 → 选上面那个 `.inf`。

### 硬件前提（很容易忽略）

**必须插板子上标 USB 的那个 Type-C 口**（直连芯片 GPIO19/20），标 UART/COM 的是 CH343，
只是 USB 转串口，OpenOCD 没法从它连 JTAG。这块板子两个口都能枚举出 COM，很容易看错：

- 标 UART 口 → `USB\VID_1A86&PID_55D3`（CH343），一个 COM
- 标 USB 口 → `USB\VID_303A&PID_1001`（S3 原生），两个 COM（CDC 一个 + JTAG 一个误占）

调试时两个口都插最舒服：USB 口给 JTAG，UART 口给串口日志。

---

## 四、附带的两个坑

### 4.1 build 目录"半切换"，必须 Full Clean

换 IDF 版本后 `build/` 里是混合状态：

```
CMakeCache.txt:  CMAKE_TOOLCHAIN_FILE = F:/esp_idf_6.0.1/.../toolchain-esp32s3.cmake   ← 旧 6.0.1
                 esp-idf_SOURCE_DIR   = F:/.espressif/v6.0.3/esp-idf                   ← 新 6.0.3
```

工具链用 6.0.1、源码用 6.0.3，编出来的东西不可信。换 IDF 版本后一律：
命令面板 → **`ESP-IDF: Full Clean`** → `Build` → `Flash`。

### 4.2 扩展升级到 2.3.0 后必须要有 `launch.json`

- 调试器类型从 1.x 的 `espidf` 改成了 **`gdbtarget`**（Eclipse GDB Target）；
- 没有 `.vscode/launch.json` 时按 F5 直接报
  `Required file ... launch.json could not be found for the debug session`；
- 扩展有 `DebugConfigurationProvider`，会自动补齐 gdb 路径、`program`（build 里的 elf）、
  OpenOCD 配置、gdbinit，所以文件里只需要最小配置：

```json
{
  "version": "0.2.0",
  "configurations": [
    { "type": "gdbtarget", "request": "attach",
      "name": "ESP-IDF Debug (ESP32-S3 USB-JTAG)",
      "runOpenOCD": true, "buildFlashMonitor": false }
  ]
}
```

文件已就位：`Play_Slave/.vscode/launch.json`。

---

## 五、验证（不依赖 VS Code，可直接跑）

```bash
cd "F:\Espressif603\tools\openocd-esp32\v0.12.0-esp32-20260703\openocd-esp32\bin"
./openocd.exe -f board/esp32s3-builtin.cfg -c "init; targets; resume; shutdown"
```

正常输出：

```
Info : esp_usb_jtag: serial (14:C1:9F:28:94:70)
Info : esp_usb_jtag: Device found. Base speed 40000KHz
Info : JTAG tap: esp32s3.tap0 tap/device found: 0x120034e5 (mfg: 0x272 (Tensilica), part: 0x2003)
Info : [esp32s3.cpu0] Examination succeed
Info : [esp32s3.cpu1] Examination succeed
Info : [esp32s3.cpu0] starting gdb server on 3333
```

（末尾可能出现 `Error: [esp32s3.cpu0] not halted / Failed to prepare for resume`，
是因为芯片本来就在跑、`resume` 没东西可恢复，属于正常，不影响结论。）

另外两条常用验证：

```bash
"F:\Espressif603\tools\python\v6.0.3\venv\Scripts\python.exe" -m esptool version     # 烧录链路
"F:\Espressif603\tools\xtensa-esp-elf-gdb\17.1_20260402\xtensa-esp-elf-gdb\bin\xtensa-esp32s3-elf-gdb.exe" --version   # 调试链路
```

v6.0.3 要求的工具版本（`tools/tools.json`）：`xtensa-esp-elf-gdb 17.1_20260402`、
`xtensa-esp-elf esp-15.2.0_20251204`、`openocd-esp32 v0.12.0-esp32-20260703`，本机均已安装。

> 顺带澄清：那份《ESP-IDF v6.0.3 调试问题与换芯片说明》里的 `riscv32-esp-elf-gdb` 缺失，
> 只对 **RISC-V 芯片（C3/C6/H2/P4…）** 成立。ESP32-S3 是 Xtensa，用的是
> `xtensa-esp-elf-gdb`，这次那条经验用不上。

---

## 六、遗留问题

**IRAM 已经 100% 占满。**

```
IRAM   16384 / 16384   100.0%    (.text 15356 → 93.73%, .vectors 1028 → 6.27%)
```

现在还能跑，但再往 IRAM 里加东西（中断处理、`IRAM_ATTR` 函数、WiFi/BT 相关）就会报
`region iram0_0_seg overflowed` 编译失败。等调试稳定后建议排查多余的 `IRAM_ATTR`，
把非必需的热函数移回 flash。

---

## 七、经验清单

1. **"OpenOCD 打不开"至少有三种完全不同的原因**：配置文件名不对 / 驱动被抢 / 线插错口。
   查的顺序应该是：配置 → 设备管理器里有没有 `303A:1001` → `MI_02` 是不是 WinUSB。
2. **编译通过 ≠ 能烧录 ≠ 能调试**，这是三条独立的链路（工具链 / esptool / OpenOCD+GDB+驱动）。
3. **扩展的默认配置值会随版本漂移**，尤其是 `idf.openOcdConfigs` 这种指向工具目录内文件的项。
   排查时先看扩展 `package.json` 里的 `default`，再去工具目录里 `ls` 那个文件在不在。
4. **源码包安装的 Python 包要校验完整性**。截断的 tar.gz 也能"编译成功"，产出的包缺文件但不报错。
   判断方法：看包目录内容 + `dist-info/RECORD` 行数。
5. **Windows 驱动选择按日期优先**，老 INF 抢不过新 INF。装不上不是"没装对"，是"被别的 INF 压住了"。
6. **换 IDF 版本必须 Full Clean**，否则 build 目录里会残留旧版本的 toolchain 路径。
7. 与硬件相关的东西（编译、烧录、占串口）一律自己跑；别人给的命令先看清楚它会不会碰 COM 口。

---

## 八、把 `C:\Espressif` 整体搬到 `F:\Espressif603`（2026-10-30）

EIM 装 v6.0.3 时默认把工具装在 C 盘（7.04 GB），搬走时可以照下面这套做，**光搬目录是不够的**。

### 8.1 搬运

```powershell
robocopy "C:\Espressif" "F:\Espressif603" /E /MOVE /MT:16 /R:1 /W:1 /NFL /NDL /NP
```

**一定要加 `/MT:16`**：单线程搬这棵树（`esp-clang` 是 LLVM，几万个小文件）实测 5 分钟只走了 137 MB，
多线程同样时间走 2.4 GB。实测全程 11 分 48 秒、16,008 个文件、0 失败。

### 8.2 必须一起改的四处（否则 VS Code 找不到工具链）

| 文件 | 改什么 |
|---|---|
| `F:\Espressif603\tools\Microsoft.v6.0.3.PowerShell_profile.ps1` | 约 20 处路径（`IDF_TOOLS_PATH`、`OPENOCD_SCRIPTS`、`IDF_PYTHON_ENV_PATH`、两段 `PATH`、`idf.py`/`esptool.py` 等函数体） |
| `...\Microsoft.v6.0.3.PowerShell_deactivate.ps1` | 同上 |
| `...\eim_idf.json` | `idfToolsPath` / `activationScript` / `python` 三个字段（注意 JSON 里是 `C:\\Espressif\\tools` 双反斜杠，要单独替换） |
| `venv\Scripts\Activate.bat` / `Activate.ps1` | 写死的 `VIRTUAL_ENV` 绝对路径 |

### 8.3 venv 里的启动器需要重建

`venv\Scripts\*.exe`（pip、esptool、esp-coredump…）的头部嵌了**旧的 python 绝对路径**，
路径长度一变就没法二进制补丁，只能重装对应包来重建：

```powershell
& "F:\Espressif603\tools\python\v6.0.3\venv\Scripts\python.exe" -m ensurepip --upgrade
& "F:\Espressif603\tools\python\v6.0.3\venv\Scripts\python.exe" -m pip install --force-reinstall --no-deps --no-index --find-links "F:\python3.10.1\Lib\ensurepip\_bundled" pip
& "F:\Espressif603\tools\python\v6.0.3\venv\Scripts\python.exe" -m pip install --no-deps --force-reinstall --no-cache-dir "F:\workbuddy-data\downloads\esptool\esptool-5.6.0.tar.gz"
```

`Scripts\*.py` 里的 shebang 是纯文本，直接批量替换即可（本次改了 7 个文件）。

> `idf.py`、`esptool`、`monitor` 这些 IDF 内部调用走的都是 `python.exe <脚本>`，不依赖 `.exe` 启动器，
> 所以就算个别启动器没重建也不影响编译和烧录。

### 8.4 搬完之后

1. **先补一个设置，否则版本列表里会直接看不到 v6.0.3**（见 8.5 第一条）；
2. VS Code：**`ESP-IDF: Select Current ESP-IDF Version`** 重新选 v6.0.3（刷新扩展缓存的 env）；
3. **`ESP-IDF: Full Clean`** + 重编（`build/CMakeCache.txt` 里有 24 处 `C:/Espressif` 旧路径）；
4. 验证四条：`openocd --version`、`xtensa-esp32s3-elf-gdb --version`、`xtensa-esp32s3-elf-gcc --version`、`idf.py --version`（应为 `ESP-IDF v6.0.3`）。

### 8.5 三个坑

- **扩展找 EIM 安装记录的路径是写死的 `C:\Espressif\tools\eim_idf.json`**。
  扩展 2.3.0 的 `dist/extension.js` 里写的是：

  ```js
  const e = Gi("idf.eimIdfJsonPath");
  const t = e && pathExists(e) ? e
          : win32 ? join("C:", "Espressif", "tools", "eim_idf.json")
                  : join(HOME, ".espressif", "tools", "eim_idf.json");
  ```

  目录一搬，扩展就找不到这份记录，**"Select ESP-IDF version" 列表里 v6.0.3 直接消失**
  （只剩 `F:\Espressif\idf-env.json` 里的 6.0.1）。修法是在**用户设置**里加
  （`idf.eimIdfJsonPath` 的 scope 是 `application`，写工作区设置无效）：

  ```json
  "idf.eimIdfJsonPath": "F:\\Espressif603\\tools\\eim_idf.json"
  ```

  改完 `Developer: Reload Window`。

- **删除的目录会进回收站**：这台机器上的删除操作被守卫接管，搬完后 7 GB 并没有真正释放，
  而是躺在 `C:\$Recycle.Bin` 里（约 3 万条）。要用 `[IO.Directory]::Delete()` 直接删，
  或者手动清空回收站。清空时注意别把用户自己以前的旧删除一起带走。

- **`pagefile.sys` 会涨**：多线程搬运时内存压力大，分页文件涨到 9.59 GB，
  这部分属于系统自动管理，不是搬运脚本的锅。

> 搬迁后 EIM 里的 `installationConfig`（base64）仍指向旧路径，那只影响"在 EIM 里重新安装/修复"，
> 日常编译调试不走它。真要重装，直接在 EIM 里删掉这一项重装即可。
> 另外 `idfSelectedId` 原本指向一个不存在的条目（`esp-idf-c55e5c…`），已改成实际条目的
> `esp-idf-673d35…`。

