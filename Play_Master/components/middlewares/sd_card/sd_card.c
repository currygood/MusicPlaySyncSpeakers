/**
 * @file sd_card.c
 * @brief SD 卡模块实现（私有 SPI 协议 + FATFS diskio，句柄式文件 API）
 *
 * 设计说明：
 *   - 不再依赖 ESP-IDF 的 sdspi/sdmmc 协议栈；
 *   - 字节收发全部走 bsp/spi_driver 的从设备句柄（Spi_Handle_t）；
 *   - 自行实现 SD 卡 SPI 模式时序：CMD0/CMD8/ACMD41 上电初始化、
 *     CMD9 读 CSD 解析容量、CMD17/24 单块读写；
 *   - CS 使用会话手动控制（csManualCtrl + Spi_Device_Acquire/SetCS），
 *     保证“命令 + 响应 + 数据”整段序列 CS 恒定，且上电训练时钟时 CS 为高；
 *   - FATFS 通过 ff_diskio_register 注册自定义 diskio，再以
 *     esp_vfs_fat_register + f_mount 挂载到 /sdcard。
 *
 * 文件路径约定：
 *   - /sdcard/music ：本地音频文件（MP3/WAV）
 *   - /sdcard/ui    ：LVGL 界面资源
 */

#include "sd_card.h"

#include "spi_driver.h"
#include "esp_vfs_fat.h"
#include "diskio_impl.h"
#include "ff.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>

#define TAG "SD_CARD"

/* ======================== SD SPI 协议常量 ======================================== */

#define SD_CMD_0       0   /* CMD0  GO_IDLE_STATE      */
#define SD_CMD_8       8   /* CMD8  SEND_IF_COND       */
#define SD_CMD_9       9   /* CMD9  SEND_CSD（数据读） */
#define SD_CMD_16      16  /* CMD16 SET_BLOCKLEN       */
#define SD_CMD_17      17  /* CMD17 READ_SINGLE_BLOCK  */
#define SD_CMD_24      24  /* CMD24 WRITE_BLOCK        */
#define SD_CMD_55      55  /* CMD55 APP_CMD 前缀       */
#define SD_CMD_58      58  /* CMD58 READ_OCR           */
#define SD_CMD_41      41  /* ACMD41 SD_SEND_OP_COND   */

#define SD_R1_IN_IDLE       0x01   /* 卡处于空闲（idle）状态 */
#define SD_R1_ILLEGAL_CMD   0x04   /* 非法命令位 */
#define SD_DATA_TOKEN       0xFE   /* 数据块起始令牌（读/写） */
#define SD_WRITE_OK         0x05   /* 写数据响应：xxx00101 */
#define SD_WRITE_MASK       0x1F
#define SD_BUSY_TOTAL       2000000UL   /* 写 DBY 轮询上限（10MHz 下约 2s） */

/* ======================== 模块状态 =============================================== */

/**
 * SD 卡模块状态（单实例）
 */
typedef struct
{
    Spi_Handle_t handle;          /* bsp/spi_driver 从设备句柄 */
    uint32_t     sector_count;    /* 总扇区数（CSD 解析） */
    uint32_t     sector_size;     /* 扇区字节数（通常 512） */
    bool         high_capacity;   /* TRUE=块寻址（SDHC/SDXC），FALSE=字节寻址（SDSC） */
    BYTE         pdrv;            /* FATFS 物理驱动号 */
    FATFS       *fs;              /* esp_vfs_fat_register 分配的 FATFS 结构 */
    bool         mounted;         /* FATFS+VFS 是否已挂载 */
    bool         owns_bus;        /* SPI 总线是否由本模块创建 */
} sd_card_ctx_t;

static sd_card_ctx_t Sd;

/** 文件句柄实现：包一层标准 C 文件流（VFS/FATFS） */
struct sd_card_file_s
{
    FILE *fp;           /* FATFS 文件流 */
    bool  opened;       /* 槽位是否被占用 */
};

/** 打开文件槽位表（上限 SD_CARD_MAX_OPEN_FILES） */
static struct sd_card_file_s Sd_Files[SD_CARD_MAX_OPEN_FILES];

/* ======================== 底层 SPI 收发（总走 bsp 句柄） ====================== */

