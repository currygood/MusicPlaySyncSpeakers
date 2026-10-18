# ESP32 WiFi 配网调试笔记

------

## 问题现象

1. ESP32 连接 2.4G WiFi 时反复失败，串口日志：

```
wifi:state: assoc -> run (0x10)       ← 4-way handshake 开始
... (约3秒后)
wifi:state: run -> init (0xf00)       ← 握手超时，reason=15 错误码 0xf00
```

2. ESP32 扫描 WiFi 时只显示 1 个 AP，串口日志却扫到 8 个

3. 扫描时 UI 报 `ESP_ERR_WIFI_STATE`，重连与扫描互斥

## 根本原因（四个独立 bug）

### Bug 1：密码存入 NVS 时写入乱码（主因）

**文件**：`scr_settings.c` 的 `wifi_connect_do()` 函数

**时序**：

```c
/* 1) 关闭密码对话框（lv_obj_del）*/
lv_obj_del(s_wifiPwdDlg);   // textarea 是对话框子控件，一并被 free❗

/* 2) 用悬空指针写 NVS */
snprintf(role_cfg.wifi_password, ..., password);  // password 已指向已释放内存❗
```

- `password` 来自 `lv_textarea_get_text()`，返回的是 textarea 内部缓冲区指针
- 步骤1删对话框时 textarea 内存被释放，步骤2依旧用这个悬空指针 → 写入 NVS 的是乱码
- 日志 `plen=13` 但实际密码 9 位，就是因为读到已释放内存的残留数据

**影响**：

- 本次连接：`wifi_manager_set_sta()` 也用悬空指针，下发乱码密码 → 握手超时
- 后续启动：NVS 里存的是乱码，`node_role_init()` 读出来就是乱码，永远连不上

**修复**：删对话框前把密码拷贝到局部缓冲 `pwd_buf[65]`，后续全部用 `pwd_buf`

------

### Bug 2：WiFi 接收缓冲区偏小（次要因素）

**文件**：`sdkconfig`

**现象**：ESP32 在信道6上（周围4+个AP），Beacon帧 + 控制帧占满静态RX缓冲区，WPA握手期的 EAPOL 帧被 DMA 静默丢弃 → 握手超时。

**配置**：

```
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM:  4 → 6   (+2 缓冲, ~3.2KB)
CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM: 4 → 8   (+4 缓冲, ~6.4KB)
```

内存增量约 9.6KB（内部 DRAM 剩余 14.7KB 以内）

------

### Bug 3：扫描到多个 AP 但 UI 只显示一个

**文件**：`scr_settings.c` 的扫描列表构建

**现象**：串口日志打印扫描到 8 个 AP，但 UI 列表里只看到 1 个。

**根因**：LVGL 列表容器没有设置自动布局，每行 row 全部堆叠在坐标 `(0, 0)`，后面的覆盖前面的，最终只露出最后创建的那一行。

**修复**：给列表容器添加 Flex 垂直布局，让每行自动向下排列。

```c
lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
lv_obj_set_style_pad_row(list, 2, 0);
```

------

### Bug 4：WiFi 扫描与重连状态冲突

**文件**：`wifi_manager.c` 的 `wifi_manager_scan()`

**现象**：UI 显示扫描失败 `ESP_ERR_WIFI_STATE`，日志出现 `reason=201`。重连定时器正在发起 `esp_wifi_connect()` 时，用户点击扫描触发 `esp_wifi_scan_start()`，两个操作互斥。

**修复**：
1. 新增 `bool scanning` 标志，在断连事件中阻止重连调度
2. 扫描前停止重连定时器、调用 `esp_wifi_disconnect()` 确保状态机空闲
3. 主动扫描改为被动扫描，每信道驻留 400ms，提高弱信号场景的 AP 发现率

```c
conf.sta.scan_method     = WIFI_ALL_CHANNEL_SCAN;
conf.sta.threshold.authmode = (p_len > 0) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
```

```c
wifi_scan_config_t sc = {
    .scan_type = WIFI_SCAN_TYPE_PASSIVE,
    .scan_time.passive = 400,
};
```

------

## 排查过程中排除的因素

| 假设             | 验证结果                         |
| :--------------- | :------------------------------- |
| 密码输错         | 日志明文打印正确，"Tgs200410"    |
| 路由器客户端上限 | 换手机热点测试同样超时           |
| PMF 不兼容       | 启用 `pmf_cfg.capable=true` 无效 |
| OWE/SAE 模式     | 已确认使用 WPA2_PSK              |

------

## 关键日志对照

```
# 修复前（密码悬空）
pass="x??P~?:" plen=13     ← 乱码

# 修复后（密码正确）
pass="Tgs200410" plen=9    ← 正确
```

------

## 最终修改清单

| 文件             | 改动                                                      |
| :--------------- | :-------------------------------------------------------- |
| `scr_settings.c` | `wifi_connect_do()` 中 `pwd_buf` 局部拷贝，删对话框前完成 |
| `scr_settings.c` | 扫描列表容器添加 `LV_FLEX_FLOW_COLUMN` 垂直布局            |
| `wifi_manager.c` | 新增 `scanning` 标志，扫描前停止重连/断开当前连接          |
| `wifi_manager.c` | 主动扫描 → 被动扫描，400ms/信道                            |
| `sdkconfig`      | RX 缓冲区 4→6 / 8                                         |

------

## 教训

1. **LVGL 中 lv_textarea_get_text() 返回的是内部指针，控件的生命周期就是指针的有效期**。如果先 `lv_obj_del()` 再读，必然悬空。
2. ESP32 内部 DRAM 紧张时，WiFi RX 缓冲区不够会导致 EAPOL 帧静默丢弃，症状和密码错误一模一样（握手超时），难以区分。
3. **LVGL 容器默认无布局**，子控件全部叠在原点。需要显式设置 `lv_obj_set_flex_flow()` 才能自动排列。
4. **WiFi 扫描和连接不可同时进行**，扫描前必须确保没有挂起的重连定时器，且当前处于断开状态。