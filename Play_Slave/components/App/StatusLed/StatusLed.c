/**
 * @file StatusLed.c
 * @brief 状态 LED 模块实现
 *
 * 核心功能： 让MusicPlay调用，如果正在播放音乐，点亮LED，否则熄灭LED
 * 
 * @note LED为高电平有效，高电平亮，低电平熄灭
 */
#include "StatusLed.h"


/**
 * @brief 初始化状态 LED
 * 
 * @return ESP_OK 初始化成功
 * @return ESP_ERR_INVALID_ARG GPIO 引脚无效
 * @return ESP_FAIL 初始化失败
 */
esp_err_t StatusLed_Init(void)
{
	// 初始化 GPIO 引脚为输出模式
	return Gpio_Init(STATUS_LED_GPIO, GPIO_OUTPUT);
}

/**
 * @brief 点亮状态 LED
 * 
 * @return ESP_OK 点亮成功
 * @return ESP_ERR_INVALID_ARG GPIO 引脚无效
 * @return ESP_FAIL 点亮失败
 */
esp_err_t StatusLed_On(void)
{
	Gpio_Set_Level(STATUS_LED_GPIO, STATUS_LED_ON);
	return ESP_OK;
}

/**
 * @brief 熄灭状态 LED
 * 
 * @return ESP_OK 熄灭成功
 * @return ESP_ERR_INVALID_ARG GPIO 引脚无效
 * @return ESP_FAIL 熄灭失败
 */
esp_err_t StatusLed_Off(void)
{
	Gpio_Set_Level(STATUS_LED_GPIO, STATUS_LED_OFF);
	return ESP_OK;
}