/** 发送指定字节 */
static esp_err_t Sd_Tx(const uint8_t *data, size_t length)
{
    return Spi_Transmit(Sd.handle, data, length);
}

/** 接收指定字节（SPI 同步总线：发送方补 0xFF） */
static esp_err_t Sd_Rx(uint8_t *data, size_t length)
{
    return Spi_Receive(Sd.handle, data, length);
}

/**
 * @brief 发送一条命令并等待 R1 响应
 *
 * CRC 说明：SPI 模式下默认 CRC 关闭，仅 CMD0（0x95）与 CMD8（0x87）
 * 的 CRC 被卡强制校验，其余命令填 0xFF 即可。
 */
static esp_err_t Sd_Cmd_R1(uint8_t cmd, uint32_t arg, uint8_t crc, uint8_t *r1)
{
    uint8_t frame[6];
    frame[0] = (uint8_t)(0x40 | cmd);
    frame[1] = (uint8_t)(arg >> 24);
    frame[2] = (uint8_t)(arg >> 16);
    frame[3] = (uint8_t)(arg >> 8);
    frame[4] = (uint8_t)arg;
    frame[5] = crc;

    esp_err_t err = Sd_Tx(frame, sizeof(frame));
    if (err != ESP_OK)
    {
        return err;
    }

    /* R1 在命令后 1~8 个时钟内出现（0xFF 表示还没到） */
    for (int i = 0; i < 16; i++)
    {
        uint8_t b;
        err = Sd_Rx(&b, 1);
        if (err != ESP_OK)
        {
            return err;
        }
        if ((b & 0x80) == 0)
        {
            *r1 = b;
            return ESP_OK;
        }
    }
    return ESP_ERR_TIMEOUT;
}

/** 等待数据起始令牌（读到非 0xFF 即返回） */
static esp_err_t Sd_Wait_Data_Token(uint8_t *token)
{
    for (int i = 0; i < 64; i++)
    {
        uint8_t b;
        esp_err_t err = Sd_Rx(&b, 1);
        if (err != ESP_OK)
        {
            return err;
        }
        if (b != 0xFF)
        {
            *token = b;
            return ESP_OK;
        }
    }
    return ESP_ERR_TIMEOUT;
}

/** 写块后等待卡“不再忙”（DO 恢复到高电平） */
static esp_err_t Sd_Wait_Not_Busy(void)
{
    for (uint32_t i = 0; i < SD_BUSY_TOTAL; i++)
    {
        uint8_t b;
        esp_err_t err = Sd_Rx(&b, 1);
        if (err != ESP_OK)
        {
            return err;
        }
        if (b == 0xFF)
        {
            return ESP_OK;
        }
        if ((i & 0x3FF) == 0)
        {
            vTaskDelay(pdMS_TO_TICKS(1));   /* 周期性让出 CPU */
        }
    }
    return ESP_ERR_TIMEOUT;
}

/* ======================== 会话（bus 占用 + CS 手动） =========================== */

/** 进入设备会话：独占总线，等待调用方手动拉低 CS */
static esp_err_t Sd_Session_Start(void)
{
    return Spi_Device_Acquire(Sd.handle);
}

/** 结束设备会话：先拉高 CS 再归还总线 */
static void Sd_Session_End(void)
{
    Spi_Device_SetCS(Sd.handle, true);
    Spi_Device_Release(Sd.handle);
}

/* ======================== CSD 解析 ============================================== */

/**
 * @brief 解析 CSD 寄存器（16 字节，与 ESP-IDF sdmmc 的位定义一致）
 *
 * CSD 字节序：byte0 = 位[127:120]（最高有效字节先到达）。
 *
 * @param csd         CSD 原始字节
 * @param out_sectors 输出：总扇区数（512B 扇区归一）
 * @return ESP_OK / ESP_ERR_NOT_SUPPORTED
 */
