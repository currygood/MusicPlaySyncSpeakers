# Interrupt WDT：触摸 I2C 间歇 NACK 与共享中断自锁（软件侧降暴露）

> 崩溃现场（第 5 次，2026-10-26 17:40，up=710s）：
> `Guru Meditation Error: Core 0 panic'ed (Interrupt wdt timeout on CPU0)`
> PC=0x400960d9（`xPortSetInterruptMaskFromISR`，inline 进 `xPortEnterCriticalTimeout`），
> backtrace：`xPortEnterCriticalTimeout → vPortEnterCritical(inline) → shared_intr_isr(intr_alloc.c:467) → _xt_lowint1`；
> A2=A3=0xFFFFFFFF（= `portMUX_NO_TIMEOUT`）；CPU1 停在 IDLE1；
> 崩溃前最后一条心跳：`i2c f=2195 r=0 last=ESP_ERR_INVALID_RESPONSE`。
>
> 这是同一故障的第 5 次复发，前 4 次记录见 `docs/04-测试/主节点bug解决.md`。

## 一、五次崩溃串起来

| # | 日期 | up | 当时结论 | 现在回看 |
|---|---|---|---|---|
| 1~2 | 10-24 前 | — | core1 被唤醒词吃满、日志全停 | 是并存问题，不是本案 |
| 3 | 10-25 | 2850s | `i2c f=2848`，`spinlock_acquire` 无限等 | 同一故障，位置在 I2C ISR 内部的队列锁 |
| 4 | 10-26 | — | A3=0xCDCD=本核 owner，判断"锁状态错位/自锁" | 与第 5 次同源，都在**共享中断派发层** |
| 5 | 10-26 | 710s | 本文 | `r=0` 仍崩 ⇒ 推翻"clear_bus 是必要条件" |

## 二、第 5 次崩溃带来的三个新结论

### 1. `shared_intr_isr` 必然是 I2C 的派发层

读了 IDF 6.0.1 源码：

- `esp_driver_i2c/i2c_private.h:59`
  `I2C_INTR_ALLOC_FLAG = ESP_INTR_FLAG_SHARED | ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LOWMED`
  → **I2C 中断本身就是共享中断**；
- `LCD_Touch.h: LCD_TOUCH_FT6336_INT = -1`（10-18 起就 committed）
  → `gpio_install_isr_service` 从未被调用 → GPIO 中断从未注册；
- 两者相加：**本固件唯一的共享中断就是 I2C**，backtrace 里的
  `shared_intr_isr` 必然是它。之前怀疑的 GPIO 中断风暴可以排除。

### 2. `clear_bus / FSM reset` 不是必要条件

`r=0` 说明 `I2c_Reset_Bus` 一次都没被调用（新加的"连续 32 次才复位"门槛从未
达到），但崩溃照常发生。所以上一版"失败 → FSM reset → 危险窗口"的因果链
不成立，**真正的致命路径是 i2c_master 的软件超时分支本身**：
`xSemaphoreTake` 失败后仍然 `xSemaphoreGive`（i2c_master.c:535-542），凭空多一个
token → 任务与硬件 FSM 脱同步 → 中断在派发层"关中断 + 无限等锁"。

### 3. 不是普通的跨核锁竞争

两次崩溃 CPU1 都停在 IDLE1——没有"另一核长期持锁"的主体。更像派发层自锁/
重入或锁 owner 字段错位（与第 4 次 A3=0xCDCD=本核 的判断一致）。微机制靠这份
日志定不死，若再崩建议 `idf.py coredump-debug` 挂 GDB 看锁 owner。

### 教训：ELF 不一致时 backtrace 是假的

这次日志带 `Checksum mismatch between flashed and built applications`——板上固件
与本地 ELF 不同（build 产物 17:31 生成、设备 ~17:28 开机）。IDF 低地址段
（0x4008/0x4009）符号仍可信，app 段（0x4025xxxx）可能整体错位；
`EPC1 = rtc_gpio_pulldown_dis` 是红鲱鱼（panic 打印本身覆盖 EPC1，
第 3 次同样的位置是 `uart_hal_write_txfifo`）。**先烧同一构建，再信 backtrace。**

## 三、软件侧能做什么：把"事务数"当"样本数"管理

机制：IDF 6.0 的 `i2c_master` 同步 API 实为"队列 + 信号量 + ISR"，读 6 字节要
2~3 次 ISR 进出，每次都要抢 intr_alloc 的全局 spinlock。轮询 20ms = 50 事务/s ≈
150 次 ISR/s，按 6% NACK 率，每秒约 3 次把驱动推向脱同步。**降低事务数就是
降低崩溃概率**，这是不改硬件时唯一有效的抓手。

### 本次改动（2026-10-27）

