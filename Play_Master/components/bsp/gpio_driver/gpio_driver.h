/**
 * @file gpio_driver.h
 * @brief GPIO 通用驱动接口（封装 ESP-IDF GPIO 驱动）
 *
 * 本模块将 ESP-IDF 的 GPIO 底层驱动封装为统一的 BSP 层接口，
 * 供上层 msystem、middlewares 等模块使用。设计要点：
 *
 *   1. 统一初始化接口：通过 Gpio_Mode_t 枚举配置输入/输出/上拉/开漏模式；
 *   2. 屏蔽底层细节：调用方无需关心 gpio_config_t 结构体和 ESP-IDF 版本差异；
 *   3. 支持中断服务：提供 ISR 服务安装、中断类型设置、中断使能等完整链路；
 *   4. 错误日志集成：所有 API 调用失败时自动输出 ESP_LOGE 日志。
 *
 * 使用示例：
 * @code
 *     // 配置输出引脚并拉高
 *     Gpio_Init(GPIO_NUM_2, GPIO_OUTPUT);
 *     Gpio_Set_Level(GPIO_NUM_2, 1);
 *
 *     // 配置输入引脚（带内部上拉）并读取电平
 *     Gpio_Init(GPIO_NUM_4, GPIO_INPUT_PULLUP);
 *     uint8_t level = Gpio_Get_Level(GPIO_NUM_4);
 *
 *     // 翻转输出电平
 *     Gpio_Toggle(GPIO_NUM_2);
 * @endcode
 *
 * @note GPIO34-39 为仅输入引脚，不支持输出模式和内部上下拉
 */

#ifndef GPIO_DRIVER_H
#define GPIO_DRIVER_H

#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"

/* ======================== 类型定义 =============================================== */

/**
 * @brief GPIO 工作模式枚举
 *
 * 定义 GPIO 引脚的四种工作模式，覆盖常见应用场景。
 */
typedef enum {
    GPIO_OUTPUT,         /**< 推挽输出模式（普通输出） */
    GPIO_INPUT,          /**< 浮空输入模式（需外部上拉/下拉） */
    GPIO_INPUT_PULLUP,   /**< 上拉输入模式（内部上拉，GPIO34-39 不支持） */
    GPIO_OD_OUTPUT       /**< 开漏输出模式（I2C 等总线场景） */
} Gpio_Mode_t;

/* ======================== 基础操作 API ========================================== */

/**
 * @brief 初始化 GPIO 引脚
 *
 * 根据指定模式配置 GPIO 引脚的方向、上下拉和中断类型（默认禁用中断）。
 * 此函数会完整配置引脚，重复调用可切换模式。
 *
 * @param gpioNum  GPIO 引脚号（gpio_num_t 枚举值）
 * @param mode     工作模式（Gpio_Mode_t 枚举）
 *
 * @return
 *         - ESP_OK：初始化成功
 *         - ESP_ERR_INVALID_ARG：无效的模式参数
 *         - 其他：ESP-IDF gpio_config() 返回的错误码
 *
 * @warning GPIO34-39 不支持 GPIO_INPUT_PULLUP 模式（无内部上拉电路）
 */
esp_err_t Gpio_Init(gpio_num_t gpioNum, Gpio_Mode_t mode);

/**
 * @brief 设置 GPIO 输出电平
 *
 * 将指定 GPIO 引脚设置为高电平(1)或低电平(0)。
 * 调用前必须先通过 Gpio_Init() 配置为输出模式。
 *
 * @param gpioNum  GPIO 引脚号
 * @param level    输出电平（0=低电平，1=高电平）
 */
void Gpio_Set_Level(gpio_num_t gpioNum, uint8_t level);

/**
 * @brief 读取 GPIO 输入电平
 *
 * 读取指定 GPIO 引脚当前的电平状态。
 * 调用前必须先通过 Gpio_Init() 配置为输入模式。
 *
 * @param gpioNum  GPIO 引脚号
 *
 * @return 当前电平（0=低电平，1=高电平）
 */
uint8_t Gpio_Get_Level(gpio_num_t gpioNum);

/**
 * @brief 翻转 GPIO 输出电平
 *
 * 读取当前电平后取反输出，实现电平翻转功能。
 * 常用于 LED 闪烁等场景。
 *
 * @param gpioNum  GPIO 引脚号（必须已配置为输出模式）
 */
void Gpio_Toggle(gpio_num_t gpioNum);

/* ======================== 中断操作 API ========================================== */

/**
 * @brief 安装 GPIO 中断服务
 *
 * 为 GPIO 中断分配 ISR 服务资源。在使用 GPIO 中断功能前必须调用此函数，
 * 且整个系统只需调用一次（通常在系统初始化阶段）。
 *
 * @param flags  ISR 服务标志（通常填 0，表示不注册到默认 ISR 服务）
 *
 * @return
 *         - ESP_OK：安装成功
 *         - ESP_ERR_INVALID_STATE：ISR 服务已安装
 *         - ESP_ERR_NO_MEM：内存不足
 */
esp_err_t Gpio_Install_Isr_Service(int flags);

/**
 * @brief 设置 GPIO 中断触发类型
 *
 * 配置指定 GPIO 引脚的中断触发方式（上升沿、下降沿、双边沿、低电平、高电平）。
 *
 * @param gpioNum    GPIO 引脚号
 * @param intrType   中断类型（gpio_int_type_t 枚举）
 *
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t Gpio_Set_Intr_Type(gpio_num_t gpioNum, gpio_int_type_t intrType);

/**
 * @brief 注册 GPIO 中断处理函数
 *
 * 为指定 GPIO 引脚绑定 ISR 回调函数。当该引脚触发中断时，
 * 系统自动调用注册的处理函数。
 *
 * @param gpioNum      GPIO 引脚号
 * @param isrHandler   中断处理函数指针（gpio_isr_t 类型）
 * @param args         传递给处理函数的用户参数（可为 NULL）
 *
 * @return ESP_OK 成功，其他值表示失败
 *
 * @note 处理函数应在 ISR 上下文中快速执行，避免阻塞操作
 */
esp_err_t Gpio_Isr_Handler_Add(gpio_num_t gpioNum, gpio_isr_t isrHandler, void *args);

/**
 * @brief 使能 GPIO 中断
 *
 * 启用指定 GPIO 引脚的中断检测。调用前应已完成：
 *   1. Gpio_Install_Isr_Service() 安装 ISR 服务
 *   2. Gpio_Set_Intr_Type() 设置中断类型
 *   3. Gpio_Isr_Handler_Add() 注册处理函数
 *
 * @param gpioNum  GPIO 引脚号
 *
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t Gpio_Intr_Enable(gpio_num_t gpioNum);

#endif