static esp_err_t Sd_Parse_Csd(const uint8_t *csd, uint32_t *out_sectors)
{
    uint8_t csd_ver = csd[0] >> 6;
    uint32_t blocks;            /* 以 2^READ_BL_LEN 字节为单位的总块数 */
    uint8_t read_bl_len;

    if (csd_ver == 0)           /* CSD v1.0（SDSC） */
    {
        read_bl_len = csd[5] & 0x0F;
        uint32_t c_size     = ((uint32_t)(csd[6] & 0x03) << 10) |
                              ((uint32_t)csd[7] << 2) |
                              ((csd[8] >> 6) & 0x03);
        uint32_t c_size_mult = ((uint32_t)(csd[9] & 0x03) << 1) |
                              ((csd[10] >> 7) & 0x01);
        blocks = (c_size + 1) << (c_size_mult + 2);
    }
    else if (csd_ver == 1)      /* CSD v2.0（SDHC/SDXC） */
    {
        uint32_t c_size = ((uint32_t)(csd[7] & 0x3F) << 16) |
                          ((uint32_t)csd[8] << 8) |
                          csd[9];
        blocks   = (c_size + 1) << 10;   /* 每块 512B，直接得扇区数 */
        read_bl_len = 9;
    }
    else
    {
        ESP_LOGE(TAG, "unknown CSD version %d", csd_ver);
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* 归一到 512B 扇区 */
    uint32_t blk_size = 1u << read_bl_len;
    uint32_t sector_size = (blk_size < 512) ? blk_size : 512;
    *out_sectors = blocks;
    if (sector_size < blk_size)
    {
        *out_sectors *= blk_size / sector_size;
    }
    return ESP_OK;
}

/* ======================== SD 上电初始化（私有 SPI 协议） ====================== */

static esp_err_t Sd_Card_Init_Hw(void)
{
    uint8_t  r1;
    uint8_t  dummy[10];

    Sd.sector_count = 0;        /* Probing 未完成前不可用 */

    esp_err_t err = Sd_Session_Start();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "acquire bus failed: %s", esp_err_to_name(err));
        return err;
    }

    /* ① CS 保持高电平，输出 ≥80 个空时钟（10 字节 0xFF）让卡完成上电同步 */
    memset(dummy, 0xFF, sizeof(dummy));
    err = Spi_Transmit(Sd.handle, dummy, sizeof(dummy));
    if (err != ESP_OK)
    {
        Sd_Session_End();
        return err;
    }

    /* ② 拉低 CS 进入命令模式 */
    Spi_Device_SetCS(Sd.handle, false);

    /* ③ CMD0 → 卡进入空闲态（R1=0x01） */
    bool idle = false;
    for (int i = 0; i < 10 && !idle; i++)
    {
        err = Sd_Cmd_R1(SD_CMD_0, 0, 0x95, &r1);
        if (err == ESP_OK && r1 == SD_R1_IN_IDLE)
        {
            idle = true;
        }
        else
        {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
    if (!idle)
    {
        ESP_LOGE(TAG, "CMD0 failed: err=%s r1=0x%02X", esp_err_to_name(err), r1);
        Sd_Session_End();
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "CMD0 OK, card in idle state");

    /* ④ CMD8 探测 SD v2.0+ */
    err = Sd_Cmd_R1(SD_CMD_8, 0x000001AA, 0x87, &r1);
    if (err == ESP_OK && r1 == SD_R1_IN_IDLE)
    {
        uint8_t echo[4] = {0};
        err = Sd_Rx(echo, sizeof(echo));
        if (err != ESP_OK || echo[2] != 0x01 || echo[3] != 0xAA)
        {
            ESP_LOGE(TAG, "CMD8 echo mismatch: %02X %02X %02X %02X",
                     echo[0], echo[1], echo[2], echo[3]);
            Sd_Session_End();
            return ESP_ERR_INVALID_RESPONSE;
        }
        ESP_LOGI(TAG, "SD v2.0+ card detected");
    }
    else if (err == ESP_OK && (r1 & SD_R1_ILLEGAL_CMD))
    {
        ESP_LOGW(TAG, "SD v1.x card (不支持 CMD8)");
    }
    else
    {
        ESP_LOGE(TAG, "CMD8 failed: err=%s r1=0x%02X", esp_err_to_name(err), r1);
        Sd_Session_End();
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* ⑤ ACMD41：置 HCS 位并轮询卡初始化完成（超时约 2s） */
    bool ready = false;
    for (int i = 0; i < 500 && !ready; i++)
    {
        err = Sd_Cmd_R1(SD_CMD_55, 0, 0xFF, &r1);
        if (err != ESP_OK)
        {
            break;
        }
        if ((r1 & 0xFE) != 0)
        {
            break;          /* R1 出现错误位 → 非 SD 卡或异常 */
        }
        err = Sd_Cmd_R1(SD_CMD_41, 0x40000000, 0xFF, &r1);
        if (err == ESP_OK && r1 == 0x00)
        {
            ready = true;   /* 卡初始化完成 */
        }
        else if (err == ESP_OK && (r1 & 0xFE) != 0)
        {
            break;          /* 非法命令等 → 不支持 */
        }
        else
        {
            vTaskDelay(pdMS_TO_TICKS(4));
        }
    }
    if (!ready)
    {
        ESP_LOGE(TAG, "ACMD41 timeout: err=%s r1=0x%02X", esp_err_to_name(err), r1);
        Sd_Session_End();
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "ACMD41 ready");

    /* ⑥ CMD58 读 OCR：bit30=HCS → 高位容量判别块寻址 */
    err = Sd_Cmd_R1(SD_CMD_58, 0, 0xFF, &r1);
    if (err != ESP_OK || r1 != 0x00)
    {
        ESP_LOGE(TAG, "CMD58 failed: err=%s r1=0x%02X", esp_err_to_name(err), r1);
        Sd_Session_End();
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint8_t ocr[4] = {0};
    err = Sd_Rx(ocr, sizeof(ocr));
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "CMD58 OCR read failed: %s", esp_err_to_name(err));
        Sd_Session_End();
        return err;
    }
    Sd.high_capacity = (ocr[0] & 0x40) != 0;
    ESP_LOGI(TAG, "OCR: %02X %02X %02X %02X, high-capacity=%d", ocr[0], ocr[1], ocr[2], ocr[3], Sd.high_capacity);

    /* ⑦ CMD16 设置块长 512B */
    err = Sd_Cmd_R1(SD_CMD_16, 512, 0xFF, &r1);
    if (err != ESP_OK || r1 != 0x00)
    {
        ESP_LOGE(TAG, "CMD16 failed: err=%s r1=0x%02X", esp_err_to_name(err), r1);
        Sd_Session_End();
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* ⑧ CMD9 读 CSD 并解析容量 */
    err = Sd_Cmd_R1(SD_CMD_9, 0, 0xFF, &r1);
    if (err != ESP_OK || r1 != 0x00)
    {
        ESP_LOGE(TAG, "CMD9 failed: err=%s r1=0x%02X", esp_err_to_name(err), r1);
        Sd_Session_End();
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint8_t tok = 0xFF;
    err = Sd_Wait_Data_Token(&tok);
    if (err != ESP_OK || tok != SD_DATA_TOKEN)
    {
        ESP_LOGE(TAG, "CSD data token error: err=%s tok=0x%02X", esp_err_to_name(err), tok);
        Sd_Session_End();
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint8_t csd[18] = {0};        /* 16B CSD + 2B CRC */
    err = Sd_Rx(csd, sizeof(csd));
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "CSD read failed: %s", esp_err_to_name(err));
        Sd_Session_End();
        return err;
    }
    err = Sd_Parse_Csd(csd, &Sd.sector_count);
    if (err != ESP_OK)
    {
        Sd_Session_End();
        return err;
    }
    Sd.sector_size = 512;
    ESP_LOGI(TAG, "SD capacity: %lu MB (%lu sectors)",
             (unsigned long)(Sd.sector_count / 2048UL), (unsigned long)Sd.sector_count);

    Sd_Session_End();
    return ESP_OK;
}

/* ======================== 扇区读写 ============================================= */

/** 计算命令参数地址：SDSC=字节寻址，SDHC/SDXC=块寻址 */
static uint32_t Sd_Block_Addr(uint32_t sector)
{
    return Sd.high_capacity ? sector : sector * Sd.sector_size;
}

/**
 * @brief 读取单个扇区（CMD17）
 */
static esp_err_t Sd_Read_Sector(uint32_t sector, uint8_t *buffer)
{
    uint8_t r1 = 0xFF;
    esp_err_t err = Sd_Session_Start();
    if (err != ESP_OK)
    {
        return err;
    }

    Spi_Device_SetCS(Sd.handle, false);
    err = Sd_Cmd_R1(SD_CMD_17, Sd_Block_Addr(sector), 0xFF, &r1);
    if (err == ESP_OK && r1 == 0x00)
    {
        uint8_t token = 0xFF;
        err = Sd_Wait_Data_Token(&token);
        if (err == ESP_OK && token == SD_DATA_TOKEN)
        {
            uint8_t crc[2];
            err = Sd_Rx(buffer, Sd.sector_size);
            if (err == ESP_OK)
            {
                Sd_Rx(crc, sizeof(crc));    /* CRC 关闭状态，丢弃 */
            }
        }
        else
        {
            ESP_LOGE(TAG, "sector %lu read token error: 0x%02X",
                     (unsigned long)sector, token);
            err = ESP_ERR_INVALID_RESPONSE;
        }
    }
    else
    {
        ESP_LOGE(TAG, "CMD17(0x%08lX) failed: r1=0x%02X err=%s",
                 (unsigned long)Sd_Block_Addr(sector), r1, esp_err_to_name(err));
        err = (err == ESP_OK) ? ESP_ERR_INVALID_RESPONSE : err;
    }

    Sd_Session_End();
    return err;
}

/**
 * @brief 写入单个扇区（CMD24）
 */
static esp_err_t Sd_Write_Sector(uint32_t sector, const uint8_t *buffer)
{
    uint8_t r1 = 0xFF;
    esp_err_t err = Sd_Session_Start();
    if (err != ESP_OK)
    {
        return err;
    }

    Spi_Device_SetCS(Sd.handle, false);
    err = Sd_Cmd_R1(SD_CMD_24, Sd_Block_Addr(sector), 0xFF, &r1);
    if (err == ESP_OK && r1 == 0x00)
    {
        uint8_t token = SD_DATA_TOKEN;
        uint8_t crc[2] = {0xFF, 0xFF};
        err = Sd_Tx(&token, 1);
        if (err == ESP_OK)
        {
            err = Sd_Tx(buffer, Sd.sector_size);
        }
        if (err == ESP_OK)
        {
            err = Sd_Tx(crc, sizeof(crc));
        }
        if (err == ESP_OK)
        {
            uint8_t resp;
            err = Sd_Rx(&resp, 1);
            if ((resp & SD_WRITE_MASK) == SD_WRITE_OK)
            {
                err = Sd_Wait_Not_Busy();
            }
            else
            {
                ESP_LOGE(TAG, "write response: 0x%02X", resp);
                err = ESP_ERR_INVALID_RESPONSE;
            }
        }
    }
    else
    {
        ESP_LOGE(TAG, "CMD24(0x%lx) failed: r1=0x%02X err=%s",
                 (unsigned long)Sd_Block_Addr(sector), r1, esp_err_to_name(err));
        err = (err == ESP_OK) ? ESP_ERR_INVALID_RESPONSE : err;
    }

    Sd_Session_End();
    return err;
}

/* ======================== FATFS diskio 回调 ==================================== */

/** 磁盘初始化回调：卡已探测成功即可用 */
static DSTATUS Sd_Disk_Init(BYTE pdrv)
{
    return (Sd.sector_count > 0) ? 0 : STA_NOINIT;
}

/** 磁盘状态回调 */
static DSTATUS Sd_Disk_Status(BYTE pdrv)
{
    return (Sd.sector_count > 0) ? 0 : STA_NOINIT;
}

/** 扇区读取回调 */
static DRESULT Sd_Disk_Read(BYTE pdrv, BYTE *buff, DWORD sector, UINT count)
{
    if (Sd.sector_count == 0)
    {
        return RES_NOTRDY;
    }
    for (UINT i = 0; i < count; i++)
    {
        if (Sd_Read_Sector(sector + i, buff + (size_t)i * Sd.sector_size) != ESP_OK)
        {
            return RES_ERROR;
        }
    }
    return RES_OK;
}

/** 扇区写入回调 */
static DRESULT Sd_Disk_Write(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count)
{
    if (Sd.sector_count == 0)
    {
        return RES_NOTRDY;
    }
    for (UINT i = 0; i < count; i++)
    {
        if (Sd_Write_Sector(sector + i, buff + (size_t)i * Sd.sector_size) != ESP_OK)
        {
            return RES_ERROR;
        }
    }
    return RES_OK;
}

/** 控制命令回调（FATFS 用磁盘参数查询） */
static DRESULT Sd_Disk_Ioctl(BYTE pdrv, BYTE ctrl, void *buff)
{
    switch (ctrl)
    {
        case CTRL_SYNC:
            return RES_OK;                    /* 写块时已等待 DO 空闲 */
        case GET_SECTOR_COUNT:
            *(DWORD *)buff = Sd.sector_count;
            return RES_OK;
        case GET_SECTOR_SIZE:
            *(WORD *)buff = (WORD)Sd.sector_size;
            return RES_OK;
        case GET_BLOCK_SIZE:
            return RES_ERROR;                 /* 无擦除粒度，FATFS 仅 mkfs 用 */
        default:
            return RES_PARERR;
    }
}

/**
 * @brief 注册自定义 diskio 并挂载 FATFS 到 /sdcard
 */
static esp_err_t Sd_Fat_Mount(void)
{
    BYTE pdrv;
    esp_err_t err = ff_diskio_get_drive(&pdrv);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "no free FATFS drive slot: %s", esp_err_to_name(err));
        return err;
    }

    /* 注册物理驱动（FATFS 通过它调用 fs_driver_impl） */
    static const ff_diskio_impl_t diskio_impl = {
        .init   = Sd_Disk_Init,
        .status = Sd_Disk_Status,
        .read   = Sd_Disk_Read,
        .write  = Sd_Disk_Write,
        .ioctl  = Sd_Disk_Ioctl,
    };
    ff_diskio_register(pdrv, &diskio_impl);

    /* VFS 绑定：/sdcard → "0:" 驱动 */
    char drv[3] = {(char)('0' + pdrv), ':', '\0'};
    esp_vfs_fat_conf_t conf = {
        .base_path = SD_CARD_MOUNT_POINT,
        .fat_drive = drv,
        .max_files = SD_CARD_MAX_OPEN_FILES,
    };
    FATFS *fs_obj = NULL;
    err = esp_vfs_fat_register(&conf, &fs_obj);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_vfs_fat_register failed: %s", esp_err_to_name(err));
        ff_diskio_unregister(pdrv);
        return err;
    }

    FRESULT fres = f_mount(fs_obj, drv, 1);
    if (fres != FR_OK)
    {
        ESP_LOGE(TAG, "f_mount failed: %d (请确认卡为 FAT32)", (int)fres);
        f_mount(NULL, drv, 0);
        esp_vfs_fat_unregister_path(SD_CARD_MOUNT_POINT);
        ff_diskio_unregister(pdrv);
        return ESP_FAIL;
    }

    Sd.pdrv    = pdrv;
    Sd.fs      = fs_obj;
    Sd.mounted = true;
    ESP_LOGI(TAG, "FATFS drive %d mounted, path=%s", pdrv, SD_CARD_MOUNT_POINT);
    return ESP_OK;
}

