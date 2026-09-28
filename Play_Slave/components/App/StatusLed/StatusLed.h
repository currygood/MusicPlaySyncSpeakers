/**
 * @file StatusLed.h
 * @brief 状态 LED 模块接口
 *
 * 核心功能： 让MusicPlay调用，如果正在播放音乐，点亮LED，否则熄灭LED
 * 
 * @note LED为高电平有效，高电平亮，低电平熄灭
 */
#ifndef __STATUS_LED_H__
#define __STATUS_LED_H__


#include "esp_err.h"
#include "gpio_driver.h"

#define STATUS_LED_GPIO 15


#define STATUS_LED_ON  1
#define STATUS_LED_OFF 0

/**
 * @brief 初始化状态 LED
 * 
 * @return ESP_OK 初始化成功
 * @return ESP_ERR_INVALID_ARG GPIO 引脚无效
 * @return ESP_FAIL 初始化失败
 */
esp_err_t StatusLed_Init(void);

/**
 * @brief 点亮状态 LED
 * 
 * @return ESP_OK 点亮成功
 * @return ESP_ERR_INVALID_ARG GPIO 引脚无效
 * @return ESP_FAIL 点亮失败
 */
esp_err_t StatusLed_On(void);

/**
 * @brief 熄灭状态 LED
 * 
 * @return ESP_OK 熄灭成功
 * @return ESP_ERR_INVALID_ARG GPIO 引脚无效
 * @return ESP_FAIL 熄灭失败
 */
esp_err_t StatusLed_Off(void);

#endif
