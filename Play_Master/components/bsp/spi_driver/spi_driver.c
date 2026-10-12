#include "spi_driver.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_memory_utils.h"   /* esp_ptr_dma_capable：判断缓冲是否可交给 SPI DMA */
#include <string.h>

static const char *TAG = "SPI_Driver";

/**
 * SPI DMA 收发缓冲约束（ESP32 + esp_driver_spi，踩坑记录）：
 *
 *  - TX 缓冲：只要求 DMA 可达（内部 RAM），无对齐要求；
 *  - RX 缓冲：要求 DMA 可达，且起始地址 4 字节对齐
 *    （esp_hal_gpspi/esp32/spi_ll.h 的 spi_dma_ll_get_rx_alignment_require
 *     写死 *internal_size = 4，注释为 "esp32 needs 4 bytes alignment on
 *     hardware design"；而 esp32 的 cache_hal_get_cache_line_size 对内部 RAM
 *     返回 0，所以 RX 只有地址要对齐，长度不必对齐）。
 *
 * 只要不满足，ESP-IDF 就会为「每一笔事务」从内部 DMA 堆申请一份私有 bounce
 * 缓冲（spi_common.c: spicommon_dma_setup_priv_buffer）。本项目里 FATFS 的
 * 扇区缓冲在 PSRAM（CONFIG_FATFS_ALLOC_PREFER_EXTRAM=y），而 ESP32 的 SPI DMA
 * 到不了 PSRAM，于是每次 SD 扇区读写都会申请一份 512B 内部缓冲；内部 RAM 一旦
 * 吃紧，该分配失败，驱动的错误清理路径会对空指针做 memcpy 直接 panic
 * （spi_common.c 分配失败时不回写出参 → spi_master.c setup_priv_desc →
 *  uninstall_priv_desc 对 NULL 复制）。
 *
 * 因此本驱动自己做中转：DMA 可达（RX 还需 4 字节对齐）的缓冲直传，否则分块
 * 经静态对齐缓冲搬运（s_rx_dummy / s_rx_bounce / s_tx_bounce）。调用方不必
 * 关心 DMA 约束，也不会再有"每笔事务一次堆分配"的抖动。
 *
 * 注意：分块会把一笔传输拆成多笔事务。硬件片选的设备（LCD）每笔事务之间会插入
 * CS 脉冲，所以这类设备的发送缓冲必须保证 DMA 可达（ui_disp.c 的 flushBuf 用
 * MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL 分配，天然满足）；手动片选的设备（SD 卡）
 * 整个会话 CS 保持低电平，分块没有副作用。
 */
#define SPI_DMA_ALIGN_BYTES  4

/**
 * 单笔 SPI 事务字节上限，必须覆盖最大的一笔传输：
 * 目前是 LCD 局部刷屏缓冲（LCD_WIDTH(320) × UI_FLUSH_ROWS_MAX(8) × 2B = 5120），
 * SD 扇区仅 512B。该值还决定总线初始化时预分配的 DMA 描述符数量
 * （spicommon_dma_desc_alloc：desc = ceil(max / 4092)，每个 12B，TX/RX 各一份），
 * 所以不要按整屏 240×320×2 设置——那会白占内部 RAM。
 * 注意：若调大 UI_FLUSH_ROWS_MAX，需同步调大这里。
 */
#define SPI_MAX_TRANSFER_SZ  (320 * 8 * 2 + 8)

/* ======================== 从设备句柄结构 / 注册表 =============================== */

/**
 * 从设备句柄（不透明 struct spi_device_s）：
 * 绑定 CS 引脚与 ESP-IDF 底层设备句柄，收发时通过 handle->cs_pin 选中设备。
 */
struct spi_device_s
{
    bool                used;
    int                 cs_pin;           /* 片选 GPIO，注册时指定 */
    int                 clock_speed_hz;   /* 该从设备时钟 */
    int                 mode;             /* SPI 模式 0~3 */
    int                 queue_size;       /* 事务队列深度（改时钟重建设备时沿用） */
    bool                cs_manual;        /* 手动片选（SD 卡协议需要会话级 CS 控制） */
    spi_device_handle_t espidf_handle;    /* ESP-IDF 底层设备句柄 */
};

