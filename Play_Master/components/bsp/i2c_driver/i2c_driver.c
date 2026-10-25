#include "i2c_driver.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* I2C 事务超时（毫秒）：
 * -1 会永久阻塞（卡死时饿死任务、触发看门狗）；原来用 100ms 也偏大——
 * 触摸屏读 6 字节在 100kHz 下只需 ~0.6ms，一旦从设备不响应，100ms 纯属白等，
 * 而且失败后驱动还会做一次总线 FSM 复位（每次复位会刷一对
 * "GPIO xx is not usable" 告警）。实测触摸总线不通时每秒能刷 20 行日志，
 * 把真正有用的音频日志全冲掉。10ms 足够覆盖最坏情况（含被抢占）。 */
#define I2C_TRANS_TIMEOUT_MS  10

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
 * 32 位对齐的读写在本平台是原子的，因此不需要加锁。 */
static struct {
    volatile uint32_t fail_cnt;   /* 累计失败事务数 */
    volatile uint32_t reset_cnt;  /* 累计总线软件复位次数 */
    volatile uint32_t last_err;   /* 最后一次错误码（esp_err_t 数值） */
    volatile int64_t  last_us;    /* 最后一次失败时刻（us，0 表示从未失败） */
} s_i2cStat;

static inline void I2c_Note_Result(esp_err_t ret)
{
    if (ret == ESP_OK)
    {
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
    free(temp_buf);
    return ret;
}
