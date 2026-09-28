/**
 * @file sd_card.h
 * @brief SD 卡模块接口（私有 SPI 协议 + FATFS 挂载，句柄式文件 API）
 *
 * 主节点通过触摸屏模块自带的 Micro SD 卡槽读取 FAT32 文件系统：
 *   - /sdcard/music  ：本地音频文件（MP3/WAV）
 *   - /sdcard/ui     ：LVGL 界面资源（字体/图片）
 *
 * 架构说明：
 *   - 字节收发走 bsp/spi_driver 的从设备句柄（Spi_Handle_t）；
 *   - 模块内部自实现 SD 卡 SPI 模式协议（CMD0/8/ACMD41/16/17/24/9），
 *     不再依赖 ESP-IDF 的 sdspi/sdmmc 协议栈；
 *   - FATFS 通过 ff_diskio_register 绑定自定义 diskio，再经
 *     esp_vfs_fat_register + f_mount 挂载到 /sdcard；
 *   - CS 采用会话手动控制（csManualCtrl），保证命令+响应+数据
 *     整段序列 CS 恒定，且上电训练时钟时 CS 为高。
 *
 * 风格说明（句柄式）：
 *   - 文件以句柄（sd_card_file_handle_t）访问，句柄不透明；
 *   - Open/Read/Write/Seek/Tell/Close 一组 API，风格与 i2s_driver /
 *     audio_bus 的总线句柄保持一致，底层为 FATFS/VFS（线程安全）；
 *   - 句柄内部校验：非法句柄 / 未挂载时返回 ESP_ERR_INVALID_STATE 或
 *     ESP_ERR_INVALID_ARG，不会写坏内存。
 *
 * 硬件特性：
 *   - SD 卡与 LCD 分时复用 SPI2 总线（MOSI/SCK/MISO 共用，片选独立）；
 *   - 若 SPI2 总线已被 LCD 初始化，本模块直接复用，不重复初始化。
 */
#ifndef SD_CARD_H
#define SD_CARD_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

/* ======================== 挂载路径 / 引脚 / 常量 ================================= */

/** FATFS 挂载点（VFS 根路径），文件统一通过 /sdcard/xxx 访问 */
#define SD_CARD_MOUNT_POINT   "/sdcard"

/** SD 卡 SPI 片选引脚（主节点 PCB：GPIO15，严禁外部下拉） */
#define SD_CARD_CS_GPIO       15

/** SPI 时钟引脚（与 LCD 共用总线） */
#define SD_CARD_SCK_GPIO      19

/** SPI 数据输出引脚（与 LCD 共用总线） */
#define SD_CARD_MOSI_GPIO     18

/** SPI 数据输入引脚（与 LCD 共用总线） */
#define SD_CARD_MISO_GPIO     21

/** SPI 时钟频率（Hz）：SD 卡 SPI 模式上限 25MHz，10MHz 兼顾稳定性 */
#define SD_CARD_SPI_FREQ_HZ   (10 * 1000 * 1000)

/** SPI 上电/识别时钟（Hz）：SD 规范要求初始化命令 <=400kHz */
#define SD_CARD_INIT_FREQ_HZ   (400 * 1000)

/** 同时打开的文件句柄上限（音乐解码 2~3 + UI 资源若干） */
#define SD_CARD_MAX_OPEN_FILES 8

/* ======================== 句柄类型 =============================================== */

/** 文件句柄（不透明结构体） */
typedef struct sd_card_file_s *sd_card_file_handle_t;

/* ======================== API 函数 =============================================== */

/**
 * @brief 初始化 SD 卡并挂载 FATFS
 *
 * 流程：
 *   1. 若 SPI2 总线尚未初始化则按本模块引脚配置初始化；
 *      若已被 LCD 占用（ESP_ERR_INVALID_STATE）则直接复用现有总线；
 *   2. 以 SPI 模式挂载 FAT32 文件系统到 SD_CARD_MOUNT_POINT；
 *   3. 确保 /sdcard/music、/sdcard/ui 目录存在（不存在则创建）。
 *
 * @return ESP_OK              挂载成功
 *         ESP_FAIL             总线或挂载失败（卡未插 / 非 FAT32 等）
 */
esp_err_t SD_Card_Init(void);

