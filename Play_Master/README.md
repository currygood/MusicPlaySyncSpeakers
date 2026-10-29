| Supported Targets | ESP32 | ESP32-C2 | ESP32-C3 | ESP32-C5 | ESP32-C6 | ESP32-C61 | ESP32-H2 | ESP32-H21 | ESP32-H4 | ESP32-P4 | ESP32-S2 | ESP32-S3 | Linux |
| ----------------- | ----- | -------- | -------- | -------- | -------- | --------- | -------- | --------- | -------- | -------- | -------- | -------- | ----- |

# Hello World Example

Starts a FreeRTOS task to print "Hello World".

(See the README.md file in the upper level 'examples' directory for more information about examples.)

## How to use example

Follow detailed instructions provided specifically for this example.

Select the instructions depending on Espressif chip installed on your development board:

- [ESP32 Getting Started Guide](https://docs.espressif.com/projects/esp-idf/en/stable/get-started/index.html)
- [ESP32-S2 Getting Started Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s2/get-started/index.html)


## Example folder contents

The project **hello_world** contains one source file in C language [hello_world_main.c](main/hello_world_main.c). The file is located in folder [main](main).

ESP-IDF projects are built using CMake. The project build configuration is contained in `CMakeLists.txt` files that provide set of directives and instructions describing the project's source files and targets (executable, library, or both).

Below is short explanation of remaining files in the project folder.

```
├── CMakeLists.txt
├── pytest_hello_world.py      Python script used for automated testing
├── main
│   ├── CMakeLists.txt
│   └── hello_world_main.c
└── README.md                  This is the file you are currently reading
```

For more information on structure and contents of ESP-IDF projects, please refer to Section [Build System](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/build-system.html) of the ESP-IDF Programming Guide.

## Troubleshooting

* Program upload failure

    * Hardware connection is not correct: run `idf.py -p PORT monitor`, and reboot your board to see if there are any output logs.
    * The baud rate for downloading is too high: lower your baud rate in the `menuconfig` menu, and try again.

## Technical support and feedback

Please use the following feedback channels:

* For technical queries, go to the [esp32.com](https://esp32.com/) forum
* For a feature request or bug report, create a [GitHub issue](https://github.com/espressif/esp-idf/issues)

We will get back to you as soon as possible.

---

# 本工程（Play_Master）补充说明

> 上面的内容是 ESP-IDF `hello_world` 模板原文，未针对本工程改写；
> 项目实际说明见仓库根目录的 `项目文档.md` / `完整项目文档.md`，设计文档见 `docs/`。

## 中文字体（运行时 FreeType）

界面中文**不含任何编译期字库**，而是运行时从 flash 分区加载一份 TTF：

| 资产 | 路径 | 说明 |
|---|---|---|
| 字体 | `storage/font/ui_font.ttf` | Noto Sans SC 400 子集，2.02 MiB，GB2312 全量 6763 字 |
| 元数据 | `storage/font/ui_font.meta` | 启动校验依据（size / SHA256 / 覆盖率） |
| 生成脚本 | `tools/font/make_ui_font.py` | 子集化 + 校验 + 写 meta |
| 字符集 | `tools/font/ui_font_chars.txt` | GB2312 + ASCII + 标点 |

运行时路径 `/storage/font/ui_font.ttf`（SPIFFS 挂载点 `/storage`），开机整块预载到 PSRAM，
再用只读内存 VFS `/fontmem` 交给 FreeType。

**烧录时必须整片写**（`idf.py flash`）：字体在 `storage` 分区里，只烧 app 的话
`/storage/font/ui_font.ttf` 不存在，界面会切到英文错误页。

改字体后要同步改 `ui_font.meta` 的 `size=`，否则启动校验失败。
详见 `docs/03-开发/主音频节点中文字体（FreeType运行时字体）.md`。

## 常用构建开关

| 开关 | 默认 | 说明 |
|---|---|---|
| `UI_UI_DRAW_OPT` | `Os` | LVGL / FreeType 的优化级别（`Og` / `Os` / `O2`）。`O2` 在本机 GCC 15.2 上会触发编译器 ICE，会自动附带 `-fjump-tables` |
| `UI_DIAG_STACK_USAGE` | `OFF` | 生成 `.su` 栈帧大小文件，排查栈用量时开：`idf.py -DUI_DIAG_STACK_USAGE=ON build`（会全量重编） |
