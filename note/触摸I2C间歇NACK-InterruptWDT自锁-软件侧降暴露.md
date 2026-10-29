# Interrupt WDT：触摸 I2C 自锁（7 次崩溃 → 触摸整体改 GPIO 模拟 I2C）

> **状态：已解决（2026-10-29）。**
> 结论先说：根因不在应用层用法，而是 **IDF `i2c_master` 的 level-1 共享中断在总线异常时
> 能把中断派发层自己锁死**——这种崩溃软件侧只能降概率，不能断根。
> 最终把触摸整体搬出 `i2c_master`，改用 **GPIO 模拟 I2C（50 kHz 开漏）**，
> 从此没有任何可被打爆的 ISR 路径。
>
> 实现细节另见 `docs/03-开发/触摸I2C改为GPIO模拟.md`。

---

## 一、崩溃长什么样（7 次时间线）

| # | 日期 | up | backtrace 关键帧 | 当时结论 | 现在回看 |
|---|---|---|---|---|---|
| 1~2 | 10-24 前 | — | core1 被唤醒词吃满、日志全停 | 唤醒词占用 | 并存问题，与本案无关 |
| 3 | 10-25 | 2850s | `spinlock_acquire` 无限等，`i2c f=2848` | 跨核锁竞争 | 同一故障，位置在 I2C ISR 内部的队列锁 |
| 4 | 10-26 | — | `A3=0xCDCD`（本核 owner） | 锁状态错位/自锁 | 与 #5 同源，都在**共享中断派发层** |
| 5 | 10-26 17:40 | 710s | `xPortSetInterruptMaskFromISR → shared_intr_isr(intr_alloc.c:467)` | `clear_bus` 危险窗口 | `r=0` 仍崩 ⇒ 推翻"clear_bus 是必要条件" |
| 6 | 10-27 晚 | 6196s（103 min） | `s_i2c_send_commands → s_i2c_hw_fsm_reset(clear_bus=true) → gpio_set_level` | **IDF 缺陷**：FSM reset 不关中断 | 成立，但只解释了一条路径 |
| 7 | 10-29 | **8453s（2.35 h）** | `i2c_master_isr_handler_default` **函数序言**（`i2c_master.c:805`） | — | **反复重入**，不是卡在循环里 |

第 7 次的两个细节最关键：

```text
Guru Meditation Error: Core 0 panic'ed (Interrupt wdt timeout on CPU0)
PC : 0x40090f21  i2c_master_isr_handler_default  i2c_master.c:805
805 行实际内容： portBASE_TYPE HPTaskAwoken = pdFALSE;   ← 函数序言
Core 0 was running in ISR context: EPC1 = uart_hal_write_txfifo
Core 0: IDLE0        Core 1: IDLE1        ← 两个核都空闲，UI_Task 根本不在栈上
```

- **PC 停在函数序言**说明中断在反复重入（清掉中断 → 外设立刻再置位），不是死循环；
- `CONFIG_ESP_INT_WDT_TIMEOUT_MS` 已经是 **3000ms**（默认 300 的 10 倍）还超时，
  说明这是持续 3 秒以上的中断风暴；
- 两核都在 IDLE，崩溃前 15 分钟没有任何 `[REFR]`、`blk/s=0`、`a2cb=0`
  → 与 UI、字体、音频**完全无关**。

## 二、为什么 `i2c_master` 这条路走不通

### 2.1 它的同步 API 本质是"队列 + 信号量 + ISR"

一次 6 字节读要 **2~3 次中断进出**，每次进 ISR 都要抢 `intr_alloc` 的全局 spinlock。
轮询 20ms = 50 事务/s ≈ 150 次 ISR/s，按 6% NACK 率，每秒约 3 次把驱动推向脱同步。

### 2.2 两条能自锁的路径

1. **软件超时分支**（`i2c_master.c:535-542`）：`xSemaphoreTake` 失败后**仍然 Give**，
   凭空多一个 token → 任务与硬件 FSM 脱同步 → 后续中断找不到对应事务；
