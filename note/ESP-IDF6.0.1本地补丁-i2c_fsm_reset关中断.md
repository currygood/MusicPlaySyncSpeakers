# ESP-IDF 6.0.1 本地补丁：`i2c_master` FSM reset 期间关中断

> ## ⛔ 已回退（2026-10-29）—— 本文仅作备查
>
> 触摸已整体改为 **GPIO 模拟 I2C**（见
> `note/触摸I2C间歇NACK-InterruptWDT自锁-软件侧降暴露.md`
> 与 `docs/03-开发/触摸I2C改为GPIO模拟.md`），工程内**已没有任何 `i2c_master` 调用者**，
> 所以这个补丁护住的那段代码已经走不到。留在共用工具链上只会变成
> "看不见的差异"——换机器/重装 IDF 后它静默消失，谁也不知道。
>
> **当前状态**：`F:/esp_idf_6.0.1/esp_tool/v6.0.1/esp-idf` 工作区与官方 v6.0.1
> **完全一致**（`git -C <IDF> status --porcelain` 输出为空即可验证，该目录是 git 仓库，
> HEAD = `8c19b1560 change(version): Update version to 6.0.1`）。
>
> **什么时候需要重新打**：只有当某个工程重新开始用 `i2c_master` 时
> （接传感器、第二块触摸、或改用官方 `esp_lcd_touch` 驱动）。打之前先读上面那份笔记的
> 2.3 节——它只护住"FSM reset"一个窗口，第 7 次崩溃就是换了个入口。
> 重打步骤见本文第五节。

> 位置：`F:/esp_idf_6.0.1/esp_tool/v6.0.1/esp-idf/components/esp_driver_i2c/i2c_master.c`
> 补丁文件：`note/patch/esp-idf-6.0.1-i2c-fsm-reset-intr.patch`（标准 git diff，可重打）
> 建立日期：2026-10-27 · 回退日期：2026-10-29
> **这份 IDF 是共用工具链**（Play_Master / Play_Slave 都编译在它上面），
> 所以当时才需要写这份文档：影响面不止一个工程。

---

## 一、为什么要打这个补丁（唯一原因）

2026-10-27 的第 6 次崩溃（`up=6196s`）backtrace 第一次拍全了 IDF 内部调用链：

```text
LCD_TOUCH_FT6336G_Process_Task        LCD_Touch.c（应用层：任务上下文读触摸）
→ s_i2c_asynchronous_transaction      i2c_master.c:931
→ s_i2c_synchronous_transaction       i2c_master.c:1004
→ s_i2c_send_commands                 i2c_master.c:566     ← status == I2C_STATUS_TIMEOUT
→ s_i2c_hw_fsm_reset(clear_bus=true)  i2c_master.c:124/132
→ i2c_common_deinit_pins              i2c_common.c:446
→ i2c_bus_occupied → gpio_set_level   i2c_common.c:153 / gpio.c:250
→ 【level-1 中断插入】_xt_lowint1 → shared_intr_isr → 抢锁死等
   A15 = 0xCDCD（= 本核 CPU id，即 spinlock 的 owner 就是被打断的这颗核）
→ tick 停 → Interrupt wdt timeout on CPU0 → panic
```

关键事实（对照 IDF 6.0.1 源码逐行核实）：

1. 总线被从机拉住时，硬件报 `I2C_STATUS_TIMEOUT`，`s_i2c_send_commands` 会**在任务上下文里**
   直接做 `s_i2c_hw_fsm_reset(clear_bus=true)`；
2. 这个 reset 会：抢 GPIO → 手动敲最多 9 个 SCL → 切回 I2C 外设 → 复位外设寄存器
   → 重新配时序/滤波。**整个过程 I2C 中断一直是使能的**，中断掩码要到最后
   （`i2c_ll_disable_intr_mask`）才清掉；
