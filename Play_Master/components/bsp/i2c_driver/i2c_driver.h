#ifndef I2C_DRIVER_H
#define I2C_DRIVER_H

/* ======================== 是否打开 I2C 调试打印 ======================== */
#define I2C_DRIVER_DEBUG_LOGS    (false)

#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

// --- I2C硬件配置宏 ---
#define I2C_PORT I2C_NUM_0
#define I2C_SDA_GPIO 32
#define I2C_SCL_GPIO 33
#define I2C_FREQ 100000

// I2C 总线句柄
typedef struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
} i2c_handle_t;

// 初始化 I2C 总线
esp_err_t I2c_Init_Bus(i2c_port_t port, gpio_num_t sda_pin, gpio_num_t scl_pin, uint32_t freq_hz, i2c_master_bus_handle_t *bus_handle);

// 添加 I2C 设备
esp_err_t I2c_Add_Device(i2c_master_bus_handle_t bus_handle, uint16_t dev_addr, uint32_t freq_hz, i2c_master_dev_handle_t *dev_handle);

// 写寄存器
esp_err_t I2c_Write_Reg(i2c_master_dev_handle_t dev_handle, uint8_t reg, uint8_t data);

// 读寄存器
esp_err_t I2c_Read_Reg(i2c_master_dev_handle_t dev_handle, uint8_t reg, uint8_t *data);

// 读 FIFO 或多字节
esp_err_t I2c_Read_Bytes(i2c_master_dev_handle_t dev_handle, uint8_t reg, uint8_t *buffer, size_t count);

// 复位 I2C 控制器状态（用于 SDA/SCL 被从设备拉低后的软件恢复）
esp_err_t I2c_Reset_Bus(i2c_master_bus_handle_t bus_handle);

// 删除设备
esp_err_t I2c_Remove_Device(i2c_master_dev_handle_t dev_handle);

// 删除总线
esp_err_t I2c_Delete_Bus(i2c_master_bus_handle_t bus_handle);

// 全局I2C总线句柄获取函数
i2c_master_bus_handle_t I2c_Get_Global_Bus_Handle(void);

// 写多个字节
esp_err_t I2c_Write_Bytes(i2c_master_dev_handle_t dev_handle, uint8_t reg, uint8_t *buffer, size_t count);

/* I2C 总线错误快照
 *
 * 驱动内部的 i2c.master / i2c.common 日志被整体静音了（见 i2c_driver.c 的
 * I2C_DRIVER_SILENCE_BUS_ERRORS），一旦总线被从设备拉死，串口上完全看不到
 * 任何迹象 —— 而这个故障会造成任务侧长时间阻塞，是"系统级卡死"的头号嫌疑。
 * 这里提供无锁计数，由心跳任务按低频汇总打印。 */
typedef struct {
    uint32_t fail_cnt;    /* 累计失败事务数 */
    uint32_t ok_cnt;      /* 累计成功事务数：只有和 fail_cnt 一起看才知道**失败率** */
    uint32_t reset_cnt;   /* 累计总线软件复位次数 */
    int      last_err;    /* 最后一次错误码（esp_err_t） */
    int64_t  last_us;     /* 最后一次失败时刻（us），0 = 从未失败 */
} i2c_err_stats_t;

// 取总线错误统计（out 为空则忽略）
void I2c_Get_Err_Stats(i2c_err_stats_t *out);
#endif // I2C_DRIVER_H