2. **硬件超时分支**：驱动内部自己调 `s_i2c_hw_fsm_reset(clear_bus=true)`，
   而 reset 全过程 I2C 中断**仍是使能**的（掩码到最后才清）→ 总线毛刺立刻触发
   共享中断 → 派发层持全局 spinlock 跑 handler 链 → 与被打断的临界区**同核自锁**。

### 2.3 我们做过的补救和它们的边界

| 处置 | 效果 | 为什么不够 |
|---|---|---|
| `I2C_TRANS_TIMEOUT_MS` 50 → 150ms | 少踩"软件超时"分支 | 只挡一条路径 |
| 轮询 20 → 40ms、失败退避到 5s 慢探测 | 事务量降一个量级，`f=` 速率 3.1 → 0.94 次/s | **单次失败率没变（~4.9%）**，降的是样本数 |
| 删掉应用层 `I2c_Reset_Bus` | 不再占最危险窗口 | `r=0` 仍崩，说明不是它 |
| CTP_INT 飞线（GPIO2，负边沿 + 兜底轮询） | 静止事务量 25 → 5 次/s | 同上，只是降概率 |
| **IDF 本地补丁**：`s_i2c_hw_fsm_reset` 进出各加 `esp_intr_disable/enable` | 第 7 次崩溃推迟到 2.35h | **只护住 reset 那一个窗口**，还有别的入口 |

补丁本身是对的（`note/ESP-IDF6.0.1本地补丁-i2c_fsm_reset关中断.md`），
但它的作用是"拆掉一颗已知的雷"，不是"排掉所有雷"。

### 2.4 电气侧的判断

`i2c_driver.h` 的 `I2C_FREQ` 本来就是 **100 kHz**（不是 400k，"降速验证"这个选项早期就用掉了）。
`bsp/i2c_driver.c` 只开了 `.enable_internal_pullup = true`，**没有外部上拉**；
ESP32 内部上拉约 45 kΩ，100 kHz 下上升沿远超规范 1 µs，尤其走 FPC 到触摸屏时。
累计失败率 **8.5%（f=3128 / ok=33402）**，对板级 I2C 明显不正常。

> 这是硬件侧的真问题，但**不是本次崩溃的充分条件**——因为即使 NACK 率降到 0，
> `i2c_master` 在别的异常路径上仍然可能自锁。所以最终选择"从架构上不用它"。

## 三、最终方案：GPIO 模拟 I2C

### 3.1 设计要点

| 项 | 取值 | 理由 |
|---|---|---|
| 引脚 | SDA=**GPIO32**、SCL=**GPIO33** | 沿用 `bsp/i2c_driver.h` 的宏，避免两处漂移 |
| 方向 | `GPIO_MODE_INPUT_OUTPUT_OD` | 真开漏，与 I2C 电气一致；读 SDA 前先写 1 释放 |
| 上拉 | 内部 `GPIO_PULLUP_ENABLE` | 仍是内部 45k，所以速度必须让 |
| 速率 | 半周期 `FT_I2C_HALF_US = 10` → **≈50 kHz** | 100 kHz 时半周期 5 µs，内部上拉撑不住上升沿 |
| 任务 | `Touch_Task` prio **3** / core0 / 栈 2048 | 忙等期间可被 UI(10)、协议栈、WiFi 抢占 |

### 3.2 时序实现的几个要点

- `ft_i2c_write_byte()`：**开漏写 1 之后要读回 SDA** 才能判断从机 ACK
  （开漏下写 1 等于释放总线，电平由从机决定）；
- `ft_i2c_read_byte()`：主机在**第 9 个时钟**释放 SDA 并采样 ACK/NACK；
- `ft_i2c_bus_recover()`：敲 **9 个 SCL + 补一个 STOP**，把被从设备拉死的 SDA 放出来。
  **这个动作现在才是安全的**——以前不敢做是因为 `i2c_master_bus_reset()` 要把引脚
  从外设手里抢过来再切回去，是个危险窗口；现在引脚完全归应用所有，随时可以敲。
  触发条件：连续失败每 20 次执行一次（单次约 200 µs）。
