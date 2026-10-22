/**
 * @file ctl_rx.h
 * @brief 从节点控制通道接收（组播 239.0.0.1:5679）
 *
 * 主节点的 LCD / 蓝牙音量变化时，会在控制通道上发一帧 4 字节基础帧：
 *     { u16 magic=0xAA55; u8 cmd=0x05(VOLUME); u8 param=0~100; }
 * 本模块收到后直接调 Amplifier_Set_Volume()，让从节点音量跟随主节点。
 *
 * 帧格式与主节点 components/middlewares/sync_protocol/sync_protocol.h 的
 * sync_ctrl_basic_t 一致；只处理音量，其它命令（暂停/切歌等）暂时忽略。
 */
#ifndef CTL_RX_H
#define CTL_RX_H

#include "esp_err.h"
#include "wifi_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 控制组播地址/端口（与主节点 SYNC_PROTOCOL_MCAST_* 保持一致） */
#define CTL_RX_GROUP    "239.0.0.1"
#define CTL_RX_PORT     5679

/**
 * @brief 启动控制通道接收任务（幂等；重复调用返回 ESP_OK）
 *
 * @param wifi 已由 wifi_manager_create() 创建的句柄（未连接也可以，
 *             连接成功后 wifi_manager 会自动补建 socket 并重新加组）
 * @return ESP_OK / ESP_ERR_INVALID_ARG / ESP_ERR_NO_MEM 等
 */
esp_err_t ctl_rx_start(wifi_manager_handle_t wifi);

#ifdef __cplusplus
}
#endif

#endif /* CTL_RX_H */
