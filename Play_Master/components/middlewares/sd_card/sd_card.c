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
#include "driver/gpio.h"
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

/** 一次性格式化开关：SD_Card_Set_Format_Once(true) 后，本次初始化中
 *  无论挂载失败还是成功，都先 f_mkfs 重建干净 FAT 卷（调试用，重启自动失效） */
static bool s_format_once = false;

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
    else if (csd_ver == 1 || csd_ver == 2)  /* CSD v2.0/v3.0（SDHC/SDXC，布局相同） */
    {
        uint32_t c_size = ((uint32_t)(csd[7] & 0x3F) << 16) |
                          ((uint32_t)csd[8] << 8) |
                          csd[9];
        blocks   = (c_size + 1) << 10;   /* 2^9=512B 块，直接得扇区数 */
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
    uint8_t  r1 = 0xFF;
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
    vTaskDelay(pdMS_TO_TICKS(1));   /* 上电稳定：首个命令前 >=1ms */

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
        ESP_LOGE(TAG, "CMD0 failed: err=%s r1=0x%02X (检查卡座接触/供电/连线)",
                 esp_err_to_name(err), r1);
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

    /* ⑧ CMD16 设置块长 512B（400kHz 下配置） */
    err = Sd_Cmd_R1(SD_CMD_16, 512, 0xFF, &r1);
    if (err != ESP_OK || r1 != 0x00)
    {
        ESP_LOGE(TAG, "CMD16 failed: err=%s r1=0x%02X", esp_err_to_name(err), r1);
        Sd_Session_End();
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* ⑨ CMD9 读 CSD 并解析容量（仍在 400kHz 初始化时钟下读，规范要求，时序余量最大） */
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

    /* 识别 + CSD 已在 400kHz 完成：释放会话后提升时钟 */
    Sd_Session_End();

    err = Spi_Device_Set_Clock(Sd.handle, SD_CARD_SPI_FREQ_HZ);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "raise clock failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "card ready, SPI clock raised to 10MHz");

    /* 重新开会话并立即结束：后续读写/诊断自行管理会话 */
    err = Sd_Session_Start();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "acquire bus failed: %s", esp_err_to_name(err));
        return err;
    }
    Sd_Session_End();

    SD_Card_Diag();   /* 调试团：释放会话后可自由做多点位回环 */
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

/* ======================== 调试自检（裸扇区） =================================== */

static void sd_hex_log(const char *label, uint32_t sector, const uint8_t *data, size_t len)
{
    char line[64];
    size_t n = 0;
    n += snprintf(line + n, sizeof(line) - n, "[%05lu]", (unsigned long)sector);
    size_t show = (len > 16) ? 16 : len;
    for (size_t i = 0; i < show; i++)
    {
        n += snprintf(line + n, sizeof(line) - n, " %02X", data[i]);
    }
    ESP_LOGI(TAG, "%s%s", label, line);
}

/* 定义在本段下方 */
static void sd_loopback(uint32_t sector);

esp_err_t SD_Card_Diag(void)
{
    if (Sd.sector_count == 0)
    {
        ESP_LOGE(TAG, "diag: card not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "diag: 多点位扇区回环开始 (total=%lu)",
             (unsigned long)Sd.sector_count);
    const uint32_t probes[] = {0, 1, 63, 64, 1000, 40960,
                               Sd.sector_count / 2, Sd.sector_count - 1};
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++)
    {
        sd_loopback(probes[i]);
    }
    ESP_LOGI(TAG, "diag: 回环结束");
    return ESP_OK;
}

