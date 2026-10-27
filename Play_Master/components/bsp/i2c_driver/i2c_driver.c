#include "i2c_driver.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* I2C 事务超时（毫秒）：
 *
 * 这个值的含义不是"总线要跑多久"，而是"任务最坏调度延迟"。
 * 读 6 字节在 100kHz 下只要 ~0.6ms，但任务等的是"中断把我叫醒"，
 * 所以真正的约束是被抢占多久。
 *
 * 曾经取 10ms（理由是"总线不通时别白等"），实测在播放音乐时会频繁误判：
 * 触摸任务优先级只有 3，core0 上还有蓝牙/共存中断，PSRAM 带宽又被解码和
 * LVGL 抢，实际调度延迟经常超过 10ms。一旦误判，命中的是 IDF 6.0 驱动的
 * **软件超时**分支，而那条分支有副作用：
 *     i2c_master.c:535   xSemaphoreTake(cmd_semphr, ticks) 失败
 *     i2c_master.c:541   xSemaphoreGive(cmd_semphr)   ← 没拿到却又给一个
 * 于是 cmd_semphr 上凭空多一个 token，后续事务 take 立刻返回，但硬件并没有
 * 产生事件 → 任务与硬件 FSM 脱同步 → 接下来走
 *     i2c_master.c:546   s_i2c_hw_fsm_reset(bus, clear_bus=true)
 * 也就是"抢 GPIO + 手敲 9 个 SCL + 复位 FSM 寄存器"。这段时间 I2C 中断仍然
 * 是活的，而该驱动的 ISR 会先关中断再无限等锁（xQueueGiveFromISR →
 * vPortEnterCritical → spinlock_acquire(portMUX_NO_TIMEOUT)），抢不到就是
 * Interrupt wdt timeout。所以"省那几十毫秒"换来的是把驱动推进失步状态，
 * 代价远高于多等几十毫秒。
 *
 * 结论：超时宁可给大也不能误判。-1 会永久阻塞（真挂死时饿死任务、触发
 * 任务看门狗），故取 150ms。失败后的重试节奏由调用方控制（LCD_Touch 的
 * 指数退避），不靠这里卡时间。
 *
 * 2026-10-27：50 → 150ms。第 5 次 INT WDT（up=710s / i2c f=2195 r=0 /
 * backtrace 停在 shared_intr_isr）证明了两件事：
 *   1. `r=0` —— 那次崩溃**根本没走 clear_bus / FSM reset**，所以"复位总线"
 *      不是必要条件，上一版把它当放大器是判断偏了；
 *   2. 真正致命的是"软件超时"这条分支本身（凭空多 Give 一个 token → 任务与
 *      硬件 FSM 脱同步 → 中断在派发层关中断无限等 intr_alloc 的全局 spinlock）。
 * 既然致命的是"误判超时"，那就把误判概率压到最低。150ms 远大于可观测的最坏
 * 调度延迟（LVGL 刷一整屏 ~123ms，而触摸任务本来就是低优先级、可以被拖）。
 * 代价只是"总线真挂时多等一会儿"，而失败后触摸任务本身就退避到 1s/5s 才读一次。 */
#define I2C_TRANS_TIMEOUT_MS  150

/* 是否屏蔽 I2C 驱动的总线错误日志（i2c.master / i2c.common）。
 * 触摸屏未接线或总线被拉死时，这两类日志会以每秒十几条的速率刷屏，
 * 严重干扰音频调试；真正的触摸状态由 LCD_Touch 自己的 W/E 日志体现。
 *
 * 注意：屏蔽日志 ≠ 放弃观测。下面 s_i2cStat 会**静默计数**每次事务失败，
 * 由心跳任务低频汇总输出（[hb] 里的 i2c f=/r=），既不失明也不刷屏。
 * 排查总线故障时可以临时把这里改成 0，把驱动原始错误放出来。 */
#define I2C_DRIVER_SILENCE_BUS_ERRORS  1

// 全局I2C总线句柄
static i2c_master_bus_handle_t global_i2c_bus = NULL;

/* 总线事务错误统计（写者：任意任务上下文；读者：心跳任务）
 * 32 位对齐的读写在本平台是原子的，因此不需要加锁。
 *
 * ok_cnt 是后补的：只有 fail 数没有成功数，是**算不出失败率**的 ——
 * "f=2195 但 710s 里成功了 3 万次"(6%) 和 "f=2195 而几乎全失败"(90%)
 * 是完全不同的两个故障，处置方向也相反（前者查信号完整性，后者查硬件连接）。 */
static struct {
    volatile uint32_t fail_cnt;   /* 累计失败事务数 */
    volatile uint32_t ok_cnt;     /* 累计成功事务数 */
    volatile uint32_t reset_cnt;  /* 累计总线软件复位次数 */
    volatile uint32_t last_err;   /* 最后一次错误码（esp_err_t 数值） */
    volatile int64_t  last_us;    /* 最后一次失败时刻（us，0 表示从未失败） */
} s_i2cStat;