/**
 * @brief 反初始化：关闭所有未关闭的句柄并卸载文件系统
 *
 * @return ESP_OK 成功
 */
esp_err_t SD_Card_Deinit(void);

/**
 * @brief 裸扇区自检（调试用）：读引导扇区，并对最后一个扇区做写入回环测试
 *
 * 注意：会在最后一个扇区临时写入测试图案，之后恢复原内容；
 *       结果是否通过以日志中的 RAW loopback 行判定。
 *
 * @return ESP_OK 自检流程完成（不代表回环通过，需看日志）
 */
esp_err_t SD_Card_Diag(void);

/**
 * @brief 文件系统诊断（调试用）：打印 FAT 几何信息、根目录中文件名对应的
 *        起始簇/FAT 表项，并直接物理读取其数据扇区，判定“FAT 逻辑映射”
 *        与“卡上实际数据”是否一致。
 */
void SD_Card_Diag_File(void);

/**
 * @brief 格式化整张卡并重新挂载（调试用，一次性重置）
 *
 * 警告：会删除卡上所有数据。格式化为 FAT（自动 FAT12/16/32），
 *       完成后重建 /sdcard/music、/sdcard/ui 目录。
 *
 * @return ESP_OK 格式化并重新挂载成功
 */
esp_err_t SD_Card_Format(void);

/**
 * @brief 一次性格式化开关（见实现文件注释）
 */
void SD_Card_Set_Format_Once(bool enable);

/**
 * @brief 查询 SD 卡是否已挂载
 *
 * @return true 已挂载，可安全读写 /sdcard
 */
bool SD_Card_Is_Mounted(void);

/**
 * @brief 获取挂载点字符串
 *
 * @return 挂载点（SD_CARD_MOUNT_POINT）
 */
const char *SD_Card_Get_Mount_Point(void);

/**
 * @brief 打开文件（句柄式）
 *
 * @param path 文件路径。必须位于本文件系统挂载点（SD_CARD_MOUNT_POINT）下，
 *             如 "/sdcard/music/a.mp3"；传其他文件系统路径（如 "/spiffs/x"）
 *             会被拒绝并返回 NULL。
 * @param mode 打开模式，同 fopen："r"/"w"/"a"/"rb"/"wb" 等
 *
 * @return 有效句柄；失败返回 NULL（可打印日志查看原因）
 */
sd_card_file_handle_t SD_Card_Open(const char *path, const char *mode);

/**
 * @brief 从文件句柄读取数据
 *
 * @param file       句柄
 * @param buffer     输出缓冲区
 * @param size       期望读取的字节数
 * @param bytes_read 输出参数：实际读取字节数（可为 NULL）
 *
 * @return ESP_OK 或 ESP_ERR_INVALID_ARG/INVALID_STATE
 */
esp_err_t SD_Card_Read(sd_card_file_handle_t file, void *buffer, size_t size, size_t *bytes_read);

/**
 * @brief 向文件句柄写入数据
 *
 * @param file          句柄
 * @param buffer        待写入数据
 * @param size          字节数
 * @param bytes_written 输出参数：实际写入字节数（可为 NULL）
 *
 * @return ESP_OK 或错误码
 */
esp_err_t SD_Card_Write(sd_card_file_handle_t file, const void *buffer, size_t size, size_t *bytes_written);

/**
 * @brief 定位文件读写位置（同 fseek）
 *
 * @param file   句柄
 * @param offset 偏移量
 * @param whence SEEK_SET / SEEK_CUR / SEEK_END
 *
 * @return ESP_OK 或错误码
 */
esp_err_t SD_Card_Seek(sd_card_file_handle_t file, long offset, int whence);

/**
 * @brief 获取当前读写位置（同 ftell）
 *
 * @param file     句柄
 * @param position 输出参数：当前位置（字节），从文件头起
 *
 * @return ESP_OK 或错误码
 */
esp_err_t SD_Card_Tell(sd_card_file_handle_t file, size_t *position);

/**
 * @brief 关闭文件句柄并释放资源
 *
 * @param file 句柄（关闭后置 NULL 由调用方负责）
 *
 * @return ESP_OK 或错误码
 */
esp_err_t SD_Card_Close(sd_card_file_handle_t file);

#endif