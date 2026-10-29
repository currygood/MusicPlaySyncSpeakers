#include "spi_driver.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_memory_utils.h"   /* esp_ptr_dma_capable：判断缓冲是否可交给 SPI DMA */
#include <stdatomic.h>
#include <string.h>

#if SPI_DRIVER_DEBUG_LOGS
static const char *TAG = "SPI_Driver";
#define SPI_LOGE(...) ESP_LOGE(TAG, __VA_ARGS__)
#define SPI_LOGW(...) ESP_LOGW(TAG, __VA_ARGS__)
#define SPI_LOGI(...) ESP_LOGI(TAG, __VA_ARGS__)
#else
#define SPI_LOGE(...) do { } while (0)
#define SPI_LOGW(...) do { } while (0)
#define SPI_LOGI(...) do { } while (0)
#endif

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

/* 单笔事务上限 SPI_MAX_TRANSFER_SZ 已移到 spi_driver.h：
 * 它必须与 UI 的刷屏缓冲高度匹配（否则 spi_master 会拒收整笔事务，
 * 屏幕就画不出来），所以放在头文件里让 UI 侧用 _Static_assert 交叉校验。 */

/* ======================== 会话仲裁默认参数 ====================================== */

/** 单段连续占用预算（ms）：超时后在协议安全点让出总线给高优先级设备 */
#define SPI_SESSION_BUDGET_MS_DEFAULT   10

/** 防饿死阈值（ms）：连续等待超过该值，本设备临时按最高优先级仲裁 */
#define SPI_STARVATION_MS_DEFAULT       500

/** 让出节流（µs）：距上次让出不足该值时不重复让出，避免总线来回切换开销 */
#define SPI_YIELD_THROTTLE_US           1000

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
    int                 priority;         /* 仲裁优先级（spi_prio_t） */
    const char         *label;            /* 设备名（日志/统计） */
    spi_device_handle_t espidf_handle;    /* ESP-IDF 底层设备句柄 */

    /* 会话仲裁统计 */
    uint32_t            acquires;         /* 成功申请次数（含让出后重新申请） */
    uint32_t            waits;            /* 因总线被占而等待次数 */
    uint32_t            yields;           /* 主动让出次数 */
    uint32_t            held_total_us;    /* 累计持有时长 */
    uint32_t            held_max_us;      /* 单次最长持有 */
    uint32_t            wait_total_us;    /* 累计等待时长 */
    uint32_t            wait_max_us;      /* 单次最长等待 */
};

/** 从设备池（句柄指向此数组中的元素） */
static struct spi_device_s Spi_Devices[SPI_MAX_DEVICES];

/**
 * 仲裁共享状态（多任务访问，统一由 s_arb_mutex 保护）：
 *   - wait_mask    ：全局等待者优先级位图，bit(prio) 置位表示有该优先级会话在等；
 *   - dev_wait_cnt ：每设备等待任务数（同一设备可能被多个任务同时使用）；
 *   - dev_wait_prio：每设备等待者的最高优先级，用于“有比我高的在等”。
 * 每个 waiter 的等待起点存放在各自会话结构里（跨任务不混账）。
 */
static SemaphoreHandle_t s_arb_mutex = NULL;
static uint32_t          s_wait_mask = 0;
static uint8_t           s_dev_wait_cnt[SPI_MAX_DEVICES];
static uint8_t           s_dev_wait_prio[SPI_MAX_DEVICES];

/** 会话结构：优先级准入 + 会话预算 + 防饿死 + 统计 */
typedef struct {
    Spi_Handle_t handle;         /* 绑定的从设备 */
    int          priority;       /* 本次会话生效优先级（防饿死时会被临时提升） */
    int          index;          /* 设备池下标（统计用） */
    uint32_t     budget_ms;      /* 会话预算 */
    uint32_t     starvation_ms;  /* 防饿死阈值 */
    uint32_t     segment_grant_ms; /* 本段被授予的连续占用时长（动态防饿死窗口） */
    uint32_t     wait_accum_ms;  /* 本次等待累计（连续拿不到总线的时长，防饿死用） */
    int64_t      wait_start_us;  /* 本次等待起点（0=当前未在等待） */
    int64_t      seg_start_us;   /* 本段占用起点（预算判定用） */
    int64_t      last_yield_us;  /* 上次让出时刻（节流用） */
    atomic_int   yield_flag;     /* 判定结果：1=建议让出 */
    int          holds_bus;      /* 当前是否实际持有总线（让出期间为 0，标记仍有效） */
} spi_session_t;

