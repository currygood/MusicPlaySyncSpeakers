# 播放中突然"卡死" —— Core1 被唤醒词检测吃满

> 日期：2026-10-24 ｜ 项目：Play_Master（CallPhone / sync_protocol）
> 结论：**不是死锁。Core1 被 Wakenet9 唤醒词检测长期 100% 占用，IDLE1 饿死、音频链路失去全部抖动余量；再叠上一段无上限忙等，就表现为"日志全停"。**

---

## 1. 现象

- 播放一段时间后突然没声音；
- **串口日志同时全部停止**（连 `[btpcm]` / `[hb]` 都没有）；
- 手机侧仍显示"已连接、正在播放"，并且持续给主节点发音频；
- 当时 `CONFIG_ESP_TASK_WDT_EN` 没开，什么都没报；
- 第一判断：手机一直在发、主节点不收 → 99% 是死锁。

## 2. 为什么"日志全停"不等于死锁

先想清楚一件事：**"什么都没有"本身不携带任何信息**。三种完全不同的故障，在串口上的表现是一样的"静默"：

| 真实情况 | 串口表现 |
| --- | --- |
| 真的死锁（任务卡在锁上） | 静默 |
| 任务被更高优先级饿死 | 静默 |
| 整机复位过（手机还显示已连接几秒） | 静默（重启日志被丢） |

所以第一步不是猜，是**补观测**。

## 3. 补的三层观测

### 3.1 打开任务看门狗

```text
CONFIG_ESP_TASK_WDT_EN=y
CONFIG_ESP_TASK_WDT_TIMEOUT_S=5
CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=y
CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=y
```

然后让三个音频任务订阅（`esp_task_wdt_add(NULL)` + 主循环里 `esp_task_wdt_reset()`）：

- `bt_audio_task`（生产者）
- `SyncTx_Task`（发包）
- `SyncLocalPlay_Task`（出声最后一环）

它们卡住 5s，TWDT 会打印**所有任务的状态和 backtrace**，直接指出卡在等锁、等 I2S 还是忙等。退出前记得 `esp_task_wdt_delete(NULL)`，否则 TWDT 会报"任务不存在"。

### 3.2 独立心跳 `app_hb`

关键设计：心跳任务**不能依赖任何音频任务**。原来 `[btpcm]` 是在 `bt_audio_task` 的 PCM 回调里打印的 —— 这个任务一卡，日志链跟着一起静默，等于没有诊断。

`app_hb`（core0 / prio2 / 3s）只读两个单调计数器：

```text
[hb] a2dp_cb=.. beat=.. | sync st=.. dq=.. sil=.. | cp st=.. proc=../..us over=..
```

`a2dp_cb`（协议栈数据回调次数）和 `beat`（bt_audio 任务主循环圈数）组合能唯一区分：

| a2dp_cb | beat | 结论 |
| --- | --- | --- |
| 停 | 停 | 手机/链路不再送数据（host 根本没收到） |
| 涨 | 停 | `bt_audio_task` 自己卡住（被下游锁死） |
| 停 | 涨 | 数据没到，但任务活着 |

再加上 `reset=esp_reset_reason()`：**重启后手机端往往还会显示"已连接"几秒**，很容易把"其实重启过"误判成"卡死"。

### 3.3 别再被日志刷屏盖住

同一时期 I2C 触摸不通，`i2c.master` / `i2c.common` 每秒刷 20 行，把真正的音频日志全冲掉了。改了：事务超时 100 → 10ms，`esp_log_level_set("i2c.master"/"i2c.common", ESP_LOG_NONE)`，触摸读失败指数退避 20 → 1000ms。

## 4. 根因

WDT backtrace **每次都停在同一个地方**：

```text
CallPhone_Listening()  →  afe_fetch()  →  wakenet9 ...
IDLE1 (CPU 1) 未被喂狗
```

三处叠加：

### 4.1 零阻塞空转（主因）

`Microphone_Read_Pcm16()` 在 FIFO 没数据时是**立刻返回 0**，不是阻塞。而 LISTENING 分支原来等都不等就进下一轮：

```c
case CALL_PHONE_STATE_LISTENING:
    CallPhone_Listening(cp);   /* 读空立刻返回 → 立刻下一轮 → 死循环 */
    break;
```

主循环退化成零阻塞的忙循环，**core1 被 100% 占满**，IDLE1 一次都轮不到。这不是"唤醒词太重"，是"根本没让它睡"。

### 4.2 AFE 档位过高