| 文件 | 改动 | 理由 |
|---|---|---|
| `i2c_driver.c` | `I2C_TRANS_TIMEOUT_MS` 50 → **150ms** | 不命中"软件超时 → 凭空 Give"分支；150ms 远大于最坏调度延迟（LVGL 刷一屏 ~123ms） |
| `i2c_driver.c/.h` | `s_i2cStat` 新增 `ok_cnt`，`I2c_Write_Bytes` 补统计 | 只有 fail 数算不出失败率；失败率的量级决定查硬件还是查时序 |
| `LCD_Touch.c` | **删除 `I2c_Reset_Bus` 调用**及整套复位预算逻辑 | `r=0` 仍崩 ⇒ 无收益且占最危险窗口 |
| `LCD_Touch.c` | 轮询 `LCD_TOUCH_POLL_MS` **20 → 40ms** | 事务量砍半，手感无感（一次按下持续 80~150ms） |
| `LCD_Touch.c` | 连续失败 ≥200 次 → **5s 慢探测**（`LCD_TOUCH_BACKOFF_DEAD_MS`） | 故障期间事务量从 25/s 降到 0.2/s，只留"总线是否恢复"的探针 |
| `app_main.c` | 心跳 `i2c f= ok= r= last=` | f/ok 必须一起看（见下） |

读心跳的方法：
- 失败率 ≈ 5%（f 缓慢涨、ok 飞快涨）→ 信号完整性问题 → 查上拉/排线；
- 失败率 ≈ 90%（ok 几乎不涨）→ 从设备根本没应答 → 查供电/CTP_RST/接插件。

### 一个重要更正

`i2c_driver.h` 里 `I2C_FREQ` 本来就是 **100000（100kHz）**，不是最初以为的
400kHz。"降速验证"这个选项已经用掉了。**100kHz + 仅靠 S3 内部约 45kΩ 上拉
仍有 ~5% NACK**，基本可以把锅判给：外部上拉缺失/阻值太大、接插件接触、
CTP_RST 与 LCD_RST 共用 GPIO25 的复位余量。软件侧已无可让的空间。

## 四、改动后首跑数据（2026-10-27，46min）

```text
[hb] up=2744s reset=1 ... | i2c f=2590 ok=50601 r=0 last=ESP_ERR_INVALID_RESPONSE
```

| 指标 | 改动前 | 改动后 |
|---|---|---|
| 失败速率 | 3.1 次/s | **0.94 次/s**（≈1/3） |
| 单次事务失败率 | ~6% | **4.9%**（没变，电气问题） |
| 运行时长 | 710s 崩 | 2744s 无重启 |
| 音频链路 | — | resync=0、un=3 不涨、drop=0，健康 |

结论：**单次失败率没降，降的是失败次数 → 概率性改善，不是根治。**
只要还存在 I2C 事务，就还存在"偶发自锁"的可能，预期 MTBF 在"小时级"，
不承诺绝对不崩。

## 五、下一步（按优先级）

1. **24h 长跑**（进行中）：24h 无崩即可认为软件侧"压住"；
2. **硬件治本**：SDA/SCL 外挂 4.7kΩ 上拉（最可能的根因）；
3. **备用：飞线接 CTP_INT**（代码已留好启用说明，见 `LCD_Touch.h` 注释）：
   - FT6336G 的 INT 是**电平保持型**（trigger 模式 G_MODE=0x01 已配，
     读走 TD_STATUS 才释放）；
   - 必须用 `GPIO_INTR_NEGEDGE` + **保留 500ms 兜底轮询**：边沿型不会风暴，
     被 NACK 吃掉的边沿由兜底补；
   - **绝不用 `GPIO_INTR_LOW_LEVEL`**：NACK 时 INT 常低 → 持续中断风暴，
     而每次进 ISR 都要抢同一把 intr_alloc 全局 spinlock，风险反而上升；
   - ISR 里只 `vTaskNotifyGiveFromISR`（现有实现已正确），I2C 读留在任务里；
   - 长期启用建议 `gpio_install_isr_service(ESP_INTR_FLAG_IRAM)`。

## 六、排查工具备忘

- 想看 IDF 驱动原始错误：`i2c_driver.c` 的 `I2C_DRIVER_SILENCE_BUS_ERRORS` 改 0
  （每秒十几条刷屏，只建议短时开）；
- 再崩时：`idf.py coredump-info`（coredump 存于 flash 0xfe0000，36KB），
  注意必须用与板上固件一致的 ELF 解析，否则符号不可信；
- 触摸手感嫌钝：`LCD_TOUCH_POLL_MS` 改回 20（代价是暴露面翻倍）。

---

## 七、第 6 次崩溃 → 定位到 IDF 缺陷 → 本地补丁 + CTP_INT 启用（2026-10-27 晚）