/** 清空某设备的仲裁统计 */
static void Spi_Arb_Reset_Stats(int index)
{
    struct spi_device_s *d = &Spi_Devices[index];
    d->acquires      = 0;
    d->waits         = 0;
    d->yields        = 0;
    d->held_total_us = 0;
    d->held_max_us   = 0;
    d->wait_total_us = 0;
    d->wait_max_us   = 0;
}

/** 优先级归一到合法区间 [SPI_PRIO_LOW, 7]（0 按 NORMAL 处理） */
static int Spi_Arb_Prio_Clamp(int prio)
{
    if (prio <= 0)
    {
        prio = SPI_PRIO_NORMAL;
    }
    if (prio > 7)
    {
        prio = 7;
    }
    return prio;
}

/** 全局仲裁状态复位（Spi_Deinit 时调用） */
static void Spi_Arb_Reset(void)
{
    s_wait_mask = 0;
    memset(s_dev_wait_cnt, 0, sizeof(s_dev_wait_cnt));
    memset(s_dev_wait_prio, 0, sizeof(s_dev_wait_prio));
    for (int i = 0; i < SPI_MAX_DEVICES; i++)
    {
        Spi_Arb_Reset_Stats(i);
    }
}

/** 登记等待者：返回等待起点时间戳 */
static int64_t Spi_Arb_Wait_Begin(int index, int prio)
{
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_arb_mutex, portMAX_DELAY);
    s_dev_wait_cnt[index]++;
    if (prio > s_dev_wait_prio[index])
    {
        s_dev_wait_prio[index] = (uint8_t)prio;
    }
    s_wait_mask |= (1u << prio);
    xSemaphoreGive(s_arb_mutex);
    return now;
}

/** 注销等待者并登记等待统计 */
static void Spi_Arb_Wait_End(int index, int64_t start_us)
{
    uint32_t elapsed = (uint32_t)(esp_timer_get_time() - start_us);
    xSemaphoreTake(s_arb_mutex, portMAX_DELAY);
    if (s_dev_wait_cnt[index] > 0)
    {
        s_dev_wait_cnt[index]--;
    }
    if (s_dev_wait_cnt[index] == 0)
    {
        s_dev_wait_prio[index] = 0;
    }
    /* 重算全局等待位图：每个优先级只要还有任意设备在等就保持置位 */
    s_wait_mask = 0;
    for (int i = 0; i < SPI_MAX_DEVICES; i++)
    {
        if (s_dev_wait_cnt[i] > 0)
        {
            s_wait_mask |= (1u << s_dev_wait_prio[i]);
        }
    }
    xSemaphoreGive(s_arb_mutex);

    struct spi_device_s *d = &Spi_Devices[index];
    d->waits++;
    d->wait_total_us += elapsed;
    if (elapsed > d->wait_max_us)
    {
        d->wait_max_us = elapsed;
    }
}

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
    /* 仲裁互斥锁：设备注册/会话申请都经它保护共享状态（幂等创建） */
    if (s_arb_mutex == NULL)
    {
        s_arb_mutex = xSemaphoreCreateMutex();
        if (s_arb_mutex == NULL)
        {
            SPI_LOGE("arbiter mutex create failed");
            return ESP_ERR_NO_MEM;
        }
    }

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
        SPI_LOGI("use default SPI pins: SCK=%d MOSI=%d MISO=%d",
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
        SPI_LOGW("SPI bus already initialized by other module, reuse it");
        return ESP_OK;
    }
    if (ret != ESP_OK)
    {
        SPI_LOGE("SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    SPI_LOGI("SPI bus initialized on SPI2, speed=%dMHz",
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
        SPI_LOGE("SPI bus free failed: %s", esp_err_to_name(ret));
        return ret;
    }
    memset(Spi_Devices, 0, sizeof(Spi_Devices));
    Spi_Arb_Reset();
    s_bus_created = false;
    SPI_LOGI("SPI bus released");
    return ESP_OK;
}

