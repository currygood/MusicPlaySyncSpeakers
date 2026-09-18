/**
 * @file StartAndStop.h
 * @brief 系统开关机控制模块接口（软件定时器检测长按关机）
 *
 * 本模块实现基于 ESP32-S3 的开关机控制功能，设计要点：
 *
 *   1. 开机：SystemStart() 拉高 GPIO14，导通 PMOS 为系统供电；
 *   2. 关机：长按 GPIO36 按键 ≥ LONG_PRESS_MS（默认3秒），
 *      软件定时器周期采样检测，触发后拉低 GPIO14 截断 PMOS；
 *   3. 启动稳定期：上电后前 2 秒忽略按键输入，防止开机瞬间误触发；
 *   4. 防抖机制：按键释放时自动清零计数器，短按不触发关机。
 *
 * 硬件说明（当前板卡）：
 *   - 开关机按键    ：GPIO36（仅输入引脚，外部上拉 + 二极管隔离）
 *   - 电源控制      ：GPIO14 → NPN → PMOS 栅极（低电平关断电源）
 */

#ifndef __START_AND_STOP_H__
#define __START_AND_STOP_H__

#include "gpio_driver.h"

/* ======================== 宏定义 =============================================== */

/** 开关机按键引脚（GPIO34-39 为仅输入引脚，无内部上下拉） */
#define POWER_BTN_GPIO         GPIO_NUM_36

/** 电源控制引脚（PMOS 栅极驱动） */
#define SYSTEMCONTROL_GPIO     GPIO_NUM_14

/** 长按触发关机的时间阈值（毫秒） */
#define LONG_PRESS_MS          3000

/** 软件定时器扫描间隔（毫秒），越小响应越快但 CPU 占用越高 */
#define BTN_SCAN_INTERVAL_MS   20

/* ======================== API 函数 =============================================== */

/**
 * @brief 初始化开关机模块
 *
 * 配置 GPIO36 为输入模式（无内部上下拉），创建并启动 FreeRTOS 软件
 * 定时器用于周期性检测按键状态。调用后需等待 2 秒稳定期结束才有效。
 */
void StartAndStop_Init(void);

/**
 * @brief 系统开机
 *
 * 拉高 GPIO14 导通 PMOS，为系统供电。应在 app_main() 中尽早调用。
 */
void SystemStart(void);


#endif