/* ======================== 内部工具（总线/目录） =============================== */

/**
 * @brief 初始化 SPI2 总线（若尚未被 LCD 占用）
 */
static esp_err_t Sd_Bus_Init(void)
{
    Spi_Config_t cfg = {
        .clockSpeedHz = SD_CARD_SPI_FREQ_HZ,
        .sckPin       = SD_CARD_SCK_GPIO,
        .mosiPin      = SD_CARD_MOSI_GPIO,
        .misoPin      = SD_CARD_MISO_GPIO,
    };

    esp_err_t ret = Spi_Init(&cfg);
    if (ret == ESP_ERR_INVALID_STATE)
    {
        ESP_LOGW(TAG, "SPI bus already initialized, reuse it");
        return ret;   /* 调用方据此保持 owns_bus=false */
    }
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    Sd.owns_bus = true;
    ESP_LOGI(TAG, "SPI2 bus created by SD card: SCK=%d MOSI=%d MISO=%d",
             SD_CARD_SCK_GPIO, SD_CARD_MOSI_GPIO, SD_CARD_MISO_GPIO);
    return ESP_OK;
}

/** 确保挂载点下业务目录存在（/music、/ui） */
static void Sd_Card_Ensure_Dirs(void)
{
    const char *dirs[] = {
        SD_CARD_MOUNT_POINT "/music",
        SD_CARD_MOUNT_POINT "/ui",
    };

    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
    {
        DIR *dir = opendir(dirs[i]);
        if (dir != NULL)
        {
            closedir(dir);   /* 已存在 */
            continue;
        }
        if (mkdir(dirs[i], 0777) == 0)
        {
            ESP_LOGI(TAG, "create dir: %s", dirs[i]);
        }
        else
        {
            ESP_LOGW(TAG, "create dir failed: %s", dirs[i]);
        }
    }
}


