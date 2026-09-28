/**
 * @file StartAndStop.c
 * @brief 系统开关机控制模块实现（软件定时器检测长按关机）
 *
 * 本文件实现系统开机/关机的硬件控制逻辑：
 *
 *   - SystemStart()    ：拉高 GPIO14 导通 PMOS，系统上电
 *   - SystemStop()     ：拉低 GPIO14 截断 PMOS，系统断电
 *   - Power_Button_Init()：配置 GPIO36 为输入模式
 *   - PowerBtn_Timer_Callback()：FreeRTOS 软件定时器回调，
 *     周期采样 GPIO36 电平，检测长按并触发关机
 */

#include "StartAndStop.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "gpio_driver.h"

/* === 日志标签 === */
#define TAG "StartAndStop"

/* ======================== 私有变量 =============================================== */

/** 软件定时器句柄 */
static TimerHandle_t StartAndStop_PowerBtnTimer = NULL;

/** 按键按下持续计数（每次定时器回调 +1） */
static uint32_t StartAndStop_BtnPressCount = 0;

/** 日志打印计数器（用于控制日志输出频率） */
static uint32_t StartAndStop_LogTickCount = 0;

/** 初始化稳定期计数器（前 2 秒忽略按键输入） */
static uint32_t StartAndStop_InitTickCount = 0;

/* ======================== 私有函数 =============================================== */

/**
 * @brief 系统关机（内部函数）
 *
 * 拉低 GPIO14 使 NPN 截止，PMOS 栅极被上拉电阻拉高，
 * PMOS 截止切断 7.4V 电源输出。
 *
 * @note 此函数由 PowerBtn_Timer_Callback() 在检测到长按后调用
 */
static void SystemStop(void)
{
	// ESP_LOGE(TAG, "SystemStop: pulling SYSTEMCONTROL_GPIO low -> power OFF");
	// Gpio_Init(SYSTEMCONTROL_GPIO, GPIO_OUTPUT);   // 初始化过了
	Gpio_Set_Level(SYSTEMCONTROL_GPIO, 0);
}

/**
 * @brief 系统开机
 *
 * 拉高 GPIO14 使 NPN 导通，PMOS 栅极被拉低，
 * PMOS 导通接通 7.4V 电源输出为系统供电。
 *
 * @note 应在 app_main() 中尽早调用
 */
void SystemStart(void)
{
	Gpio_Init(SYSTEMCONTROL_GPIO, GPIO_OUTPUT);
	Gpio_Set_Level(SYSTEMCONTROL_GPIO, 1);
}

/**
 * @brief 初始化开关机按键 GPIO
 *
 * 配置 GPIO36 为输入模式：
 *   - 无内部上下拉（GPIO34-39 为仅输入引脚，无内部上下拉电路）
 *   - 不使用中断（采用软件定时器轮询方式）
 *   - 外部硬件提供上拉电阻和二极管隔离
 *
 * @note 此函数仅供 StartAndStop_Init() 内部调用
 */
static void Power_Button_Init(void)
{
	Gpio_Init(POWER_BTN_GPIO, GPIO_INPUT);

	int init_level = Gpio_Get_Level(POWER_BTN_GPIO);
	// ESP_LOGE(TAG, "GPIO%d initial level = %d (0=low/pressed, 1=high/released)",
	// 	POWER_BTN_GPIO, init_level);
}

/**
 * @brief 软件定时器回调函数（周期检测按键状态）
 *
 * 每 BTN_SCAN_INTERVAL_MS（默认 20ms）被 FreeRTOS 定时器任务调用一次，
 * 执行以下逻辑：
 *
 *   1. 稳定期检查：上电后前 2 秒直接返回，不检测按键；
 *   2. 电平采样：读取 GPIO36 电平，低电平 = 按下；
 *   3. 长按计数：按下时累加计数器，松开时清零（防抖）；
 *   4. 触发关机：计数器达到阈值（LONG_PRESS_MS / BTN_SCAN_INTERVAL_MS）
 *      时停止定时器并调用 SystemStop()。
 *
 * @param xTimer  触发此回调的软件定时器句柄
 *
 * @note 此函数在 FreeRTOS 定时器服务任务上下文中执行，
 *       不应执行阻塞操作
 */
static void PowerBtn_Timer_Callback(TimerHandle_t xTimer)
{
	uint32_t press_threshold = LONG_PRESS_MS / BTN_SCAN_INTERVAL_MS;
	uint32_t init_stable_count = 2000 / BTN_SCAN_INTERVAL_MS;
	int level = Gpio_Get_Level(POWER_BTN_GPIO);

	StartAndStop_InitTickCount++;
	StartAndStop_LogTickCount++;

	if(StartAndStop_InitTickCount <= init_stable_count)
	{
		if(StartAndStop_LogTickCount % 25 == 1)
		{
			// ESP_LOGE(TAG, "init stable: %lu/%lu, level=%d (ignoring)",
			// 	StartAndStop_InitTickCount, init_stable_count, level);
		}
		return;
	}

	if(StartAndStop_LogTickCount % 25 == 1)
	{
		// ESP_LOGE(TAG, "timer: level=%d, pressCount=%lu/%lu", level,
		// 	StartAndStop_BtnPressCount, press_threshold);
	}

	if(level == 0)
	{
		StartAndStop_BtnPressCount++;
		if(StartAndStop_BtnPressCount >= press_threshold)
		{
			// ESP_LOGE(TAG, "long press detected! calling SystemStop()");
			StartAndStop_BtnPressCount = 0;
			xTimerStop(StartAndStop_PowerBtnTimer, 0);
			SystemStop();
		}
	}
	else
	{
		StartAndStop_BtnPressCount = 0;
	}
}

/**
 * @brief 初始化开关机模块（对外接口）
 *
 * 完成以下初始化工作：
 *   1. 调用 Power_Button_Init() 配置 GPIO36；
 *   2. 创建 FreeRTOS 软件定时器（周期 = BTN_SCAN_INTERVAL_MS）；
 *   3. 启动定时器开始轮询检测按键。
 *
 * @note 必须在 SystemStart() 之前调用，且仅需调用一次
 */
void StartAndStop_Init(void)
{
	// ESP_LOGE(TAG, "=== StartAndStop_Init: creating timer, interval=%dms, threshold=%dms ===",
	// 	BTN_SCAN_INTERVAL_MS, LONG_PRESS_MS);

	Power_Button_Init();

	StartAndStop_PowerBtnTimer = xTimerCreate(
		"PowerBtnTimer",
		pdMS_TO_TICKS(BTN_SCAN_INTERVAL_MS),
		pdTRUE,
		(void *)0,
		PowerBtn_Timer_Callback
	);

	if(StartAndStop_PowerBtnTimer != NULL)
	{
		xTimerStart(StartAndStop_PowerBtnTimer, 0);
		// ESP_LOGE(TAG, "=== StartAndStop_Init: timer started OK ===");
	}
	else
	{
		// ESP_LOGE(TAG, "!!! StartAndStop_Init: FAILED to create timer !!!");
	}
}