/** 对单个扇区做“读-写-读回-恢复-校验”回环 */
static void sd_loopback(uint32_t sector)
{
    uint8_t orig[512];
    uint8_t chk[512];

    if (Sd_Read_Sector(sector, orig) != ESP_OK)
    {
        ESP_LOGW(TAG, "LB[%lu]: read-before FAIL", (unsigned long)sector);
        return;
    }
    sd_hex_log("LB before:", sector, orig, 8);

    uint8_t pat[512];
    for (int i = 0; i < 512; i++)
    {
        pat[i] = (uint8_t)i;
    }
    if (Sd_Write_Sector(sector, pat) != ESP_OK)
    {
        ESP_LOGE(TAG, "LB[%lu]: write FAIL", (unsigned long)sector);
        return;
    }
    if (Sd_Read_Sector(sector, chk) != ESP_OK)
    {
        ESP_LOGE(TAG, "LB[%lu]: re-read FAIL", (unsigned long)sector);
        return;
    }
    sd_hex_log("LB read :", sector, chk, 8);
    int diff = 0, first = -1;
    for (int i = 0; i < 512; i++)
    {
        if (chk[i] != pat[i])
        {
            diff++;
            if (first < 0)
            {
                first = i;
            }
        }
    }
    if (diff == 0)
    {
        ESP_LOGI(TAG, "LB[%lu]: PASS (512B identical)", (unsigned long)sector);
    }
    else
    {
        ESP_LOGE(TAG, "LB[%lu]: FAIL diff=%d first@%d",
                 (unsigned long)sector, diff, first);
    }

    /* 恢复原内容并校验 */
    if (Sd_Write_Sector(sector, orig) != ESP_OK)
    {
        ESP_LOGW(TAG, "LB[%lu]: restore-write FAIL", (unsigned long)sector);
        return;
    }
    if (Sd_Read_Sector(sector, chk) != ESP_OK)
    {
        ESP_LOGW(TAG, "LB[%lu]: restore-read FAIL", (unsigned long)sector);
        return;
    }
    ESP_LOGI(TAG, "LB[%lu]: restore %s",
             (unsigned long)sector,
             (memcmp(chk, orig, 512) == 0) ? "OK" : "FAIL");
}

/* ======================== 文件系统诊断（FAT 层）=============================== */

/** 只读一个物理扇区的包装 */
static esp_err_t sd_diag_raw_read(uint32_t sector, uint8_t *buffer)
{
    return Sd_Read_Sector(sector, buffer);
}