/**
 * @brief 校验路径是否属于本文件系统（FATFS 挂载点下）
 *
 * 挂载点前缀匹配必须落在完整路径段上：
 *   - "/sdcard"        → 合法（挂载点本身）
 *   - "/sdcard/music"  → 合法
 *   - "/sdcardXxx..."  → 非法（避免前缀串到别的路径段）
 *   - "/spiffs/xxx"    → 非法（其他文件系统的路径，直接拒绝）
 *
 * @param path 待校验路径
 * @return true 属于本文件系统
 */
static bool Sd_Is_Fat_Path(const char *path)
{
    const char *mount = SD_CARD_MOUNT_POINT;
    size_t len = strlen(mount);

    if (path == NULL || strncmp(path, mount, len) != 0)
    {
        return false;
    }
    /* 下一个字符必须是结尾或 '/'; 避免 /sdcardXxx 这种前缀误匹配 */
    return (path[len] == '\0' || path[len] == '/');
}

/** 关闭所有由用户打开未关闭的文件句柄（Deinit 使用） */
static void Sd_Card_Force_Close_All(void)
{
    for (int i = 0; i < SD_CARD_MAX_OPEN_FILES; i++)
    {
        if (Sd_Files[i].fp != NULL)
        {
            ESP_LOGW(TAG, "force close unclosed file handle slot %d", i);
            fclose(Sd_Files[i].fp);
            Sd_Files[i].fp = NULL;
        }
    }
}

