#include "spi_driver.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "SPI_Driver";

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
        .max_transfer_sz = 240 * 320 * 2 + 8
    };

    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret == ESP_ERR_INVALID_STATE)
    {
        ESP_LOGW(TAG, "SPI bus already initialized by other module, reuse it");
        return ret;
    }
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "SPI bus initialized on SPI2, speed=%dMHz",
             config->clockSpeedHz / 1000000);
    return ESP_OK;
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
    dev->cs_manual        = cs_manual;
    dev->espidf_handle    = espidf_handle;

    ESP_LOGI(TAG, "SPI device registered (CS pin %d, %dMHz, mode%d)",
             dev->cs_pin, dev->clock_speed_hz / 1000000, dev->mode);
    return (Spi_Handle_t)dev;
}

/* ======================== 传输 API（按句柄，句柄内绑定 CS） ===================== */

esp_err_t Spi_Transmit(Spi_Handle_t handle, const uint8_t *data, size_t length)
{
    if (handle == NULL || data == NULL || length == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    spi_transaction_t trans = {
        .length    = length * 8,
        .tx_buffer = data,
    };

    esp_err_t ret = spi_device_transmit(handle->espidf_handle, &trans);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "SPI transmit(CS=%d) failed: %s", handle->cs_pin, esp_err_to_name(ret));
    }
    return ret;
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

    spi_transaction_t trans = {
        .length    = length * 8,
        .rx_buffer = buffer,
        /* tx_buffer 为 NULL，主机会自动发送全 1 时钟 */
    };

    esp_err_t ret = spi_device_transmit(handle->espidf_handle, &trans);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "SPI receive(CS=%d) failed: %s", handle->cs_pin, esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t Spi_Transmit_Receive(Spi_Handle_t handle,
                               const uint8_t *tx_buffer, uint8_t *rx_buffer,
                               size_t length)
{
    if (handle == NULL || tx_buffer == NULL || rx_buffer == NULL || length == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    spi_transaction_t trans = {
        .length    = length * 8,
        .tx_buffer = tx_buffer,
        .rx_buffer = rx_buffer,
    };

    esp_err_t ret = spi_device_transmit(handle->espidf_handle, &trans);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "SPI transmit+receive(CS=%d) failed: %s", handle->cs_pin, esp_err_to_name(ret));
    }
    return ret;
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