/* ======================== 设备注册 =============================================== */

Spi_Handle_t Spi_Register_Device(const Spi_DeviceConfig_t *config)
{
    if (config == NULL)
    {
        SPI_LOGE("device config is NULL");
        return NULL;
    }
    if (Spi_Find_By_Cs(config->csPin) != NULL)
    {
        SPI_LOGE("CS pin %d already registered", config->csPin);
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
        SPI_LOGE("too many SPI devices (max %d)", SPI_MAX_DEVICES);
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
        SPI_LOGE("SPI device register failed (CS pin %d): %s",
                 config->csPin, esp_err_to_name(ret));
        return NULL;
    }

    dev->used             = true;
    dev->cs_pin           = config->csPin;
    dev->clock_speed_hz   = dev_cfg.clock_speed_hz;
    dev->mode             = dev_cfg.mode;
    dev->queue_size       = config->queueSize;
    dev->cs_manual        = cs_manual;
    dev->priority         = Spi_Arb_Prio_Clamp(config->priority);
    dev->label            = (config->label != NULL) ? config->label : "";
    dev->espidf_handle    = espidf_handle;
    Spi_Arb_Reset_Stats((int)(dev - &Spi_Devices[0]));

    SPI_LOGI("SPI device registered (CS pin %d, %dMHz, mode%d, prio=%d, %s)",
             dev->cs_pin, dev->clock_speed_hz / 1000000, dev->mode,
             dev->priority, dev->label);
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
        SPI_LOGE("device remove failed (CS %d): %s",
                 handle->cs_pin, esp_err_to_name(ret));
        return ret;
    }

    spi_device_handle_t new_handle;
    ret = spi_bus_add_device(SPI2_HOST, &dev_cfg, &new_handle);
    if (ret != ESP_OK)
    {
        SPI_LOGE("device re-add failed (CS %d): %s",
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
    SPI_LOGI("device clock updated (CS %d) -> %d kHz",
             handle->cs_pin, clock_speed_hz / 1000);
    return ESP_OK;
}

int Spi_Get_Priority(Spi_Handle_t handle)
{
    return (handle != NULL) ? handle->priority : 0;
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
            SPI_LOGE("SPI transmit(CS=%d) failed: %s", handle->cs_pin, esp_err_to_name(ret));
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
        SPI_LOGW("Spi_Receive length %d > %d, rejected",
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
            SPI_LOGE("SPI receive(CS=%d) failed: %s", handle->cs_pin, esp_err_to_name(ret));
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
        SPI_LOGW("Spi_Transmit_Receive length %d > %d, rejected",
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
            SPI_LOGE("SPI transmit+receive(CS=%d) failed: %s",
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
        SPI_LOGE("CS pin %d not registered", csPin);
        return ESP_ERR_NOT_FOUND;
    }
    return Spi_Transmit(dev, data, length);
}

esp_err_t Spi_Receive_CS(int csPin, uint8_t *buffer, size_t length)
{
    Spi_Handle_t dev = Spi_Find_By_Cs(csPin);
    if (dev == NULL)
    {
        SPI_LOGE("CS pin %d not registered", csPin);
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
        SPI_LOGE("CS pin %d not registered", csPin);
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

    int index = (int)(handle - &Spi_Devices[0]);
    int64_t wait_us = Spi_Arb_Wait_Begin(index, Spi_Arb_Prio_Clamp(handle->priority));
    /* 独占总线：期间其他从设备（如 LCD）的事务排队等待 */
    esp_err_t ret = spi_device_acquire_bus(handle->espidf_handle, portMAX_DELAY);
    Spi_Arb_Wait_End(index, wait_us);
    if (ret == ESP_OK)
    {
        handle->acquires++;
    }
    return ret;
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

/* ======================== 优先级会话 API（预算 + 让出 + 防饿死） ================ */

/** 申请总线（登记等待、统计等待时长），供基础/会话 API 共用 */
static esp_err_t Spi_Arb_Acquire_Timed(Spi_Handle_t handle, int index, int prio)
{
    int64_t wait_us = Spi_Arb_Wait_Begin(index, prio);
    esp_err_t ret = spi_device_acquire_bus(handle->espidf_handle, portMAX_DELAY);
    Spi_Arb_Wait_End(index, wait_us);
    if (ret == ESP_OK)
    {
        handle->acquires++;
    }
    return ret;
}

void *Spi_Device_Session_Start(Spi_Handle_t handle, const spi_session_cfg_t *cfg)
{
    if (handle == NULL)
    {
        return NULL;
    }

    spi_session_t *s = calloc(1, sizeof(*s));
    if (s == NULL)
    {
        SPI_LOGE("session alloc failed");
        return NULL;
    }

    s->handle        = handle;
    s->index         = (int)(handle - &Spi_Devices[0]);
    s->priority      = Spi_Arb_Prio_Clamp((cfg != NULL) ? cfg->priority : 0);
    s->budget_ms     = (cfg != NULL && cfg->session_budget_ms > 0)
                           ? cfg->session_budget_ms : SPI_SESSION_BUDGET_MS_DEFAULT;
    s->starvation_ms = (cfg != NULL && cfg->starvation_ms > 0)
                           ? cfg->starvation_ms : SPI_STARVATION_MS_DEFAULT;
    s->segment_grant_ms = s->starvation_ms;
    atomic_store(&s->yield_flag, 0);

    if (Spi_Arb_Acquire_Timed(handle, s->index, s->priority) != ESP_OK)
    {
        free(s);
        return NULL;
    }
    s->holds_bus    = 1;
    s->seg_start_us = esp_timer_get_time();
    return s;
}

void Spi_Device_Session_End(void *session)
{
    spi_session_t *s = session;
    if (s == NULL)
    {
        return;
    }

    if (s->holds_bus)
    {
        struct spi_device_s *d = &Spi_Devices[s->index];
        uint32_t held = (uint32_t)(esp_timer_get_time() - s->seg_start_us);
        d->held_total_us += held;
        if (held > d->held_max_us)
        {
            d->held_max_us = held;
        }
        Spi_Device_Release(s->handle);
    }
    free(s);
}

bool Spi_Device_Session_Yield_If_Needed(void *session)
{
    spi_session_t *s = session;
    if (s == NULL || !s->holds_bus)
    {
        return false;
    }
    if (atomic_load(&s->yield_flag) != 0)
    {
        return true;   /* 已判定过：让出前不再反复判定 */
    }

    int      prio   = s->priority;
    int64_t  now    = esp_timer_get_time();
    uint32_t waited = s->wait_accum_ms;
    if (s->wait_start_us > 0)   /* 正在经历让出后的重新等待 */
    {
        waited += (uint32_t)((now - s->wait_start_us) / 1000);
    }
    bool need = false;

    /* 防饿死：连续拿不到总线超过窗口 → 本次判定按最高优先级处理，
     * 让出后与高优先级设备同权排队（ESP-IDF 总线锁同优先级 FIFO） */
    if (waited > s->segment_grant_ms)
    {
        prio = 7;
    }

    xSemaphoreTake(s_arb_mutex, portMAX_DELAY);
    /* 等待者位图里存在优先级高于本会话的设备？ */
    uint32_t higher = (prio >= 7) ? 0u : (s_wait_mask & ~((1u << (prio + 1)) - 1u));
    xSemaphoreGive(s_arb_mutex);
    if (higher != 0)
    {
        need = true;
    }
    if (!need)
    {
        uint32_t seg = (uint32_t)((now - s->seg_start_us) / 1000);
        if (seg >= s->budget_ms)
        {
            need = true;
        }
    }
    /* 节流：刚让出过就不重复让出 */
    if (need && s->last_yield_us != 0 &&
        (now - s->last_yield_us) < SPI_YIELD_THROTTLE_US)
    {
        need = false;
    }
    if (need)
    {
        atomic_store(&s->yield_flag, 1);
    }
    return need;
}

esp_err_t Spi_Device_Session_Yield(void *session)
{
    spi_session_t *s = session;
    if (s == NULL || !s->holds_bus)
    {
        return ESP_ERR_INVALID_ARG;
    }
    atomic_store(&s->yield_flag, 0);

    struct spi_device_s *d = &Spi_Devices[s->index];
    uint32_t held = (uint32_t)(esp_timer_get_time() - s->seg_start_us);
    d->held_total_us += held;
    if (held > d->held_max_us)
    {
        d->held_max_us = held;
    }
    d->yields++;

    /* 结束本段占用：高优先级等待者（如 LCD 刷新）立即插入一笔事务；
     * CS 保持不变，SD 命令序列不中断 */
    Spi_Device_Release(s->handle);
    s->holds_bus = 0;

    s->wait_start_us = esp_timer_get_time();   /* 开始重新等待计时（防饿死累计） */
    esp_err_t ret = Spi_Arb_Acquire_Timed(s->handle, s->index, s->priority);
    if (ret != ESP_OK)
    {
        SPI_LOGE("re-acquire after yield failed: %s", esp_err_to_name(ret));
        s->wait_start_us = 0;
        return ret;   /* 会话失效：调用方应按错误处理并结束会话 */
    }
    /* 重新拿到总线 = 本轮饥饿结束：清零累计，并把防饿死窗口翻倍
     * （上限 8x 初始值），避免此后每次判定都提优先级、频繁让出。
     * 下一次让出后的等待会通过 wait_start_us 重新累计。 */
    if (s->wait_start_us > 0)
    {
        s->wait_accum_ms = 0;
        uint32_t grow = s->segment_grant_ms * 2u;
        uint32_t cap  = s->starvation_ms * 8u;
        if (grow > cap)
        {
            grow = cap;
        }
        s->segment_grant_ms = grow;
    }
    s->wait_start_us = 0;
    s->holds_bus    = 1;
    s->seg_start_us = esp_timer_get_time();
    s->last_yield_us = s->seg_start_us;
    return ESP_OK;
}

esp_err_t Spi_Get_Stats(Spi_Handle_t handle, spi_stats_t *out)
{
    if (handle == NULL || out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    out->label         = handle->label;
    out->acquires      = handle->acquires;
    out->waits         = handle->waits;
    out->yields        = handle->yields;
    out->held_total_us = handle->held_total_us;
    out->held_max_us   = handle->held_max_us;
    out->wait_total_us = handle->wait_total_us;
    out->wait_max_us   = handle->wait_max_us;
    return ESP_OK;
}

void Spi_Dump_Stats(void)
{
    for (int i = 0; i < SPI_MAX_DEVICES; i++)
    {
        struct spi_device_s *d = &Spi_Devices[i];
        if (!d->used)
        {
            continue;
        }
        SPI_LOGI(
                 "[arb] %s (CS %d, prio %d): acq=%lu wait=%lu yield=%lu "
                 "hold=%lu/%lu us wait_total=%lu wait_max=%lu us",
                 d->label, d->cs_pin, d->priority,
                 (unsigned long)d->acquires, (unsigned long)d->waits,
                 (unsigned long)d->yields,
                 (unsigned long)d->held_total_us, (unsigned long)d->held_max_us,
                 (unsigned long)d->wait_total_us, (unsigned long)d->wait_max_us);
    }
}

esp_err_t Spi_Device_SetCS(Spi_Handle_t handle, bool level)
{
    if (handle == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!handle->cs_manual)
    {
        SPI_LOGE("CS pin %d is not manual controlled", handle->cs_pin);
        return ESP_ERR_NOT_SUPPORTED;
    }

    return gpio_set_level(handle->cs_pin, level ? 1 : 0);
}
