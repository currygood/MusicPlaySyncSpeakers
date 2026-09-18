/**
 * @file gpio_driver.c
 * @brief GPIO 通用驱动实现（封装 ESP-IDF GPIO 驱动）
 *
 * 本文件实现 BSP 层 GPIO 驱动的所有接口函数：
 *
 *   - Gpio_Init()           ：GPIO 引脚初始化（方向 + 上下拉配置）
 *   - Gpio_Set_Level()      ：设置输出电平
 *   - Gpio_Get_Level()      ：读取输入电平
 *   - Gpio_Toggle()         ：翻转输出电平
 *   - Gpio_Install_Isr_Service()：安装 ISR 服务（全局一次）
 *   - Gpio_Set_Intr_Type()  ：设置中断触发类型
 *   - Gpio_Isr_Handler_Add()：注册中断回调函数
 *   - Gpio_Intr_Enable()    ：使能引脚中断
 *
 * 所有 API 调用失败时自动通过 ESP_LOGE 输出错误日志，
 * 便于上层模块快速定位硬件问题。
 */

#include "gpio_driver.h"
#include "esp_log.h"

/* ======================== 私有变量 =============================================== */

/** 日志标签 */
static const char *TAG = "GPIO_Driver";

/* ======================== 基础操作实现 =========================================== */

/**
 * @brief 初始化 GPIO 引脚
 *
 * 根据 Gpio_Mode_t 枚举值构建 gpio_config_t 结构体并调用 ESP-IDF 底层接口。
 * 默认禁用中断，上下拉根据模式选择。
 */
esp_err_t Gpio_Init(gpio_num_t gpioNum, Gpio_Mode_t mode)
{
    gpio_config_t ioConf = {
        .pin_bit_mask = (1ULL << gpioNum),
        .intr_type = GPIO_INTR_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE
    };

    switch(mode)
    {
        case GPIO_OUTPUT:
            ioConf.mode = GPIO_MODE_OUTPUT;
            break;
        case GPIO_INPUT:
            ioConf.mode = GPIO_MODE_INPUT;
            break;
        case GPIO_INPUT_PULLUP:
            ioConf.mode = GPIO_MODE_INPUT;
            ioConf.pull_up_en = GPIO_PULLUP_ENABLE;
            break;
        case GPIO_OD_OUTPUT:
            ioConf.mode = GPIO_MODE_OUTPUT_OD;
            break;
        default:
            ESP_LOGE(TAG, "Invalid GPIO mode");
            return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = gpio_config(&ioConf);
    if(ret != ESP_OK)
    {
        ESP_LOGE(TAG, "GPIO %d init failed", gpioNum);
    }

    return ret;
}

/**
 * @brief 设置 GPIO 输出电平
 *
 * 直接调用 ESP-IDF 的 gpio_set_level() 接口。
 */
void Gpio_Set_Level(gpio_num_t gpioNum, uint8_t level)
{
    gpio_set_level(gpioNum, level);
}

/**
 * @brief 读取 GPIO 输入电平
 *
 * 直接调用 ESP-IDF 的 gpio_get_level() 接口并返回结果。
 */
uint8_t Gpio_Get_Level(gpio_num_t gpioNum)
{
    return gpio_get_level(gpioNum);
}

/**
 * @brief 翻转 GPIO 输出电平
 *
 * 原子操作：先读后写，确保翻转的原子性。
 */
void Gpio_Toggle(gpio_num_t gpioNum)
{
    uint8_t currentLevel = gpio_get_level(gpioNum);
    gpio_set_level(gpioNum, !currentLevel);
}

/* ======================== 中断操作实现 =========================================== */

/**
 * @brief 安装 GPIO 中断服务
 *
 * 封装 ESP-IDF 的 gpio_install_isr_service()，添加错误日志。
 * 此函数全局只需调用一次，重复调用返回 ESP_ERR_INVALID_STATE。
 */
esp_err_t Gpio_Install_Isr_Service(int flags)
{
    esp_err_t ret = gpio_install_isr_service(flags);
    if(ret != ESP_OK)
    {
        ESP_LOGE(TAG, "GPIO ISR service install failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

/**
 * @brief 设置 GPIO 中断触发类型
 *
 * 封装 ESP-IDF 的 gpio_set_intr_type()，支持边沿和电平触发模式。
 */
esp_err_t Gpio_Set_Intr_Type(gpio_num_t gpioNum, gpio_int_type_t intrType)
{
    esp_err_t ret = gpio_set_intr_type(gpioNum, intrType);
    if(ret != ESP_OK)
    {
        ESP_LOGE(TAG, "GPIO %d set intr type failed: %s", gpioNum, esp_err_to_name(ret));
    }
    return ret;
}

/**
 * @brief 注册 GPIO 中断处理函数
 *
 * 封装 ESP-IDF 的 gpio_isr_handler_add()，将用户回调绑定到指定引脚。
 */
esp_err_t Gpio_Isr_Handler_Add(gpio_num_t gpioNum, gpio_isr_t isrHandler, void *args)
{
    esp_err_t ret = gpio_isr_handler_add(gpioNum, isrHandler, args);
    if(ret != ESP_OK)
    {
        ESP_LOGE(TAG, "GPIO %d ISR handler add failed: %s", gpioNum, esp_err_to_name(ret));
    }
    return ret;
}

/**
 * @brief 使能 GPIO 中断
 *
 * 封装 ESP-IDF 的 gpio_intr_enable()，启用指定引脚的中断检测。
 */
esp_err_t Gpio_Intr_Enable(gpio_num_t gpioNum)
{
    esp_err_t ret = gpio_intr_enable(gpioNum);
    if(ret != ESP_OK)
    {
        ESP_LOGE(TAG, "GPIO %d intr enable failed: %s", gpioNum, esp_err_to_name(ret));
    }
    return ret;
}