`AFE_MODE_HIGH_PERF` 是为 AEC/NS 这类多算法串联准备的档位。本模块 AEC/SE/NS/VAD/AGC **全部关闭**，只跑唤醒词 —— 高精度档位换不来任何效果，却要多花近一倍 CPU。

### 4.3 单帧超预算后不会自愈

一帧是 16ms 音频（16k × 256）。处理一旦 ≥16ms：

- 麦克风 FIFO 持续积压 → 之后**永远读得到** → 更不会进"读空让出"的分支；
- 于是占用率不降反升，彻底锁死在 100%。

### 4.4 真正的"日志全停"形态

音频链路（`SyncLocalPlay` prio12、`SyncTx` prio11 都可能在 core1）被饿死只是"卡顿"；而 `sync_play_task` 里有一段**无上限忙等**：

```c
/* 旧实现 */
while (esp_timer_get_time() < play_at) { }   /* prio 12，无上限 */
```

`play_at` 由延迟队列里的 `t_play_us` 换算。正常只在"差 2ms 以内"才进这个循环；但队列混进一条异常时间戳（换流残留/被写坏）时，这里会把 core1 彻底锁死 —— 这才是"串口一个字都没有"的真正形态。

## 5. 修复

| 文件 | 改动 |
| --- | --- |
| `call_phone.c` | `AFE_MODE_HIGH_PERF` → `AFE_MODE_LOW_COST` |
| `call_phone.c` | `CallPhone_Listening()` 改返回 bool，读空时 `vTaskDelay(2ms)` |
| `call_phone.c` | 单帧处理 `≥ frame_us` 主动让出 1 tick（约 10ms） |
| `call_phone.c` | 新增 `call_phone_get_load()`，输出 `frame_us / proc_avg_us / proc_max_us / overrun_cnt` |
| `sync_protocol.c` | `play_at` 忙等加 **10ms 硬上限** |
| `sync_protocol.c` | `SYNC_PLAY_WRITE_TIMEOUT_MS` 500 → 100（一帧只 15ms，等半秒只会把故障拖成"卡死"） |
| `sync_protocol.c` | `sync_feed_silence()` 严格按 `frame_ms` 节拍补静音 |
| `sync_protocol.c` | `sync_reset_stream()` 加 `s_push_lock` 保护 |
| `app_main.c` | UI 优先级 12 → 10；新增 `app_hb` 心跳；`[btpcm]` 诊断 |
| `i2c_driver.c` / `LCD_Touch.c` | 见 3.3 |

### 为什么"降唤醒词帧率"是可接受的

让出 1 tick 后，唤醒词大约按一半帧率处理（约 31 帧/s）。"你好小智"这类唤醒词跨越 500ms 以上，半帧率仍然能命中。用一点唤醒灵敏度换整机不卡死，很划算。

## 6. 验证

看 `[hb]` 这一段的 `cp` 部分：

```text
cp st=1 f/s=62 proc=9200/15400us over=0
```

| 指标 | 期望 | 含义 |
| --- | --- | --- |
| `frame_us` | 16000 | 实时预算（一帧音频时长） |
| `f/s` | ≈62 | 满速是 `1e6 / 16000` |
| `proc avg/max` | max < 16000 | 峰值顶到或超过 16000 就是 core1 已超载 |
| `over` | 0 | 持续增长 = 还在超预算 |

同时 WDT 不再报 `IDLE1`；手机断连/重连、切歌、暂停恢复都不再静默。

若 `over` 仍然持续增长，下一步是在**音乐播放期间把唤醒检测降频或直接暂停**（`CallPhone_Stop` / 降低 feed 频率），播放结束再恢复。

## 7. 通用教训

1. **"卡死"先补观测，再谈根因**。心跳任务必须独立于被观测的任务，否则一起沉默。
2. **任何读接口都要确认它是阻塞还是立刻返回**。非阻塞读 + `while(1)` 不 delay = 100% 占用，这是嵌入式最常见的"假死"。
3. **任何面向截止时间的忙等都必须有硬上限**。`while (now < deadline) {}` 在实时优先级上等于一颗定时炸弹。
4. **优先级要成体系地看**：唤醒词检测在 core1 上是唯一**没有硬截止时间**的任务，它必须给音频让路，而不是抢路。
5. **看门狗默认是关的**，出问题前就该开。代价是几 KB 代码，收益是"能知道死在哪"。

## 8. 关联笔记

- `docs/04-测试/主节点bug解决.md` —— 本章完整版；
- 《唤醒词与AFE重采样修复笔记》—— AFE 模式与重采样的背景；
- 《HFP语音链路调试笔记》—— `CallPhone` 状态机来源。
