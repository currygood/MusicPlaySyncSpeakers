/**
  * @file    relay.c
  * @brief   继电器驱动 —— GPIO0 拉高 / 拉低
  *
  * 对外接口和那两个硬件警告见 relay.h。
  *
  * @see     学习笔记/ESP8266开发流程.md §9.8
  */

#include <stdio.h>

#include "driver/gpio.h"

#include "relay.h"

/** @brief 继电器接在 GPIO0 上 */
#define RELAY_GPIO   GPIO_NUM_0

/** @brief 吸合：GPIO0 拉高 */
#define RELAY_ON     1

/** @brief 释放：GPIO0 拉低 */
#define RELAY_OFF    0

void relay_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1 << RELAY_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    /* gpio_config() 只是"使能输出驱动器"，它并不写电平；但驱动器一开，
       引脚立刻开始输出【输出寄存器里已有的值】，而该寄存器复位后是 0。
       所以上一句返回时 GPIO0 已经是低的了。

       下面这句是把这个状态【明确写出来】，而不是在纠正什么 ——
       别因为"看起来多余"就删掉：它表明"上电为关"是设计，不是巧合。 */
    gpio_set_level(RELAY_GPIO, RELAY_OFF);
}

void relay_on(void)
{
    gpio_set_level(RELAY_GPIO, RELAY_ON);
    printf("[relay] 开灯 —— GPIO0 拉高\n");
}

void relay_off(void)
{
    gpio_set_level(RELAY_GPIO, RELAY_OFF);
    printf("[relay] 关灯 —— GPIO0 拉低\n");
}

bool relay_get_state(void)
{
    /* 输出模式下 gpio_get_level() 读到的就是最后写进去的电平，
       所以这里直接读，不必额外维护一个状态变量。 */
    return gpio_get_level(RELAY_GPIO) == RELAY_ON;
}
