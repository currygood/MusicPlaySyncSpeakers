# INT WDT 崩溃：关蓝牙 HLI + 播放时暂停唤醒（方案②+④）

> 崩溃现场：`Guru Meditation Error: Core 0 panic'ed (Interrupt wdt timeout on CPU0)`
> PC=0x40090668 (`sdk_config_get_opts`)，A0 落在 `coex_sw_event_from_isr` 之后，
> 伴随 `task_wdt: IDLE1 (CPU 1)` 饿死、`A2DP starved ~80ms` 每 ~1.5s 一次。
> 完整定位过程见 `note/蓝牙单曲循环不生效-播放模式按源区分.md` 之后的第七/八轮记录。

## 一、定位结论（三条线索交叉）

| 线索 | 证据 | 含义 |
|---|---|---|
| 崩溃 PC 在 coex/BT 代码里 | `0x40090668 sdk_config_get_opts`，A0=`0x4008e424`（`call8 coex_sw_event_from_isr` 返回点），相邻符号全是 `sdk_config_get_bt_mode/sleep_*` + `coex_classic_bt_request` | CPU0 时间花在蓝牙控制器/共存路径 |
| HLI 开着 | `sdkconfig:1037 CONFIG_BTDM_CTRL_HLI=y` → Kconfig 原文 "Using Level 4 interrupt for Bluetooth" | BT ISR 是 **Level-4**，屏蔽 Level-1 的 FreeRTOS tick |
| 看门狗喂狗靠 tick | `esp_system/int_wdt.c` 的 `tick_hook()` | 3s 没跑 tick → panic（`CONFIG_ESP_INT_WDT_TIMEOUT_MS=3000`） |
| CPU1 已满载 | `cp st=1 f/s=24 proc=32610/66279us over=861`、两次 `IDLE1` 饿死 | wn9s 单帧耗时 ≈ 帧长，Core1 没有余量 |
| A2DP 供不上 | `un` 33→40、`starved ~80ms`、`gap≈100ms` | 主机侧排不干净 → coex 反复仲裁 → BT ISR 长期不退 |

触发链：**CPU1 被唤醒词吃满 → A2DP starve → coex 连续仲裁 → CPU0 的 Level-4 BT ISR 长时间不返回 → tick 停 → INT WDT**。
三者缺一不可，所以前 80~121 秒都正常。

## 二、改动 1：关闭 `CONFIG_BTDM_CTRL_HLI`

`sdkconfig`：
```
CONFIG_BTDM_RESERVE_DRAM=0xdb5c
# CONFIG_BTDM_CTRL_HLI is not set      ← 原 CONFIG_BTDM_CTRL_HLI=y
```

### 为什么有效（看 `components/bt/controller/esp32/bt.c`）

`bt.c` 通过 `osi_funcs_t` 把一套 OS 原语注入闭源控制器，HLI 开关直接换掉实现：

| | HLI = y | HLI = n |
|---|---|---|
| ISR 注册 | `set_isr_hlevel_wrapper` → `hli_intr_register(..., DPORT_PRO_INTR_STATUS_0_REG, ...)` | `xt_set_interrupt_handler` |
| 临界区 | `hli_intr_disable/restore`（自带 nested 计数，**只关中断，不进 FreeRTOS 临界区**） | `portENTER_CRITICAL(&global_int_mux)` |
| ISR 内拿信号量 | `semphr_take_from_isr_wrapper` **`assert(0)`（不支持）**，改用 HLI 自己的队列语义 | 真 `xSemaphoreTakeFromISR`，**不可能阻塞** |
| 中断等级 | **4** | **3**（`interrupt_l3_disable` 的命名即证据） |

关键点：**HLI 是唯一能让 BT 固件"合法地在中断里长时间等待"的机制**——它用私有队列把 ISR 上下文伪装出来，于是 ISR 可以在里面排队等资源，一直不返回。关掉之后，BT 的 ISR 只能是 level-3 + 标准 `xSemaphoreTakeFromISR`，**结构上不可能在 ISR 里阻塞**，必然快速返回。

### 必须如实说明的一点

关掉 HLI 后 BT 是 **level 3**，仍然高于 tick 的 level 1，**ISR 执行期间 tick 照样被屏蔽**。
所以准确的表述是：

> 关 HLI **不是**"保证 tick 永远能跑"，而是"**消除 ISR 内长时间等待这种结构性可能**"，
> 把"卡 3 秒"降级为"卡几十微秒"。