/** 从设备池（句柄指向此数组中的元素） */
static struct spi_device_s Spi_Devices[SPI_MAX_DEVICES];

/**
 * @brief 按 CS 引脚查找已注册的从设备
 * @return 句柄；未注册返回 NULL
 */
static Spi_Handle_t Spi_Find_By_Cs(int cs_pin)
{
    for (int i = 0; i < SPI_MAX_DEVICES; i++)
    {
        if (Spi_Devices[i].used && Spi_Devices[i].cs_pin == cs_pin)
        {
            return &Spi_Devices[i];
        }
    }
    return NULL;
}

/* ======================== 总线初始化 / 释放 ====================================== */

/* SPI2 总线是否由本驱动本次创建（供复用方判断释放权） */
static bool s_bus_created = false;

esp_err_t Spi_Init(Spi_Config_t *config)
{
    /* config 为 NULL 时使用默认宏引脚（同板卡上 LCD 与 SD 共用同一组引脚） */
    Spi_Config_t default_cfg = {
        .clockSpeedHz = 10 * 1000 * 1000,
        .sckPin  = SPI_SCK_GPIO,
        .mosiPin = SPI_MOSI_GPIO,
        .misoPin = SPI_MISO_GPIO,
    };
    if (config == NULL)
    {
        config = &default_cfg;
        ESP_LOGI(TAG, "use default SPI pins: SCK=%d MOSI=%d MISO=%d",
                 SPI_SCK_GPIO, SPI_MOSI_GPIO, SPI_MISO_GPIO);
    }

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = config->mosiPin,
        .miso_io_num = config->misoPin,
        .sclk_io_num = config->sckPin,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SPI_MAX_TRANSFER_SZ
    };

    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret == ESP_ERR_INVALID_STATE)
    {
        ESP_LOGW(TAG, "SPI bus already initialized by other module, reuse it");
        return ESP_OK;
    }
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "SPI bus initialized on SPI2, speed=%dMHz",
             config->clockSpeedHz / 1000000);
    s_bus_created = true;
    return ESP_OK;
}

bool Spi_Is_Bus_Created(void)
{
    return s_bus_created;
}

esp_err_t Spi_Deinit(void)
{
    esp_err_t ret = spi_bus_free(SPI2_HOST);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "SPI bus free failed: %s", esp_err_to_name(ret));
        return ret;
    }
    memset(Spi_Devices, 0, sizeof(Spi_Devices));
    s_bus_created = false;
    ESP_LOGI(TAG, "SPI bus released");
    return ESP_OK;
}

/* ======================== 设备注册 =============================================== */

Spi_Handle_t Spi_Register_Device(const Spi_DeviceConfig_t *config)
{
    if (config == NULL)
    {
        ESP_LOGE(TAG, "device config is NULL");
        return NULL;
    }
    if (Spi_Find_By_Cs(config->csPin) != NULL)
    {
        ESP_LOGE(TAG, "CS pin %d already registered", config->csPin);
        return NULL;
    }

    /* 找到空闲槽位 */
    struct spi_device_s *dev = NULL;
    for (int i = 0; i < SPI_MAX_DEVICES; i++)
    {
        if (!Spi_Devices[i].used)
        {
            dev = &Spi_Devices[i];
            break;
        }
    }
    if (dev == NULL)
    {
        ESP_LOGE(TAG, "too many SPI devices (max %d)", SPI_MAX_DEVICES);
        return NULL;
    }

    bool cs_manual = (config->csManualCtrl != 0);

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = (config->clockSpeedHz > 0) ? config->clockSpeedHz : (10 * 1000 * 1000),
        .mode           = (config->mode >= 0 && config->mode <= 3) ? config->mode : 0,
        .spics_io_num   = cs_manual ? -1 : config->csPin,   /* 手动片选：CS 不由 SPI 外设控制 */
        .queue_size     = config->queueSize,
        .command_bits   = 0,
        .address_bits   = 0,
        .dummy_bits     = 0,
    };

    /* 手动片选设备：CS 引脚改为 GPIO 输出，初始拉高（释放状态） */
    if (cs_manual)
    {
        gpio_config_t io_cfg = {
            .pin_bit_mask = (1ULL << config->csPin),
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en  = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_cfg);
        gpio_set_level(config->csPin, 1);
    }

    spi_device_handle_t espidf_handle;
    esp_err_t ret = spi_bus_add_device(SPI2_HOST, &dev_cfg, &espidf_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "SPI device register failed (CS pin %d): %s",
                 config->csPin, esp_err_to_name(ret));
        return NULL;
    }

    dev->used             = true;
    dev->cs_pin           = config->csPin;
    dev->clock_speed_hz   = dev_cfg.clock_speed_hz;
    dev->mode             = dev_cfg.mode;
    dev->queue_size       = config->queueSize;
    dev->cs_manual        = cs_manual;
    dev->espidf_handle    = espidf_handle;

    ESP_LOGI(TAG, "SPI device registered (CS pin %d, %dMHz, mode%d)",
             dev->cs_pin, dev->clock_speed_hz / 1000000, dev->mode);
    return (Spi_Handle_t)dev;
}