/* ======================== 公共 API ============================================= */

esp_err_t SD_Card_Init(void)
{
    if (Sd.mounted)
    {
        ESP_LOGW(TAG, "already initialized");
        return ESP_OK;
    }

    /* 1) 准备 SPI 总线（若 LCD 已初始化则复用） */
    if (Sd.handle == NULL)
    {
        esp_err_t ret = Sd_Bus_Init();
        if (ret == ESP_ERR_INVALID_STATE)
        {
            Sd.owns_bus = false;   /* 总线归 LCD 所有 */
        }
        else if (ret != ESP_OK)
        {
            return ret;
        }
    }

    /* 2) 注册 SD 卡从设备（句柄内绑定 CS=15，手动片选模式） */
    if (Sd.handle == NULL)
    {
        Spi_DeviceConfig_t dev_cfg = {
            .csPin        = SD_CARD_CS_GPIO,
            .clockSpeedHz = SD_CARD_SPI_FREQ_HZ,
            .mode         = 0,          /* SPI 模式 0（CPOL=0, CPHA=0） */
            .queueSize    = 1,
            .csManualCtrl = true,       /* SD 协议需要会话级 CS 控制 */
        };
        Sd.handle = Spi_Register_Device(&dev_cfg);
        if (Sd.handle == NULL)
        {
            ESP_LOGE(TAG, "SPI device register failed (CS=%d)", SD_CARD_CS_GPIO);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "SD device registered: CS=%d, %dMHz", SD_CARD_CS_GPIO,
                 SD_CARD_SPI_FREQ_HZ / 1000000);
    }

    /* 3) SD 卡上电初始化（私有 SPI 协议：CMD0/8/ACMD41/16/9） */
    esp_err_t ret = Sd_Card_Init_Hw();
    if (ret != ESP_OK)
    {
        /* 失败不释放总线：句柄保留，可再次调用重试 */
        return ret;
    }

    /* 4) 注册自定义 diskio 并挂载 FATFS */
    ret = Sd_Fat_Mount();
    if (ret != ESP_OK)
    {
        return ret;
    }

    /* 5) 确保业务目录存在，清空文件句柄槽位 */
    Sd_Card_Ensure_Dirs();
    memset(Sd_Files, 0, sizeof(Sd_Files));

    ESP_LOGI(TAG, "SD card mounted at \"%s\"", SD_CARD_MOUNT_POINT);
    return ESP_OK;
}

