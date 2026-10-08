/**
 * @file i2s_driver.c
 * @brief I2S 物理总线驱动实现（句柄化、参数化）
 *
 * 实现说明：
 *   - 每个句柄对应一个 I2S 控制器上的单向通道（TX 或 RX）
 *   - 所有硬件参数（引脚、采样率、位宽、DMA）均来自调用方配置
 *   - write/read 内部校验句柄方向，方向不匹配返回 ESP_ERR_INVALID_ARG
 *   - 本层不做任何锁与仲裁，互斥由上层模块负责
 */

#include "i2s_driver.h"
#include "esp_log.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#define TAG "I2S_PHY"

/** 句柄魔数，用于校验非法/悬空句柄 */
#define I2S_PHY_MAGIC 0x49325350u /* 'I2SP' */

/** I2S 物理总线实例 */
struct i2s_bus_phy_s {
    uint32_t magic;            /**< 魔数校验 */
    i2s_port_t port;           /**< I2S 控制器端口 */
    bool is_tx;                /**< true=TX 通道，false=RX 通道 */
    i2s_chan_handle_t chan;    /**< 底层通道句柄 */
    i2s_pin_cfg_t pin_cfg;     /**< 引脚配置备份 */
    i2s_bus_cfg_t bus_cfg;     /**< 音频格式配置备份 */
};

/* ======================== 内部工具 =============================================== */

/** @brief 校验句柄合法性 */
static bool i2s_phy_valid(i2s_bus_handle_t bus)
{
    return (bus != NULL) && (bus->magic == I2S_PHY_MAGIC);
}

/* ======================== 公共 API ========================================== */

esp_err_t i2s_bus_phy_create(i2s_port_t port, bool is_tx,
                             const i2s_pin_cfg_t *pin_cfg,
                             const i2s_bus_cfg_t *bus_cfg,
                             i2s_bus_handle_t *out_handle)
{
    /* 参数校验 */
    if (pin_cfg == NULL || bus_cfg == NULL || out_handle == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    /* TX 必须有数据输出脚，RX 必须有数据输入脚 */
    if (is_tx && (pin_cfg->dout < 0))
    {
        ESP_LOGE(TAG, "TX bus requires a valid dout pin");
        return ESP_ERR_INVALID_ARG;
    }
    if (!is_tx && (pin_cfg->din < 0))
    {
        ESP_LOGE(TAG, "RX bus requires a valid din pin");
        return ESP_ERR_INVALID_ARG;
    }

    /* 分配句柄 */
    i2s_bus_handle_t bus = calloc(1, sizeof(struct i2s_bus_phy_s));
    if (bus == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    bus->magic = I2S_PHY_MAGIC;
    bus->port = port;
    bus->is_tx = is_tx;
    memcpy(&bus->pin_cfg, pin_cfg, sizeof(bus->pin_cfg));
    memcpy(&bus->bus_cfg, bus_cfg, sizeof(bus->bus_cfg));

    /*
     * 步骤 1：创建 I2S 通道。
     *  - 一柄一方向：TX 时只传 tx 句柄，RX 时只传 rx 句柄
     *  - TX 使用 auto_clear，播放完自动清零 DMA 防止重复噪声
     */
    i2s_chan_config_t chan_cfg = {
        .id = port,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = bus_cfg->dma_desc_num,
        .dma_frame_num = bus_cfg->dma_frame_num,
        .auto_clear = is_tx && bus_cfg->tx_auto_clear,
    };

    i2s_chan_handle_t *tx_handle = is_tx ? &bus->chan : NULL;
    i2s_chan_handle_t *rx_handle = is_tx ? NULL : &bus->chan;
    esp_err_t ret = i2s_new_channel(&chan_cfg, tx_handle, rx_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(ret));
        free(bus);
        return ret;
    }

    /*
     * 步骤 2：配置标准模式（Philips）。
     * slot_mask / ws_pol / bit_shift 全部由调用方配置，
     * INMP441 麦克风需要 slot_mask=RIGHT + ws_pol=true。
     */
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(bus_cfg->sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bus_cfg->bit_width, bus_cfg->slot_mode),
        .gpio_cfg = {
            .mclk = (gpio_num_t)pin_cfg->mclk,
            .bclk = (gpio_num_t)pin_cfg->bclk,
            .ws   = (gpio_num_t)pin_cfg->ws,
            .dout = (gpio_num_t)pin_cfg->dout,
            .din  = (gpio_num_t)pin_cfg->din,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.slot_cfg.slot_mask  = bus_cfg->slot_mask;
    std_cfg.slot_cfg.ws_pol     = bus_cfg->slot_ws_pol;
    std_cfg.slot_cfg.bit_shift  = bus_cfg->slot_bit_shift;

    ret = i2s_channel_init_std_mode(bus->chan, &std_cfg);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(ret));
        i2s_del_channel(bus->chan);
        free(bus);
        return ret;
    }

    /* 步骤 3：使能通道，启动 SCK/WS 时钟 */
    ret = i2s_channel_enable(bus->chan);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(ret));
        i2s_del_channel(bus->chan);
        free(bus);
        return ret;
    }

    ESP_LOGI(TAG, "I2S phy created: port=%d, %s, sample_rate=%" PRIu32
             ", bclk=%d, ws=%d, dout=%d, din=%d",
             (int)port, is_tx ? "TX" : "RX", bus_cfg->sample_rate,
             pin_cfg->bclk, pin_cfg->ws, pin_cfg->dout, pin_cfg->din);

    *out_handle = bus;
    return ESP_OK;
}