esp_err_t Spi_Device_Set_Clock(Spi_Handle_t handle, int clock_speed_hz)
{
    if (handle == NULL || clock_speed_hz <= 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    /* ESP-IDF 无运行时改频接口，与官方 sdspi_host 一致：移除设备并重建 */
    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = clock_speed_hz,
        .mode           = handle->mode,
        .spics_io_num   = handle->cs_manual ? -1 : handle->cs_pin,
        .queue_size     = handle->queue_size,
        .command_bits   = 0,
        .address_bits   = 0,
        .dummy_bits     = 0,
    };

    esp_err_t ret = spi_bus_remove_device(handle->espidf_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "device remove failed (CS %d): %s",
                 handle->cs_pin, esp_err_to_name(ret));
        return ret;
    }

    spi_device_handle_t new_handle;
    ret = spi_bus_add_device(SPI2_HOST, &dev_cfg, &new_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "device re-add failed (CS %d): %s",
                 handle->cs_pin, esp_err_to_name(ret));
        /* 尝试用旧配置恢复，避免句柄悬空 */
        spi_device_interface_config_t old_cfg = dev_cfg;
        old_cfg.clock_speed_hz = handle->clock_speed_hz;
        spi_device_handle_t restore;
        if (spi_bus_add_device(SPI2_HOST, &old_cfg, &restore) == ESP_OK)
        {
            handle->espidf_handle = restore;
        }
        else
        {
            handle->espidf_handle = NULL;
        }
        return ret;
    }

    handle->espidf_handle  = new_handle;
    handle->clock_speed_hz = clock_speed_hz;
    handle->queue_size     = dev_cfg.queue_size;
    ESP_LOGI(TAG, "device clock updated (CS %d) -> %d kHz",
             handle->cs_pin, clock_speed_hz / 1000);
    return ESP_OK;
}

/* ======================== 传输 API（按句柄，句柄内绑定 CS） ===================== */

/** 中转块字节数（须为 SPI_DMA_ALIGN_BYTES 的整数倍） */
#define SPI_XFER_CHUNK_BYTES 64

/** 单笔 Spi_Receive / Spi_Transmit_Receive 允许的最大字节数（内部按块中转） */
#define SPI_RX_MAX_BYTES     2048

/** RX dummy：只收不发时 MOSI 也要输出时钟，SD 协议要求 MOSI 恒为高 */
static uint8_t s_rx_dummy[SPI_XFER_CHUNK_BYTES] __attribute__((aligned(SPI_DMA_ALIGN_BYTES)));

/** RX 中转缓冲：调用方缓冲不可 DMA（如 PSRAM）或起始地址未对齐时先收这里再回拷 */
static uint8_t s_rx_bounce[SPI_XFER_CHUNK_BYTES] __attribute__((aligned(SPI_DMA_ALIGN_BYTES)));