void SD_Card_Diag_File(void)
{
    if (Sd.fs == NULL || !Sd.mounted)
    {
        ESP_LOGE(TAG, "diagf: FATFS not mounted");
        return;
    }
    FATFS *fs = Sd.fs;
    ESP_LOGI(TAG, "diagf: FATFS buffer addr=%08X (%s)",
             (unsigned)(uintptr_t)fs,
             ((uintptr_t)fs >= 0x3F800000U && (uintptr_t)fs < 0x3FC00000U)
                 ? "PSRAM(SPIRAM)!" : "internal DRAM");
    ESP_LOGI(TAG, "diagf: fs_type=%u csize=%u n_fats=%u ssize=%u n_rootdir=%u",
             fs->fs_type, fs->csize, fs->n_fats, (uint32_t)fs->ssize, fs->n_rootdir);
    ESP_LOGI(TAG, "diagf: volbase=%lu fatbase=%lu dirbase=%lu database=%lu n_fatent=%lu fsize=%lu",
             (unsigned long)fs->volbase, (unsigned long)fs->fatbase,
             (unsigned long)fs->dirbase, (unsigned long)fs->database,
             (unsigned long)fs->n_fatent, (unsigned long)fs->fsize);

    /* 根目录物理扇区：FAT12/16 在 dirbase；FAT32 在 dirbase 簇 */
    uint32_t root_sector = 0;
    if (fs->fs_type == 3 || fs->fs_type == 4)
    {
        root_sector = (uint32_t)(fs->database + ((uint64_t)(fs->dirbase - 2) * fs->csize));
    }
    else
    {
        root_sector = (uint32_t)fs->dirbase;
    }
    ESP_LOGI(TAG, "diagf: root dir sector=%lu (read max 16 sectors)",
             (unsigned long)root_sector);

    /* 在根目录里找 "TEST    TXT"（SFN 8.3） */
    const char want[11] = {'T','E','S','T',' ',' ',' ',' ','T','X','T'};
    uint8_t sec[512];
    int      found = -1;
    uint16_t fclust = 0;
    uint32_t fsize_bytes = 0;
    for (int s = 0; s < 16 && found < 0; s++)
    {
        esp_err_t err = sd_diag_raw_read(root_sector + s, sec);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "diagf: read root sector %lu failed: %s",
                     (unsigned long)(root_sector + s), esp_err_to_name(err));
            break;
        }
        for (int e = 0; e < 16; e++)
        {
            uint8_t *en = sec + e * 32;
            if (memcmp(en, want, 11) == 0)
            {
                found   = s * 16 + e;
                fclust  = (uint32_t)en[26] | ((uint32_t)en[27] << 8);
                fsize_bytes = (uint32_t)en[28] | ((uint32_t)en[29] << 8) |
                              ((uint32_t)en[30] << 16) | ((uint32_t)en[31] << 24);
                break;
            }
        }
    }
    if (found < 0)
    {
        ESP_LOGE(TAG, "diagf: TEST    TXT NOT found in raw root dir -> 目录项没写进卡！");
        return;
    }
    ESP_LOGI(TAG, "diagf: TEST    TXT found at entry#%d start_cluster=%u size=%lu",
             found, fclust, (unsigned long)fsize_bytes);

    /* FAT 表项：FAT16 2B / FAT12 1.5B（用 1024B 缓冲避免跨扇区越界） */
    uint8_t fatbuf[1024];
    if (fs->fs_type == 2)
    {
        uint32_t fat_off = (uint32_t)fclust * 2;
        uint32_t fat_sect = (uint32_t)fs->fatbase + fat_off / 512;
        if (sd_diag_raw_read(fat_sect, fatbuf) == ESP_OK)
        {
            if (fat_off % 512 > 510)
            {
                sd_diag_raw_read(fat_sect + 1, fatbuf + 512);
            }
            uint16_t val = (uint16_t)(fatbuf[fat_off % 512] | (fatbuf[fat_off % 512 + 1] << 8));
            ESP_LOGI(TAG, "diagf: FAT16 entry = 0x%04X (期望 0xFFFF 文件结束)", val);
        }
    }
    else if (fs->fs_type == 1)
    {
        uint32_t fat_off = (uint32_t)fclust * 3 / 2;
        uint32_t fat_sect = (uint32_t)fs->fatbase + fat_off / 512;
        if (sd_diag_raw_read(fat_sect, fatbuf) == ESP_OK)
        {
            if (fat_off % 512 > 510)
            {
                sd_diag_raw_read(fat_sect + 1, fatbuf + 512);
            }
            uint16_t raw = (uint16_t)(fatbuf[fat_off % 512] | (fatbuf[fat_off % 512 + 1] << 8));
            uint16_t val = (fclust & 1) ? (raw >> 4) : (raw & 0x0FFF);
            ESP_LOGI(TAG, "diagf: FAT12 entry = 0x%03X (期望 0xFFF 文件结束)", val);
        }
    }

    /* 文件数据第一个扇区：data base + (clust-2)*csize */
    uint32_t data_sector = (uint32_t)fs->database + (fclust - 2) * fs->csize;
    ESP_LOGI(TAG, "diagf: file data sector=%lu", (unsigned long)data_sector);
    if (sd_diag_raw_read((uint32_t)data_sector, sec) == ESP_OK)
    {
        ESP_LOGI(TAG, "diagf: raw data = %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                 sec[0], sec[1], sec[2], sec[3], sec[4], sec[5], sec[6], sec[7],
                 sec[8], sec[9], sec[10], sec[11], sec[12], sec[13], sec[14], sec[15]);
        const char *expect = "Hello SD Card! This is a test message.";
        int matched = 0;
        if (fsize_bytes >= 38 && memcmp(sec, expect, 38) == 0)
        {
            matched = 1;
        }
        ESP_LOGI(TAG, "diagf: data content %s",
                 matched ? "MATCHED (数据确实写进了卡)" : "NOT-matched (卡上不是预期内容)");
    }

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
/** 卸载 + f_mkfs 重建 + 重新挂载；挂载成功后清除一次性格式化标志 */
static FRESULT sd_mkfs_and_mount(FATFS *fs_obj, const char *drv)
{
    f_mount(NULL, drv, 0);

    MKFS_PARM opt;
    memset(&opt, 0, sizeof(opt));
    opt.fmt     = (BYTE)(FM_ANY | FM_SFD);   /* 自动 FAT12/16/32，无分区表 */
    opt.n_fat   = 2;
    opt.align   = 0;
    opt.n_root  = 0;                          /* 自动 */
    opt.au_size = 0;                          /* 自动 */

    static uint8_t s_mkfs_work[4096];         /* >= FF_MAX_SS */
    FRESULT fres = f_mkfs(drv, &opt, s_mkfs_work, sizeof(s_mkfs_work));
    if (fres != FR_OK)
    {
        ESP_LOGE(TAG, "FORMAT: f_mkfs failed (%d)", (int)fres);
        return fres;
    }
    ESP_LOGI(TAG, "FORMAT: f_mkfs OK, remounting ...");

    fres = f_mount(fs_obj, drv, 1);
    if (fres == FR_OK)
    {
        s_format_once = false;   /* 本次启动只格式化一次 */
    }
    return fres;
}

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
    if (fres != FR_OK && s_format_once)
    {
        ESP_LOGW(TAG, "mount failed (%d), FORMAT_ONCE: 正在重建文件系统 ...", (int)fres);
        fres = sd_mkfs_and_mount(fs_obj, drv);
    }
    if (fres != FR_OK)
    {
        ESP_LOGE(TAG, "f_mount failed: %d (请确认卡为 FAT32)", (int)fres);
        f_mount(NULL, drv, 0);
        esp_vfs_fat_unregister_path(SD_CARD_MOUNT_POINT);
        ff_diskio_unregister(pdrv);
        return ESP_FAIL;
    }
    if (s_format_once)
    {
        /* 挂载成功但要求强制重建（防止旧结构残留） */
        ESP_LOGW(TAG, "FORMAT_ONCE: 强制重建文件系统");
        fres = sd_mkfs_and_mount(fs_obj, drv);
        if (fres != FR_OK)
        {
            ESP_LOGE(TAG, "FORMAT: 重建失败 (%d)", (int)fres);
            f_mount(NULL, drv, 0);
            esp_vfs_fat_unregister_path(SD_CARD_MOUNT_POINT);
            ff_diskio_unregister(pdrv);
            return ESP_FAIL;
        }
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
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    Sd.owns_bus = Spi_Is_Bus_Created();
    if (Sd.owns_bus)
    {
        ESP_LOGI(TAG, "SPI2 bus created by SD card: SCK=%d MOSI=%d MISO=%d",
                 SD_CARD_SCK_GPIO, SD_CARD_MOSI_GPIO, SD_CARD_MISO_GPIO);
    }
    else
    {
        ESP_LOGW(TAG, "SPI2 bus reused (already owned by LCD)");
    }
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
 * @brief 格式化整张卡（一次性重置）：卸载 → f_mkfs → 重新挂载 → 重建目录
 *
 * @return ESP_OK 成功
 */
void SD_Card_Set_Format_Once(bool enable)
{
    s_format_once = enable;
    ESP_LOGW(TAG, "FORMAT_ONCE=%d（本次启动内生效一次）", enable ? 1 : 0);
}

esp_err_t SD_Card_Format(void)
{
    if (Sd.fs == NULL || !Sd.mounted)
    {
        ESP_LOGE(TAG, "FORMAT: FATFS 未挂载，无法格式化");
        return ESP_ERR_INVALID_STATE;
    }

    char drv[3];
    drv[0] = (char)('0' + Sd.pdrv);
    drv[1] = ':';
    drv[2] = '\0';

    ESP_LOGW(TAG, "FORMAT: unmounting drive %s ...", drv);
    FRESULT res = f_mount(NULL, drv, 0);
    if (res != FR_OK && res != FR_NO_FILESYSTEM)
    {
        ESP_LOGE(TAG, "FORMAT: unmount failed (%d)", res);
        return ESP_FAIL;
    }
    Sd.mounted = false;

    MKFS_PARM opt;
    memset(&opt, 0, sizeof(opt));
    opt.fmt     = (BYTE)(FM_ANY | FM_SFD);   /* 自动 FAT12/16/32，无分区表 */
    opt.n_fat   = 2;
    opt.align   = 0;
    opt.n_root  = 0;                          /* 自动 */
    opt.au_size = 0;                          /* 自动 */

    static uint8_t s_mkfs_work[4096];         /* >= FF_MAX_SS */
    res = f_mkfs(drv, &opt, s_mkfs_work, sizeof(s_mkfs_work));
    if (res != FR_OK)
    {
        ESP_LOGE(TAG, "FORMAT: f_mkfs failed (%d)", res);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "FORMAT: f_mkfs OK, remounting ...");

    res = f_mount(Sd.fs, drv, 1);
    if (res != FR_OK)
    {
        ESP_LOGE(TAG, "FORMAT: remount failed (%d)", res);
        return ESP_FAIL;
    }
    Sd.mounted = true;

    Sd_Card_Ensure_Dirs();                    /* 重建 /music、/ui */
    ESP_LOGI(TAG, "FORMAT: done, volume re-created at %s", SD_CARD_MOUNT_POINT);
    return ESP_OK;
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

/**
 * @brief 一次性格式化开关：置位后，本次 SD_Card_Init 无论挂载失败还是成功，
 *        都会先 f_mkfs 重建干净 FAT 卷（本次成功后自动复位，重启不保留）
 *
 * @param enable true=本次初始化强制格式化一次
 */
void SD_Card_Set_Format_Once(bool enable);

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
            .clockSpeedHz = SD_CARD_INIT_FREQ_HZ,
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
        ESP_LOGI(TAG, "SD device registered: CS=%d, init=%dkHz", SD_CARD_CS_GPIO,
                 SD_CARD_INIT_FREQ_HZ / 1000);
    }

    /* MISO 内部弱上拉：卡 DO 空闲呈高电阻，避免悬空误读 */
    gpio_pullup_en(SD_CARD_MISO_GPIO);

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