如果关掉后还崩，说明死锁点不在 BT 控制器 ISR 里，而在 WiFi ISR / coex 自旋锁，那时要转向方案③（开 `WIFI_IRAM_OPT` + `RX_IRAM_OPT`）。

### 附带效果（注意）

- `ESP_SYSTEM_CHECK_INT_LEVEL` 的 choice 默认值从 LEVEL_5 变回 LEVEL_4。本项目
  `CONFIG_ESP_SYSTEM_CHECK_INT_LEVEL_5=y` 仍显式保留 → IWDT 依旧是最高级，
  崩溃可观测性不损失（level 5 > level 3/4）。
- `PM_UPDATE_CCOMPARE_HLI_WORKAROUND` 自动变 n（本项目未开 `CONFIG_PM_ENABLE`，无影响）。
- `esp_int_wdt_init_for_livelock_fix()` 的 ERI 调试口 hack 整段编在
  `#if CONFIG_BTDM_CTRL_HLI` 里，关掉后不再编译 —— 说明 Espressif 把
  "HLI + 缓存操作 = 已知活锁"写进了代码。

### 风险

IDF 默认 y 是有原因的：HLI 为控制器提供低时延的 ISR 语义。关掉后可能出现

- A2DP 时延略增、断音概率变化；
- 极端情况下控制器 assert（若闭源库某条路径假定 HLI 队列可用）。

**必须真机长跑验证**，尤其关注 `[btpcm]` 的 `un` / `gap` / `drop` 三项是否变差。

### 回退

把 `sdkconfig` 那一行改回 `CONFIG_BTDM_CTRL_HLI=y` 即可（一键回退，无代码依赖）。

---

## 三、改动 2：播放期间暂停 AFE（`call_phone`）

### 为什么

`note` 里已标定：wn9s 单帧耗时 ≈ 帧长（32ms 预算），`f/s=24`（满速 31.25）说明
**每秒丢掉约 1/4 麦克风数据**，且这一项占掉 Core1 约 78%。这不是"优化一点"能救的
量级——ESP32 上 wn9s 已是最轻模型（`model/wakenet_model/` 里没有 wn5，wn9/wn10 的
Kconfig 都 `depends on ESP32S3||P4||S31`）。

所以按产品取舍：**播放时不要唤醒词**，把 Core1 还回去。

### 接口（`call_phone.h`）

```c
esp_err_t call_phone_set_suspended(call_phone_handle_t cp, bool suspend);
bool      call_phone_is_suspended(call_phone_handle_t cp);
```

- `suspend=true`：立即停止喂麦克风、停止跑 AFE，任务阻塞在命令队列上（`portMAX_DELAY`），
  Core1 立刻收回 ~780ms/s。
- `suspend=false`：清麦克风 FIFO + 复位重采样 + 丢弃前 3 帧唤醒判定，再恢复监听。
- **幂等**：与当前状态相同直接返回，可以按轮询周期反复下发。
- **不打断通话**：`CONNECTING/STREAMING` 时只置标志，等它自己回到监听时
  由 `CallPhone_Goto_Listening()` 直接把目标状态改成 IDLE，不会"自己爬回 LISTENING"。

### 实现要点（`call_phone.c`）

1. 新命令 `CP_CMD_SET_SUSPEND` + `msg.suspend` 字段；命令一律经
   `CallPhone_Msg()` 零初始化构造，避免以后加参数漏初始化。
2. `struct call_phone_s` 增 `volatile bool suspended` + `uint8_t resume_flush`
   （只在任务线程写，任意线程读）。
3. **`CallPhone_State_Set(cp, LISTENING)` 的 4 处全部收敛到 `CallPhone_Goto_Listening()`**
   （唤醒失败、SCO 超时、SCO 被挂断、主动 HANGUP），否则会出现
   "播放期间被抑制成 IDLE，通话结束后又爬回 LISTENING" 的矛盾。
4. `CallPhone_Reset_Pipeline()`：`Microphone_Flush()` + `rs_pos=0` + `resume_flush=3`。
   暂停期间麦克风 FIFO 会一直满着丢旧数据，恢复瞬间读到的是几秒前的音频；
   AFE 内部还压着半帧。不清就是"把过去的噪声当成现在的语音"，轻则白跑几帧，
   重则误唤醒直接抢断 A2DP。