esp_err_t i2s_bus_phy_write(i2s_bus_handle_t bus, const uint8_t *buffer,
                            size_t size, size_t *bytes_written, uint32_t timeout)
{
    /* 校验句柄、方向与参数 */
    if (!i2s_phy_valid(bus) || !bus->is_tx || buffer == NULL || size == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    return i2s_channel_write(bus->chan, buffer, size, bytes_written, timeout);
}

esp_err_t i2s_bus_phy_read(i2s_bus_handle_t bus, uint8_t *buffer,
                           size_t size, size_t *bytes_read, uint32_t timeout)
{
    /* 校验句柄、方向与参数 */
    if (!i2s_phy_valid(bus) || bus->is_tx || buffer == NULL || size == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    return i2s_channel_read(bus->chan, buffer, size, bytes_read, timeout);
}

esp_err_t i2s_bus_phy_flush(i2s_bus_handle_t bus)
{
    if (!i2s_phy_valid(bus))
    {
        return ESP_ERR_INVALID_ARG;
    }
    /* 禁能再使能：清空 DMA 中排队而未播放/未读出的数据 */
    esp_err_t ret = i2s_channel_disable(bus->chan);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "disable failed during flush: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = i2s_channel_enable(bus->chan);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "enable failed during flush: %s", esp_err_to_name(ret));
        return ret;
    }
    return ESP_OK;
}

esp_err_t i2s_bus_phy_set_sample_rate(i2s_bus_handle_t bus, uint32_t sample_rate)
{
    if (!i2s_phy_valid(bus))
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = i2s_channel_disable(bus->chan);
    if (ret != ESP_OK)
    {
        return ret;
    }

    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    ret = i2s_channel_reconfig_std_clock(bus->chan, &clk_cfg);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "reconfig clock failed: %s", esp_err_to_name(ret));
        i2s_channel_enable(bus->chan);
        return ret;
    }

    ret = i2s_channel_enable(bus->chan);
    if (ret == ESP_OK)
    {
        bus->bus_cfg.sample_rate = sample_rate;
        ESP_LOGI(TAG, "sample rate switched to %u Hz", sample_rate);
    }
    return ret;
}

esp_err_t i2s_bus_phy_destroy(i2s_bus_handle_t bus)
{
    if (bus == NULL)
    {
        return ESP_OK;
    }
    if (bus->magic != I2S_PHY_MAGIC)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (bus->chan != NULL)
    {
        i2s_channel_disable(bus->chan);
        i2s_del_channel(bus->chan);
    }
    bus->magic = 0;
    free(bus);
    ESP_LOGI(TAG, "I2S phy destroyed");
    return ESP_OK;
}