esp_err_t SD_Card_Deinit(void)
{
    if (!Sd.mounted)
    {
        ESP_LOGW(TAG, "not initialized");
        return ESP_OK;
    }

    Sd_Card_Force_Close_All();

    char drv[3] = {(char)('0' + Sd.pdrv), ':', '\0'};
    f_mount(NULL, drv, 0);
    esp_vfs_fat_unregister_path(SD_CARD_MOUNT_POINT);
    ff_diskio_unregister(Sd.pdrv);

    if (Sd.owns_bus)
    {
        Spi_Deinit();
        Sd.owns_bus = false;
        Sd.handle   = NULL;   /* 总线释放后池内句柄失效 */
    }

    Sd.mounted = false;
    Sd.fs      = NULL;

    ESP_LOGI(TAG, "SD card unmounted");
    return ESP_OK;
}

bool SD_Card_Is_Mounted(void)
{
    return Sd.mounted;
}

const char *SD_Card_Get_Mount_Point(void)
{
    return SD_CARD_MOUNT_POINT;
}

/* ======================== 句柄式文件 API ======================================= */

sd_card_file_handle_t SD_Card_Open(const char *path, const char *mode)
{
    if (!Sd.mounted)
    {
        ESP_LOGE(TAG, "SD card not mounted, call SD_Card_Init first");
        return NULL;
    }
    if (path == NULL || mode == NULL)
    {
        ESP_LOGE(TAG, "invalid argument (path/mode NULL)");
        return NULL;
    }
    /* 校验路径必须位于本文件系统（FATFS）挂载点下，防止误用到其他文件系统 */
    if (!Sd_Is_Fat_Path(path))
    {
        ESP_LOGE(TAG, "path is not under FATFS mount point \"%s\": %s",
                 SD_CARD_MOUNT_POINT, path);
        return NULL;
    }

    /* 寻找空闲槽位 */
    struct sd_card_file_s *slot = NULL;
    for (int i = 0; i < SD_CARD_MAX_OPEN_FILES; i++)
    {
        if (Sd_Files[i].fp == NULL)
        {
            slot = &Sd_Files[i];
            break;
        }
    }
    if (slot == NULL)
    {
        ESP_LOGE(TAG, "too many open files (max %d)", SD_CARD_MAX_OPEN_FILES);
        return NULL;
    }

    FILE *fp = fopen(path, mode);
    if (fp == NULL)
    {
        ESP_LOGE(TAG, "fopen failed: %s (mode=%s)", path, mode);
        return NULL;
    }

    slot->fp     = fp;
    slot->opened = true;
    return (sd_card_file_handle_t)slot;
}

