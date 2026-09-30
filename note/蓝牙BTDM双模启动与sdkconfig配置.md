# 蓝牙 BTDM 双模启动与 sdkconfig 配置笔记

> 日期：2026-09-30 ｜ 项目：Play_Master（bt_audio）

## 1. 现象

测试任务里执行 `esp_bt_controller_enable(ESP_BT_MODE_BTDM)` 时在 `ESP_ERROR_CHECK` 处直接崩溃：

```
ESP_ERROR_CHECK failed: esp_err_t 0x102 (ESP_ERR_INVALID_ARG) at 0x400deef2
--- 0x400deef2: bt_sink_test_task at .../components/middlewares/bt_audio/bt_test.c:137
func: bt_sink_test_task
expression: esp_bt_controller_enable(ESP_BT_MODE_BTDM)
```

## 2. 根因

`sdkconfig` 里经典蓝牙（BR/EDR）没有使能，控制器按 BLE-only 方式编译，请求 BTDM（BLE + BR/EDR）双模使能时被协议栈判定为非法参数（`ESP_ERR_INVALID_ARG`）。

## 3. 解决：打开双模配置

`idf.py menuconfig`：

```
Component config
└── Bluetooth
    ├── Bluetooth（勾选启用编译）
    ├── Bluetooth controller
    │   └── Bluetooth mode = Dual mode (BT/BLE)  → CONFIG_BTDM_CTRL_MODE_BTDM
    └── Bluedroid Options
        ├── Classic Bluetooth（CONFIG_BT_CLASSIC_ENABLED）
        ├── A2DP（CONFIG_BT_A2DP_ENABLE）
        ├── AVRCP（CONFIG_BT_AVRCP_ENABLE）
        └── HFP Client（CONFIG_BT_HFP_CLIENT_ENABLE）
```

保存后用 `idf.py build` 全量重编译即可。注意：nuance 蓝牙相关组件（esp_bt、btc 等）差异大，改选项后建议 `idf.py fullclean` 一次。

## 4. 验证

再跑板级日志应看到：

```
I (1555) BTDM_INIT: BT controller compile version [...]
...
W (2153) BT_BTC: A2DP Enable without AVRC
```

并且 `esp_bt_controller_enable(ESP_BT_MODE_BTDM)` 返回 `ESP_OK`，手机可正常配对连接。