/** TX 中转缓冲：调用方缓冲不可 DMA（如 PSRAM）时先拷这里再发 */
static uint8_t s_tx_bounce[SPI_XFER_CHUNK_BYTES] __attribute__((aligned(SPI_DMA_ALIGN_BYTES)));

/** TX 能否直传：只要 DMA 可达即可（TX 无对齐要求） */
static bool Spi_Tx_Buffer_Is_Direct(const uint8_t *buffer)
{
    return esp_ptr_dma_capable(buffer);
}

/** RX 能否直传：DMA 可达 + 起始地址 4 字节对齐（见文件顶部说明） */
static bool Spi_Rx_Buffer_Is_Direct(const uint8_t *buffer)
{
    return esp_ptr_dma_capable(buffer) &&
           (((uintptr_t)buffer & (SPI_DMA_ALIGN_BYTES - 1)) == 0);
}

/** 计算本笔事务字节数：需要中转时按 SPI_XFER_CHUNK_BYTES 切块 */
static size_t Spi_Next_Chunk(size_t left, bool direct)
{
    return (!direct && left > SPI_XFER_CHUNK_BYTES) ? SPI_XFER_CHUNK_BYTES : left;
}

esp_err_t Spi_Transmit(Spi_Handle_t handle, const uint8_t *data, size_t length)
{
    if (handle == NULL || data == NULL || length == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    size_t done = 0;
    while (done < length)
    {
        bool           direct = Spi_Tx_Buffer_Is_Direct(data + done);
        size_t         chunk  = Spi_Next_Chunk(length - done, direct);
        const uint8_t *tx     = data + done;

        if (!direct)
        {
            memcpy(s_tx_bounce, tx, chunk);   /* PSRAM 等不可 DMA 缓冲先中转 */
            tx = s_tx_bounce;
        }

        spi_transaction_t trans = {
            .length    = chunk * 8,
            .tx_buffer = tx,
        };

        esp_err_t ret = spi_device_transmit(handle->espidf_handle, &trans);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "SPI transmit(CS=%d) failed: %s", handle->cs_pin, esp_err_to_name(ret));
            return ret;
        }
        done += chunk;
    }
    return ESP_OK;
}

esp_err_t Spi_Transmit_16Bit(Spi_Handle_t handle, uint16_t data)
{
    uint8_t data_buf[2] = {(data >> 8) & 0xFF, data & 0xFF};
    return Spi_Transmit(handle, data_buf, 2);
}

esp_err_t Spi_Receive(Spi_Handle_t handle, uint8_t *buffer, size_t length)
{
    if (handle == NULL || buffer == NULL || length == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (length > SPI_RX_MAX_BYTES)
    {
        ESP_LOGW(TAG, "Spi_Receive length %d > %d, rejected",
                 (int)length, (int)SPI_RX_MAX_BYTES);
        return ESP_ERR_INVALID_ARG;
    }
    if (s_rx_dummy[0] != 0xFF)
    {
        memset(s_rx_dummy, 0xFF, sizeof(s_rx_dummy));
    }

    size_t done = 0;
    while (done < length)
    {
        bool     direct = Spi_Rx_Buffer_Is_Direct(buffer + done);
        size_t   left   = length - done;
        /* 只收不发时 TX 侧靠 s_rx_dummy 发 0xFF，而哑元只有 SPI_XFER_CHUNK_BYTES
         * 字节，所以每笔都不能超过这个长度（不能按直传长度一次收完） */
        size_t   chunk  = (left > SPI_XFER_CHUNK_BYTES) ? SPI_XFER_CHUNK_BYTES : left;
        uint8_t *rx     = direct ? (buffer + done) : s_rx_bounce;

        spi_transaction_t trans = {
            .length    = chunk * 8,
            .tx_buffer = s_rx_dummy,  /* 显式发 0xFF：SD 协议要求 MOSI 恒高 */
            .rx_buffer = rx,
        };

        esp_err_t ret = spi_device_transmit(handle->espidf_handle, &trans);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "SPI receive(CS=%d) failed: %s", handle->cs_pin, esp_err_to_name(ret));
            return ret;
        }
        if (!direct)
        {
            memcpy(buffer + done, rx, chunk);
        }
        done += chunk;
    }
    return ESP_OK;
}

