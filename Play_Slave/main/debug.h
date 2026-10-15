/**
 * @file debug.h
 * @brief 调试 / 自检辅助函数（原先堆在 app_main.c 里，现独立成文件）
 *
 * 这里放的都是**不参与业务链路**的函数：哪一环出问题，就在硬件上单独验哪一环。
 * app_main.c 里保留了调用点（默认注释掉），需要时放开一行即可。
 *
 *   - Debug_SdRawTest()：SD 卡裸扇区回环测试，绕过文件系统直接验卡本身好坏
 *   - Debug_BeepTest() ：5 秒"嘟嘟"提示音，脱网自检功放 + I2S 通路
 *
 * ⚠️ 两个函数都会**独占硬件资源**，别在同步链路跑起来之后调用：
 *   - Debug_SdRawTest 独占 SPI2 总线，并占用 CS/SCK/MOSI/MISO 四根 GPIO；
 *   - Debug_BeepTest  自己 Init/Deinit 功放，会打断正在播放的音频。
 */
#ifndef DEBUG_H
#define DEBUG_H

#include "esp_err.h"

/* ======================== 自检入口 =============================================== */

/**
 * @brief SD 卡裸扇区回环测试（不经过文件系统）
 *
 * 用官方 sdspi + sdmmc 协议栈直接对若干关键扇区做
 * 「读原值 → 写 0x00~0xFF 模式 → 读回比对 → 恢复原值」的回环。
 * 如果"写进去读不回来"，说明卡上对应区域损坏（介质问题），与驱动无关。
 *
 * 接线（SPI 模式，卡座/面包板）：
 *   SD 模块   ->  ESP32-S3        （引脚冲突时改 debug.c 顶部的 SD_TEST_* 宏）
 *   VCC(3.3V) ->  3V3
 *   GND       ->  GND
 *   CS(DAT3)  ->  GPIO14
 *   SCK(CLK)  ->  GPIO12
 *   MOSI(CMD) ->  GPIO11
 *   MISO(DAT0)->  GPIO13
 *
 * @warning 探测点包含扇区 0 和 1，那是 MBR / 分区表所在位置 —— 测试会写入模式
 *          再恢复原值，中途掉电或恢复失败就是分区表损坏。跑之前先看
 *          debug.c 里 sd_run_probe() 的 probes[]，把不想动的扇区去掉。
 *
 * @return ESP_OK 完成（不代表全部扇区 PASS，逐扇区结果看串口）；
 *         其它为初始化/读卡阶段的错误
 */
esp_err_t Debug_SdRawTest(void);

/**
 * @brief 播放 5 秒"嘟嘟"提示音（800Hz 方波，10 声）
 *
 * 内部自行 Amplifier_Init() / Amplifier_Deinit()，因此可以独立调用。
 * 注意它用的是全链路固定的 44.1kHz / 单声道，与同步播放走同一套配置。
 */
void Debug_BeepTest(void);

#endif /* DEBUG_H */
