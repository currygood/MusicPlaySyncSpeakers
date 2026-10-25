# legacy_disabled —— 已下线模块留档（语音唤醒 / 麦克风）

**2026-10-25 起，本目录不在任何构建目标内**（IDF 只编译 `components/` 与 `main/`，
本目录在 `Play_Master/` 根下，CMake 不会 glob 到）。

## 为什么整体下线

主节点在蓝牙播放 1~2 首后反复触发 `Interrupt wdt timeout on CPU0`，四轮排查：

| 轮次 | panic PC | 处置 | 结果 |
|---|---|---|---|
| 1 | `shared_intr_isr` | 判定 level-1 中断风暴 | — |
| 2 | `sdk_config_get_opts` | `CONFIG_BTDM_CTRL_HLI` y→n（蓝牙中断 Level-4→3）+ 播放态暂停 AFE | 频率下降 |
| 3 | `esp_cpu_compare_and_set` (cpu.c:205, PSRAM 分支) | `CONFIG_I2C_ISR_IRAM_SAFE=y`（I2C 队列/自旋锁改落内部 RAM） | 频率再降 |
| 4 | `spinlock_acquire` (spinlock.h:142) ← `vPortEnterCritical` ← `xQueueGiveFromISR` ← `i2c_master_isr_handler_default` | 放弃软件规避，**整条语音/麦克风链路砍掉** | 本目录 |

第 4 次的关键点：**崩溃发生在 I2C（触摸屏）中断里**，不在 esp-sr 里。但两次崩溃的共同
土壤是 Core0/Core1 被唤醒词常驻推理（wn9s 单帧耗时 ≈ 一帧音频时长，占 Core1 约 78%）
压到没有余量 —— 触摸 I2C 的硬件超时（`i2c f=` 一路涨到 2848、`ESP_ERR_INVALID_RESPONSE`）
就是被"ISR 不能及时跑"喂出来的，而硬件超时后驱动做 FSM reset + clear_bus，
又把 I2C ISR 推进更深的中断嵌套路径，最终在 `xQueueGiveFromISR` 里对一个
抢不到的自旋锁无限等待（`portMUX_NO_TIMEOUT`），中断已屏蔽 → tick 停 → INT WDT。

功能取舍：主节点是**同步播放音箱**，语音助手属于可选功能。砍掉后
Core1 完全空出来给解码/同步，A2DP 不再 starved，i2c 超时的诱因也随之消失。

## 目录内容

- `CallPhone/`：`call_phone.c/.h` —— esp-sr 唤醒词检测（wn9s "你好小智"）+ HFP 上行桥接
- `microphone/`：`microphone.c/.h` —— INMP441 I2S RX 采集 + 内部读取任务 + FIFO

## 同时改动的调用点（恢复时按此清单反向改）

| 文件 | 改动 |
|---|---|
| `main/idf_component.yml` | 注释掉 `espressif/esp-sr`（连带 esp-dl/esp-dsp/dl_fft 不再参与构建） |
| `components/App/CMakeLists.txt` | REQUIRES 去掉 `esp-sr` |
| `sdkconfig` | 删除 `ESP Speech Recognition` 整段（`CONFIG_SR_WN_WN9S_NIHAOXIAOZHI` 等） |
| `main/app_main.c` | 去掉 `microphone.h`/`call_phone.h`、`Microphone_Init()`、`App_CallPhone_Task`、`App_Afe_Arbiter_Task`、`APP_EVT_CALL_*`、`APP_CALL_TASK_*`、`s_app.call`、心跳里的 `cp st/afe/f/s/proc/over` 字段 |
| `components/App/UI/ui.h` | `ui_cfg_t` 去掉 `call_phone` 字段 |
| `components/App/UI/scr_settings.c` | 删除"语音助手"分区与 `s_lblMicState` 轮询（第 3 页只剩"固件"） |
| `components/App/UI/ui_statusbar.c` | 删除麦克风图标（状态栏变三图标：WiFi / 蓝牙 / 同步） |
| `components/App/UI/ui.c` | 初始化日志去掉 `call_phone=` |

## 恢复步骤（不建议，除非要重新做语音功能）

1. `mv legacy_disabled/CallPhone components/App/`、`mv legacy_disabled/microphone components/middlewares/`
2. `main/idf_component.yml` 放开 `espressif/esp-sr`，`components/App/CMakeLists.txt` 的 REQUIRES 加回 `esp-sr`
3. `git diff` 对照上表把 8 处调用点改回去（或 `git log -p` 取本次改动前的版本）
4. `sdkconfig` 里重新选 `CONFIG_SR_WN_WN9S_NIHAOXIAOZHI=y`（`idf.py menuconfig` → ESP Speech Recognition）
5. 恢复前务必先解决触摸 I2C 的 3% NACK 与 `LCD_Touch.c` 失败不重置的 bug，否则 INT WDT 会回来

## 顺带说明（未改动，仍在使用）

- `bt_audio` 的 HFP(HF) 代码完整保留：`esp_hf_client_init()` 只注册 profile，
  **没有 `esp_hf_client_connect` 调用者**（唯一调用方 `call_phone` 已下线），
  所以 SLC/SCO 永远不会建立，运行期零开销。若要彻底瘦身可后续单独摘除。
- `bsp/i2s_driver` 的 RX 通道能力保留（通用驱动），只是没有使用者。
- 分区表中 `model`（512KB，esp-sr 声学模型）已闲置，可后续并给 `storage`，
  本次不动分区表以免影响 OTA/coredump 布局。