3. reset 期间的总线毛刺会**立刻触发 I2C 中断**；I2C 中断是共享中断
   （`I2C_INTR_ALLOC_FLAG` = `ESP_INTR_FLAG_SHARED | ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LOWMED`），
   `shared_intr_isr()`（`intr_alloc.c:459-477`）**持着 intr_alloc 全局 spinlock 跑完整个 handler 链**；
4. 被打断的那段代码本身正处在临界区（自旋锁 owner 记录的是本核）→ ISR 抢同一把锁 →
   `portMUX_NO_TIMEOUT` 无限重试 → **同核自锁**，而它第一步就把 level 1~3 全屏蔽了，
   喂 INT WDT 的 tick 也在里面 → 300ms 后 panic。

一句话：**这是 IDF 6.0.1 `esp_driver_i2c` 的缺陷 —— fsm reset 没有先关自己的中断。**
应用层无法规避（`r=0` 证明：我们自己的 `I2c_Reset_Bus` 一次都没调用过，照样崩）。

---

## 二、补丁内容（完整 diff）

```diff
--- a/components/esp_driver_i2c/i2c_master.c
+++ b/components/esp_driver_i2c/i2c_master.c
@@ -109,6 +109,23 @@ static esp_err_t s_i2c_hw_fsm_reset(i2c_master_bus_handle_t i2c_master, bool cle
 {
     esp_err_t ret = ESP_OK;
     i2c_hal_context_t *hal = &i2c_master->base->hal;
+    /* [LOCAL PATCH 2026-10-27] Fix Interrupt WDT deadlock during FSM reset.
+     * ...（见源码注释，英文说明来龙去脉） */
+    if (i2c_master->base->intr_handle != NULL) {
+        esp_intr_disable(i2c_master->base->intr_handle);
+    }
 #if !I2C_LL_SUPPORT_HW_FSM_RST
@@ -140,6 +157,10 @@ ...
 #endif
+    /* [LOCAL PATCH 2026-10-27] re-enable I2C interrupt after reset window. */
+    if (i2c_master->base->intr_handle != NULL) {
+        esp_intr_enable(i2c_master->base->intr_handle);
+    }
     return ret;
 }
```

实质就是 4 行代码：**进入 reset 前 `esp_intr_disable`，退出前 `esp_intr_enable`**。

细节说明：

- 中断句柄取自 `i2c_master->base->intr_handle`（`i2c_private.h:117`，由
  `esp_intr_alloc_intrstatus` 在 `i2c_master.c:1147` 填充），带 `!= NULL` 守卫，
  避免总线尚未分配中断时误用；
- 两个分支（`I2C_LL_SUPPORT_HW_FSM_RST` 的硬件 reset / 软件 reset）都被包住，
  所以 ESP32 / ESP32-S3 / C3 等系列行为一致；
- `esp_intr_disable()` 是**跨核安全的中断控制器级屏蔽**，可在任务上下文调用，
  只在 reset 的几百微秒内生效，对其它外设无影响；
- 补丁与版本相关：`i2c_master.c` 行号在其它 IDF 版本会偏，但
  `s_i2c_hw_fsm_reset` 这个函数名一直没变，按函数找即可；
- **新版 IDF 上游可能已修**，升级后先查该函数开头有没有 `esp_intr_disable`，
  有就不要再打（避免重复/冲突）。

---

## 三、这个补丁解决了什么、没解决什么

| | 状态 |
|---|---|
| `Interrupt wdt timeout on CPU0` 因 I2C 硬件超时触发 | **解决**：reset 窗口内中断进不来，同核自锁不再可能 |
| 触摸 I2C 的 ~5% NACK / 偶发读失败 | **没解决**，也不该由 IDF 解决（电气问题：100kHz + 仅内部 ~45kΩ 上拉） |
| `I2C_STATUS_TIMEOUT` 本身 | 仍会发生，只是不再致命（reset 后驱动自愈，触摸继续可用） |
| 其它可能的 IWDT 来源（PSRAM 自旋锁、UART ISR 等） | 与本次无关，未处理 |

