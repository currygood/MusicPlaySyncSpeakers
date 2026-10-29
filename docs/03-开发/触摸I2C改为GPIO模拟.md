# 触摸 I2C 改为 GPIO 模拟（2026-10-29）

> 涉及文件：`components/middlewares/LCD_Touch/LCD_Touch.c` / `LCD_Touch.h`、
> `components/App/UI/ui_disp.c`

## 1. 问题

长时间播放（实测 `up=8453s` ≈ 2.35 小时）后整机重启：

```
Guru Meditation Error: Core 0 panic'ed (Interrupt wdt timeout on CPU0)
PC : 0x40090f21  --- i2c_master_isr_handler_default  (i2c_master.c:805)
Backtrace: _xt_lowint1 -> shared_intr_isr -> i2c_master_isr_handler_default
Core 0 was running in ISR context:
EPC1 : uart_hal_write_txfifo
```

两个核都停在 `prvIdleTask`，UI 在崩溃前十几分钟没有任何重绘。

**关键细节：`i2c_master.c:805` 是 `portBASE_TYPE HPTaskAwoken = pdFALSE;`——函数序言。**
如果 ISR 里卡在某个循环，PC 会停在循环上；停在序言意味着**中断在被反复重入**（中断风暴）。

`CONFIG_ESP_INT_WDT_TIMEOUT_MS` 已经是 3000（默认 300 的 10 倍），说明风暴持续了 3 秒以上。

## 2. 为什么选"换驱动"而不是继续调参数

`i2c_master` 的每一次传输是：**命令队列 + 事件队列 + 信号量 + 异步 ISR**。
实测累计读失败率 **8.5%**（`i2c f=3128 / ok=33402`，`last=ESP_ERR_INVALID_RESPONSE`），
这个失败率足以把驱动偶发推进"任务与硬件 FSM 脱同步"的状态，之后中断在派发层被无限重入。

而 **level-1 中断占着 CPU 时，任何任务都调度不了**。所以：

- 轮询任务里的退避（40 → 80 → … → 5000ms）根本没机会执行
- `i2c_master_bus_reset()`（要在任务里跑）也执行不了
- 之前给 `s_i2c_hw_fsm_reset` 套的 `esp_intr_disable/enable` 本地补丁只护住那个窗口，
  而崩溃时走的根本不是那条路径（该补丁已于 2026-10-29 随 IDF 恢复官方源码而回退）

结论：**只要还用 `i2c_master`，就只能靠看门狗重启。** 所以改成纯 GPIO 模拟 I2C。

## 3. 实现

### 3.1 电气

| 项 | 值 |
|---|---|
| 引脚 | SDA = GPIO32，SCL = GPIO33（沿用 `bsp/i2c_driver.h` 的宏，不另立定义） |
| 模式 | `GPIO_MODE_INPUT_OUTPUT_OD`（开漏，写 1 = 释放，靠上拉拉高） |
| 上拉 | `GPIO_PULLUP_ENABLE`（内部约 45kΩ；板子没有外部上拉） |
| 速率 | 半周期 10µs ≈ **50kHz** |

**为什么要降到 50kHz**：45kΩ 上拉 + FPC 线容，上升沿需要几 µs，100kHz（半周期 5µs）
本来就不合规。降到 50kHz 后每个半周期有 10µs 余量，比原来稳。

### 3.2 时序

标准 5 个原语 + 2 个事务：

```
ft_i2c_start()        SCL 高电平期间 SDA 高→低；SDA 被拉死时返回 false
ft_i2c_stop()         SCL 高电平期间 SDA 低→高
ft_i2c_write_byte()   8 bit + 第 9 个时钟读 ACK
ft_i2c_read_byte()    8 bit + 主机回 ACK/NAK
ft_i2c_read_reg()     ADDR+W, REG, SR, ADDR+R, DATA×n
ft_i2c_write_reg()    ADDR+W, REG, VAL
```

`FT6336G 地址 0x38`（7 位），总线上发 `0x70` 写 / `0x71` 读。

### 3.3 总线恢复（现在可以安全做了）

从设备把 SDA 拉死时，敲 **9 个 SCL** 让它把剩余位输出完，再补一个 STOP。
只在**连续失败每 20 次**时执行一次（配合退避约 1~20 秒一次），单次开销约 200µs。

以前不敢做，是因为 `i2c_master_bus_reset()` 要把引脚从 I2C 外设手里抢过来、
再切回去、复位 FSM 寄存器——那是个危险窗口。现在引脚完全归应用所有，没有这个问题。

### 3.4 失败策略（保留原设计）

- 失败退避：40 → 80 → … → 1000ms
- 连续失败 ≥ 200 次：降到 5s 慢探测
- 读成功即恢复正常节奏（INT 模式 200ms 兜底 / 有触摸 40ms）

## 4. 代价与取舍

| 项 | 影响 |
|---|---|
| CPU | 一次 6 字节读 ≈ 9 字节 × 9 bit × 20µs ≈ **1.6ms** 忙等 |
| 任务 | `Touch_Task` prio=3、core0、栈 2048 —— 随时可被 UI/音频/协议栈抢占 |
| 负载 | 40ms 轮询时约 3% CPU；静止 200ms 时约 0.6% |
| 中断 | **零**。没有外设中断、没有队列、没有信号量 —— 这类崩溃不可能再发生 |
| 极限 | 最坏情况是一次读多花几毫秒然后超时返回，**不会把 CPU 卡死** |

## 5. 接口变化

```c
/* 以前：需要外部先创建 i2c_master 总线 */
void LCD_TOUCH_Init(i2c_master_bus_handle_t i2cBusHandle);
/* 现在：自己管引脚，不需要总线句柄 */
void LCD_TOUCH_Init(void);
```

`ui_disp.c` 里的 `I2c_Init_Bus(...)` 与 `s_i2cBus` 已删除。
`bsp/i2c_driver` 保留（未来其它 I2C 设备还能用），但当前**没有任何调用者**，
所以 `i2c_master` 的中断不会被注册。

## 6. 如何验证

上板后看心跳里的 `i2c f=`：

```
W (xxx) AppMain: [hb] up=... | i2c f=?? ok=?? r=0 last=ESP_ERR_INVALID_RESPONSE
```

- **成功判据**：`f=` 的增长速度应显著低于改造前的 8.5%（改造前 2.35 小时累计 3128 次失败）
- 触摸手感应与之前一致（INT 边沿 + 40ms 轮询不变）
- 连续跑 > 3 小时不再出现 `Interrupt wdt timeout`

如果 `f=` 仍然很高，说明是模组侧的问题（FPC 接触、CTP 供电、共用 RST 抖动），
下一步才是加 4.7kΩ 外部上拉或给 CTP 单独复位引脚。

## 7. 回退

把 `LCD_Touch.c` 里的 `ft_i2c_*` 调用换回 `I2c_Read_Bytes` / `I2c_Write_Reg`，
并同时改回 `LCD_TOUCH_Init(i2c_master_bus_handle_t)` 与 `ui_disp.c` 的总线创建。
两处签名必须一起改，否则编译不过。