esp_err_t SD_Card_Read(sd_card_file_handle_t file, void *buffer, size_t size, size_t *bytes_read)
{
    if (file == NULL || buffer == NULL || size == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (file->fp == NULL || !Sd.mounted)
    {
        return ESP_ERR_INVALID_STATE;
    }

    size_t got = fread(buffer, 1, size, file->fp);
    if (bytes_read != NULL)
    {
        *bytes_read = got;
    }
    return ferror(file->fp) ? ESP_FAIL : ESP_OK;
}

esp_err_t SD_Card_Write(sd_card_file_handle_t file, const void *buffer, size_t size, size_t *bytes_written)
{
    if (file == NULL || buffer == NULL || size == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (file->fp == NULL || !Sd.mounted)
    {
        return ESP_ERR_INVALID_STATE;
    }

    size_t wrote = fwrite(buffer, 1, size, file->fp);
    if (bytes_written != NULL)
    {
        *bytes_written = wrote;
    }
    return (wrote == size) ? ESP_OK : ESP_FAIL;
}

esp_err_t SD_Card_Seek(sd_card_file_handle_t file, long offset, int whence)
{
    if (file == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (file->fp == NULL || !Sd.mounted)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return (fseek(file->fp, offset, whence) == 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t SD_Card_Tell(sd_card_file_handle_t file, size_t *position)
{
    if (file == NULL || position == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (file->fp == NULL || !Sd.mounted)
    {
        return ESP_ERR_INVALID_STATE;
    }

    long pos = ftell(file->fp);
    if (pos < 0)
    {
        return ESP_FAIL;
    }
    *position = (size_t)pos;
    return ESP_OK;
}

esp_err_t SD_Card_Close(sd_card_file_handle_t file)
{
    if (file == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (file->fp == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret  = (fclose(file->fp) == 0) ? ESP_OK : ESP_FAIL;
    file->fp       = NULL;
    file->opened   = false;
    return ret;
}