esp_err_t Spi_Transmit_Receive(Spi_Handle_t handle,
                               const uint8_t *tx_buffer, uint8_t *rx_buffer,
                               size_t length)
{
    if (handle == NULL || tx_buffer == NULL || rx_buffer == NULL || length == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (length > SPI_RX_MAX_BYTES)
    {
        ESP_LOGW(TAG, "Spi_Transmit_Receive length %d > %d, rejected",
                 (int)length, (int)SPI_RX_MAX_BYTES);
        return ESP_ERR_INVALID_ARG;
    }

    size_t done = 0;
    while (done < length)
    {
        bool           tx_direct = Spi_Tx_Buffer_Is_Direct(tx_buffer + done);
        bool           rx_direct = Spi_Rx_Buffer_Is_Direct(rx_buffer + done);
        size_t         chunk     = Spi_Next_Chunk(length - done, tx_direct && rx_direct);
        const uint8_t *tx        = tx_buffer + done;
        uint8_t       *rx        = rx_direct ? (rx_buffer + done) : s_rx_bounce;

        if (!tx_direct)
        {
            memcpy(s_tx_bounce, tx, chunk);
            tx = s_tx_bounce;
        }

        spi_transaction_t trans = {
            .length    = chunk * 8,
            .tx_buffer = tx,
            .rx_buffer = rx,
        };

        esp_err_t ret = spi_device_transmit(handle->espidf_handle, &trans);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "SPI transmit+receive(CS=%d) failed: %s",
                     handle->cs_pin, esp_err_to_name(ret));
            return ret;
        }
        if (!rx_direct)
        {
            memcpy(rx_buffer + done, rx, chunk);
        }
        done += chunk;
    }
    return ESP_OK;
}

/* ======================== 传输 API（按 CS 查找从设备） =========================== */

esp_err_t Spi_Transmit_CS(int csPin, const uint8_t *data, size_t length)
{
    Spi_Handle_t dev = Spi_Find_By_Cs(csPin);
    if (dev == NULL)
    {
        ESP_LOGE(TAG, "CS pin %d not registered", csPin);
        return ESP_ERR_NOT_FOUND;
    }
    return Spi_Transmit(dev, data, length);
}

esp_err_t Spi_Receive_CS(int csPin, uint8_t *buffer, size_t length)
{
    Spi_Handle_t dev = Spi_Find_By_Cs(csPin);
    if (dev == NULL)
    {
        ESP_LOGE(TAG, "CS pin %d not registered", csPin);
        return ESP_ERR_NOT_FOUND;
    }
    return Spi_Receive(dev, buffer, length);
}

esp_err_t Spi_Transmit_Receive_CS(int csPin,
                                  const uint8_t *tx_buffer, uint8_t *rx_buffer,
                                  size_t length)
{
    Spi_Handle_t dev = Spi_Find_By_Cs(csPin);
    if (dev == NULL)
    {
        ESP_LOGE(TAG, "CS pin %d not registered", csPin);
        return ESP_ERR_NOT_FOUND;
    }
    return Spi_Transmit_Receive(dev, tx_buffer, rx_buffer, length);
}

/* ======================== 设备会话 API（手动片选） =============================== */

esp_err_t Spi_Device_Acquire(Spi_Handle_t handle)
{
    if (handle == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    /* 独占总线：期间其他从设备（如 LCD）的事务排队等待 */
    return spi_device_acquire_bus(handle->espidf_handle, portMAX_DELAY);
}

esp_err_t Spi_Device_Release(Spi_Handle_t handle)
{
    if (handle == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    spi_device_release_bus(handle->espidf_handle);
    return ESP_OK;
}

esp_err_t Spi_Device_SetCS(Spi_Handle_t handle, bool level)
{
    if (handle == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!handle->cs_manual)
    {
        ESP_LOGE(TAG, "CS pin %d is not manual controlled", handle->cs_pin);
        return ESP_ERR_NOT_SUPPORTED;
    }

    return gpio_set_level(handle->cs_pin, level ? 1 : 0);
}