结论：**这是给"偶发电气故障"加的一道隔离带**，不是修电气问题。
硬件侧 SDA/SCL 外挂 4.7kΩ 上拉仍是治本项。

---

## 四、如何确认补丁在生效

1. 源码检查（最快）：

```bash
grep -n "LOCAL PATCH 2026-10-27" \
  F:/esp_idf_6.0.1/esp_tool/v6.0.1/esp-idf/components/esp_driver_i2c/i2c_master.c
# 期望输出 2 行：第 112 行附近、第 160 行附近
```

2. git 检查（IDF 目录是 git 仓库）：

```bash
cd F:/esp_idf_6.0.1/esp_tool/v6.0.1/esp-idf && git status --porcelain components/esp_driver_i2c/
# 期望： M components/esp_driver_i2c/i2c_master.c
```

3. 运行期：**没有正面日志**（补丁是静默的）。只能反证 —— 长时间运行 + 触摸
   依然偶发 `i2c f=` 增长（说明 NACK 还在），但**不再出现 `Interrupt wdt`**，
   即补丁在工作。

---

## 五、重新应用 / 回滚

### 方式 A：打补丁文件（推荐，最快）

```bash
cd F:/esp_idf_6.0.1/esp_tool/v6.0.1/esp-idf
git apply --check F:/EmbeddedProject/MusicPlaySyncSpeakers/note/patch/esp-idf-6.0.1-i2c-fsm-reset-intr.patch
git apply         F:/EmbeddedProject/MusicPlaySyncSpeakers/note/patch/esp-idf-6.0.1-i2c-fsm-reset-intr.patch
```

`--check` 失败说明版本/上下文已变，改用方式 B。

### 方式 B：按函数手工加 4 行

打开 `components/esp_driver_i2c/i2c_master.c`，找到

```c
static esp_err_t s_i2c_hw_fsm_reset(i2c_master_bus_handle_t i2c_master, bool clear_bus)
{
    esp_err_t ret = ESP_OK;
    i2c_hal_context_t *hal = &i2c_master->base->hal;
#if !I2C_LL_SUPPORT_HW_FSM_RST
```

在 `#if !I2C_LL_SUPPORT_HW_FSM_RST` **之前**插入：

```c
    if (i2c_master->base->intr_handle != NULL) {
        esp_intr_disable(i2c_master->base->intr_handle);
    }
```

再找到该函数结尾（`#endif` 之后、`return ret;` 之前）插入：

```c
    if (i2c_master->base->intr_handle != NULL) {
        esp_intr_enable(i2c_master->base->intr_handle);
    }
```

### 回滚

```bash
cd F:/esp_idf_6.0.1/esp_tool/v6.0.1/esp-idf
git checkout -- components/esp_driver_i2c/i2c_master.c   # 丢弃本文件全部本地改动
```

注意：`git checkout` 会连带丢掉该文件其它本地修改（目前只有这一处补丁）。

---

## 六、升级 / 换 IDF 时的检查清单

1. 新 IDF 是否已修：查 `s_i2c_hw_fsm_reset` 开头有无 `esp_intr_disable`；
2. 没修 → 按方式 A 打补丁（先 `--check`）；
3. 已修 → 不要打，改为记录"新版已修，补丁下线"；
4. 无论哪种，升级后都要跑一次触摸长时间测试，确认没有 `Interrupt wdt`；
5. `idf.py fullclean` 后重新编译，避免旧对象文件残留（改的是 IDF 组件，
   增量编译有时不会重编 `esp_driver_i2c`）。

---

## 七、相关文件

- 故障全过程与五次/六次崩溃对照：`note/触摸I2C间歇NACK-InterruptWDT自锁-软件侧降暴露.md`
- 应用侧改动（超时 150ms、删 `I2c_Reset_Bus`、CTP_INT 中断驱动）：
  `Play_Master/components/bsp/i2c_driver/i2c_driver.c`、
  `Play_Master/components/middlewares/LCD_Touch/LCD_Touch.{c,h}`
- 测试记录：`docs/04-测试/主节点bug解决.md` 末节