24h 长跑在 **103min（up=6196s）** 处第 6 次崩，但这次 backtrace 第一次拍全了
IDF 内部调用链，把最后一块拼图钉死了：

```text
LCD_TOUCH_FT6336G_Process_Task            应用层：任务上下文读触摸
→ s_i2c_synchronous_transaction           i2c_master.c:1004
→ s_i2c_send_commands :566                status == I2C_STATUS_TIMEOUT（硬件超时）
→ s_i2c_hw_fsm_reset(clear_bus=true)      驱动内部自动 reset，不经应用
→ i2c_common_deinit_pins → gpio_set_level 抢 GPIO / 敲 SCL
→ 【level-1 中断插入】shared_intr_isr 抢锁死等，owner=0xCDCD（本核）
→ Interrupt wdt timeout on CPU0
```

### 新结论（推翻第五节的两处假设）

1. **`clear_bus` 不是必要条件，而且应用层关不掉。** IDF 在 `I2C_STATUS_TIMEOUT`
   后自己就会调 `s_i2c_hw_fsm_reset(clear_bus=true)`，而 reset 全过程 I2C 中断
   仍是使能的（掩码要到最后才清）→ 总线毛刺立刻触发共享中断 → 派发层持全局
   spinlock 跑 handler 链 → 与被打断的临界区同核自锁。
   **这是 IDF 6.0.1 `esp_driver_i2c` 的缺陷：fsm reset 没有先关中断。**
2. **软件侧降暴露只能降概率，不能断根**：103min 崩、失败率 6.15%、失败速率
   1.16 次/s，与首跑同量级 —— 收益兑现了（事务量降一个量级），但雷还在，
   每次硬件超时都是一张彩票。

### 处置（两件事同时做）

**① IDF 本地补丁（拆雷）**：`s_i2c_hw_fsm_reset` 进出各加一个
`esp_intr_disable/enable(base->intr_handle)`，共 4 行。
详见 `note/ESP-IDF6.0.1本地补丁-i2c_fsm_reset关中断.md`，补丁文件在
`note/patch/esp-idf-6.0.1-i2c-fsm-reset-intr.patch`
（**改的是共用工具链，升级/重装 IDF 必须重打**）。

**② CTP_INT 飞线启用（GPIO2）**：`LCD_TOUCH_FT6336_INT` -1 → 2，采样改为
"中断边沿 + 低频兜底轮询"，静止事务量从 25 次/s 降到 5 次/s：

| 位置 | 改动 | 原因 |
|---|---|---|
| `LCD_Touch.h` | `LCD_TOUCH_FT6336_INT = 2` | 飞线接入，启用已有中断路径 |
| `LCD_Touch.c` ISR | 加 `TouchTaskHandle == NULL` 守卫 | 中断在 Step 5 使能、任务在 Step 6 才创建；CTP_RST 共用 GPIO25，复位释放瞬间就可能拉低 INT → 原代码会解引用空指针 |
| `LCD_Touch.c` | `gpio_install_isr_service(0)` → `ESP_INTR_FLAG_IRAM` | SD 卡写会临时关 flash cache，非 IRAM ISR 此时触发直接 panic（`gpio_intr_service` 本身已是 IRAM_ATTR） |
| `LCD_Touch.c` | 成功读后兜底周期：静止 200ms / 有触摸 40ms | 边沿负责实时性，兜底只补被 NACK 吃掉的边沿与松手事件 |

触发方式**仍是 `GPIO_INTR_NEGEDGE`**（第五节的判断不变）：FT6336 的 INT 是
"读走 TD_STATUS 才释放"的电平信号，`LOW_LEVEL` 在 NACK 时会变成持续中断风暴，
而每次进 ISR 都要抢同一把 intr_alloc 全局 spinlock。

### 硬件注意事项

- ESP32 的 **GPIO2 是 strapping 引脚**，下载模式必须为高。CTP_INT 静止为高，
  正常无影响；**若某天无法进入下载模式，先拔这根飞线**。
- IO2 走线避开 SPI(15MHz)/I2S/功放输出，<10cm，MCU 端串 100~330Ω，共地。

### 验证判据（下一轮长跑）

- 启动日志出现 `Step 5: Setting up interrupt (GPIO2)... Interrupt configured (falling edge)`；
- 心跳 `i2c ok=` 增速从 ~26/s 降到 **~5/s**（只剩 200ms 兜底），摸屏时才涨快；
- **真正判据是崩不崩**：即使 6% NACK 仍在，硬件超时也不再能引发 IWDT。
  跑过一两天无 `Interrupt wdt`，此问题视为断根；
- 触摸手感应更跟手（按下 ms 级响应，不再等 40ms 轮询）。