- 删掉 `Ft6336I2cHandle`，改用 `s_ftI2cReady`；
  `LCD_TOUCH_Init(i2c_master_bus_handle_t)` → **`LCD_TOUCH_Init(void)`**（头文件 + `ui_disp.c` 两处）。

### 3.3 代价（已核算）

一次 6 字节读 ≈ 9 字节 × 9 bit × 20 µs ≈ **1.6 ms 忙等**。
40 ms 轮询下约 **3% CPU**，静止 200 ms 兜底时 0.6%。
任务 prio 3，随时可被抢占；音频在 core1，完全不受影响。

### 3.4 结果：路径物理上不存在了

全工程已无任何 `I2c_Init_Bus` / `I2c_Add_Device` 调用（grep 确认），
`i2c_master` 的中断永远不会被注册。这不是"降低概率"，是"没有这条路"。
`bsp/i2c_driver` 保留，以后接别的 I2C 设备（传感器等）还能用。

## 四、改造后实测（2026-10-29）

启动日志新增一行（`Touch` 标签，不受 `LCD_TOUCH_DEBUG_LOGS` 影响）：

```text
I (xxx) Touch: FT6336G ready on soft I2C: SDA=32 SCL=33 ~50kHz, chip id=0x36
```

`chip id=0x36` = FT6336G 正确应答，时序可用。

| 指标 | `i2c_master` 版 | GPIO 模拟版 |
|---|---|---|
| 触摸手感 | 40 ms 轮询，按下等一帧 | **更跟手**（用户实机反馈"比之前流畅"） |
| `i2c f=` 心跳 | 2.35 h 累计 3128 次失败 | **f=0 ok=0**（`i2c_driver` 计数器彻底不再被触碰） |
| 崩溃 | 710s / 6196s / 8453s 三次 IWDT | 连续运行无 `Interrupt wdt` |
| CPU 占用 | ISR 风暴时 100% 卡死 | 1.6 ms 忙等 / 次，可抢占 |

## 五、注意事项与回退

- **回退要改两处**：`LCD_Touch_Init()` 的签名在 `LCD_Touch.h` 和 `ui_disp.c` 各一处，
  必须一起改回去，否则编译不过。
- **手感或稳定性异常时先调 `FT_I2C_HALF_US`**：10（50 kHz，默认）→ 25（20 kHz）更稳；
  5（100 kHz）只在确认有外部 4.7kΩ 上拉后再用。
- **CTP_INT 若重新启用**：GPIO2 是 **strapping 引脚**，下载模式必须为高。
  **若某天进不了下载模式，先拔掉这根飞线。**
- **`LCD_TOUCH_LOGE` 永远不要跟着调试开关走**：这次踩过——写屏/读触摸失败的错误日志
  被 `LCD_TOUCH_DEBUG_LOGS=false` 编译成空宏，一次 `spi_master` 拒收把屏刷坏后
  **完全静默**，白跑 3 小时才发现。

## 六、排查工具备忘

- **coredump**：`idf.py coredump-info`（存于 flash `0xfe0000`，约 36 KB）。
  **必须用与板上固件一致的 ELF 解析**——曾经出现 `Checksum mismatch between flashed
  and built applications`，那时 `EPC1` 是红鲱鱼（panic 打印本身会覆盖它）。
- **level-1 ISR 里崩的，软件救不回来**：中断占着 CPU 时任何任务都调度不了，
  退避策略、看门狗喂狗都没机会执行。看到 `_xt_lowint1 → shared_intr_isr` 这类
  形态，直接往"中断源本身有问题"查，不要再在任务层找。
- **判据**：只要 backtrace 出现 `Interrupt wdt timeout on CPUx` 且两个核都在 IDLE，
  就是中断风暴，不是栈/不是内存/不是死锁。