5. `CallPhone_Listening()` 开头加 `suspended` 防御（状态切换与读取之间有一拍窗口）。
6. 保留原有的 `proc_us >= frame_us → vTaskDelay(1)` 让出逻辑（未改，另案处理）。

### App 层仲裁（`main/app_main.c`）

`on_event` 已被 UI（`ui_on_music_event`）占用，所以不改 `music_play` 的职责，
新增一个极轻的任务：

```c
#define APP_AFE_ARB_PERIOD_MS  300
App_Afe_Arbiter_Task   // core 0 / prio 1 / 栈 3072
```

每 300ms 读一次 `music_play_get_view()`，判断"是否真的在出声"：

```c
cb         = bt_audio_get_a2dp_cb_count(s_app.bt);
bt_flowing = (cb != last_cb);      /* PCM 回调计数在涨 = 有真实数据流 */
busy       = (st == MUSIC_STATE_PLAYING) || bt_flowing;
```

**双判据的原因**：手机可能不发 `PLAY_STATE` 事件，`state` 会长期停在 `PLAYING`；
只信 state 就会把唤醒词**永久关掉**。用"有没有数据流"兜底后，最坏情况只是多停
300~600ms。

只在 `busy` 变化时下发一次，日志：
```
W AppMain: [afe] music=PLAYING a2dp_cb=1288 -> wake word SUSPENDED (ESP_OK)
W AppMain: [afe] music=PAUSED  a2dp_cb=2411 -> wake word ON (ESP_OK)
```

心跳增加 `afe` 字段：
```
W AppMain: [hb] ... cp st=0 afe=OFF f/s=0 ...      ← 播放中，唤醒词已停
W AppMain: [hb] ... cp st=1 afe=on  f/s=24 proc=... ← 空闲，正常监听
```

### 取舍（需要产品确认）

**播放期间不支持语音唤醒**，必须按键或先暂停。这是本方案的核心代价。
如果以后要求"边放边唤醒"，只剩换芯片（S3 有向量指令 + wn9/wn10）或外挂语音芯片。

---

## 四、真机验证清单

1. **上电看配置生效**
   - 启动日志里没有 `HLI` 相关字样即可；更直接的验证是崩溃对比（见 5）。
2. **空闲态唤醒词仍正常**
   - `[hb]` 应显示 `cp st=1 afe=on f/s≈24~31`；
   - 喊"你好小智"应看到 `CpState: state LISTENING -> CONNECTING`。
3. **播放开始时立刻抑制**
   - 手机放歌 → 300ms 内出现 `[afe] ... wake word SUSPENDED`；
   - `[hb]` 变 `cp st=0 afe=OFF`；
   - **关键指标**：`un` 应停止增长、`A2DP starved` 应基本消失、`IDLE1` 饿死日志应消失。
4. **暂停播放后恢复**
   - 手机暂停 → 出现 `wake word ON`，`cp st=1 afe=on`；
   - 喊唤醒词能命中；**不应出现误唤醒**（`resume_flush` 生效的话，恢复瞬间不会误触发）。
5. **最关心的：INT WDT 还崩不崩**
   - 复现条件：A2DP 播放 + WiFi 同步发送 + 唤醒词（现在播放时会自动停）。
   - 如果**播放期间不再崩**、但**空闲时喊唤醒词崩**，说明瓶颈仍是 wn9s 本身，
     转向"降低唤醒占空"或换芯片。
6. **蓝牙功能回归**（关 HLI 的主要风险）
   - A2DP 长时间播放有没有新断音；
   - 切歌/暂停/音量/循环模式（含上一轮的 AVRCP repeat）是否仍正常；
   - HFP 语音助手（SCO 建立、上行）是否仍正常。

## 五、如果没有效果

优先级顺序：
1. 开 `CONFIG_ESP32_WIFI_IRAM_OPT` + `CONFIG_ESP32_WIFI_RX_IRAM_OPT`（降 coex ISR 延迟，需确认 IRAM 余量）；
2. 关 `CONFIG_BTDM_CTRL_MODEM_SLEEP`（崩点就在 `bt_sleep_enable` 家族，流式播放时 modem sleep 无意义）；
3. 把 `CONFIG_ESP_INT_WDT_TIMEOUT_MS` 从 3000 降到 1500（只让故障暴露更快，不是修复）；
4. `idf.py coredump-info` 读 flash `0xFE0000`，看清崩瞬间所有任务的栈。