static inline void I2c_Note_Result(esp_err_t ret)
{
    if (ret == ESP_OK)
    {
        s_i2cStat.ok_cnt++;
        return;
    }
    s_i2cStat.fail_cnt++;
    s_i2cStat.last_err = (uint32_t)ret;
    s_i2cStat.last_us  = esp_timer_get_time();
}

// 初始化 I2C 总线
esp_err_t I2c_Init_Bus(i2c_port_t port, gpio_num_t sda_pin, gpio_num_t scl_pin, uint32_t freq_hz, i2c_master_bus_handle_t *bus_handle) {
#if I2C_DRIVER_SILENCE_BUS_ERRORS
    /* 只关这两个驱动内部 tag，不影响业务侧（LCD_TOUCH / UI）自己的日志 */
    esp_log_level_set("i2c.master", ESP_LOG_NONE);
    esp_log_level_set("i2c.common", ESP_LOG_NONE);
#endif

    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = port,
        .sda_io_num = sda_pin,
        .scl_io_num = scl_pin,
        .flags = {
            .enable_internal_pullup = true,
        }
    };
    
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, bus_handle);
    if (ret == ESP_OK && bus_handle != NULL) {
        global_i2c_bus = *bus_handle; // 保存全局句柄
    }
    return ret;
}

// 添加 I2C 设备
esp_err_t I2c_Add_Device(i2c_master_bus_handle_t bus_handle, uint16_t dev_addr, uint32_t freq_hz, i2c_master_dev_handle_t *dev_handle) {
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = dev_addr,
        .scl_speed_hz = freq_hz,
    };
    return i2c_master_bus_add_device(bus_handle, &dev_cfg, dev_handle);
}

// 写寄存器
esp_err_t I2c_Write_Reg(i2c_master_dev_handle_t dev_handle, uint8_t reg, uint8_t data) {
    uint8_t buf[2] = {reg, data};
    esp_err_t ret = i2c_master_transmit(dev_handle, buf, sizeof(buf), I2C_TRANS_TIMEOUT_MS);
    I2c_Note_Result(ret);
    return ret;
}

// 读寄存器
esp_err_t I2c_Read_Reg(i2c_master_dev_handle_t dev_handle, uint8_t reg, uint8_t *data) {
    esp_err_t ret = i2c_master_transmit_receive(dev_handle, &reg, 1, data, 1, I2C_TRANS_TIMEOUT_MS);
    I2c_Note_Result(ret);
    return ret;
}

// 读多字节
esp_err_t I2c_Read_Bytes(i2c_master_dev_handle_t dev_handle, uint8_t reg, uint8_t *buffer, size_t count) {
    esp_err_t ret = i2c_master_transmit_receive(dev_handle, &reg, 1, buffer, count, I2C_TRANS_TIMEOUT_MS);
    I2c_Note_Result(ret);
    return ret;
}

// 删除设备
esp_err_t I2c_Remove_Device(i2c_master_dev_handle_t dev_handle) {
    return i2c_master_bus_rm_device(dev_handle);
}

// 删除总线
esp_err_t I2c_Delete_Bus(i2c_master_bus_handle_t bus_handle) {
    return i2c_del_master_bus(bus_handle);
}

// 获取全局I2C总线句柄
i2c_master_bus_handle_t I2c_Get_Global_Bus_Handle(void) {
    return global_i2c_bus;
}

// 复位 I2C 控制器状态（用于 SDA/SCL 被从设备拉低后的软件恢复）
esp_err_t I2c_Reset_Bus(i2c_master_bus_handle_t bus_handle) {
    if (bus_handle == NULL) return ESP_ERR_INVALID_ARG;
    s_i2cStat.reset_cnt++;
    return i2c_master_bus_reset(bus_handle);
}

// 取总线错误统计（供心跳任务低频打印；指针为空则忽略）
void I2c_Get_Err_Stats(i2c_err_stats_t *out) {
    if (out == NULL) return;
    out->fail_cnt  = s_i2cStat.fail_cnt;
    out->ok_cnt    = s_i2cStat.ok_cnt;
    out->reset_cnt = s_i2cStat.reset_cnt;
    out->last_err  = (int)s_i2cStat.last_err;
    out->last_us   = s_i2cStat.last_us;
}

// 在 i2c_driver.c 中添加
esp_err_t I2c_Write_Bytes(i2c_master_dev_handle_t dev_handle, uint8_t reg, uint8_t *buffer, size_t count) {
    // OLED 的逻辑通常是 [控制字节/寄存器] + [连续的数据]
    uint8_t *temp_buf = (uint8_t *)malloc(count + 1);
    if (temp_buf == NULL) return ESP_ERR_NO_MEM;
    
    temp_buf[0] = reg;
    memcpy(&temp_buf[1], buffer, count);
    
    esp_err_t ret = i2c_master_transmit(dev_handle, temp_buf, count + 1, I2C_TRANS_TIMEOUT_MS);
    I2c_Note_Result(ret);   /* 和其它接口保持一致：这条路径原来漏统计 */
    free(temp_buf);
    return ret;
}
