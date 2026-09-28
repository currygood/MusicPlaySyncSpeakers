#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_psram.h"

#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include "amplifier.h"

static const char *TAG = "MAIN";

/* ======================== SD 卡裸扇区测试（仅测试用） ===========================
 * 目的：验证这张 SD 卡本身是否损坏，与 Play_Master 故障对照。
 * 方法：用 ESP-IDF 官方 sdspi + sdmmc 协议栈，直接对指定扇区做
 *       读原值 -> 写 0x00~0xFF 模式 -> 读回比对 -> 恢复原内容 的回环测试。
 * 如果"写进去读不回来"，说明卡上对应区域损坏（介质问题），与驱动无关。
 *
 * 接线（SPI 模式，卡座/面包板）：
 *   SD 模块   ->  ESP32-S3
 *   VCC(3.3V) ->  3V3
 *   GND       ->  GND
 *   CS(DAT3)  ->  GPIO14
 *   SCK(CLK)  ->  GPIO12
 *   MOSI(CMD) ->  GPIO11
 *   MISO(DAT0)->  GPIO13
 * 引脚如有冲突，只改下面 5 个宏即可。
 */
#define SD_TEST_HOST      SPI2_HOST
#define SD_TEST_DMA_CH    SPI_DMA_CH_AUTO
#define SD_TEST_SCK_GPIO  12
#define SD_TEST_MOSI_GPIO 11
#define SD_TEST_MISO_GPIO 13
#define SD_TEST_CS_GPIO   14
#define SD_TEST_FREQ_KHZ  10000           /* 读卡完成后提升到 10MHz */
#define SD_TEST_ROUNDS    2               /* 每个扇区测 2 轮 */

/**
 * @brief 打印某扇区前 16 字节 hex，便于与故障卡对比
 */
static void sd_hex16(const char *label, uint32_t sector, const uint8_t *buf)
{
    char line[64];
    int n = snprintf(line, sizeof(line), "%s [%05lu]:",
                     label, (unsigned long)sector);
    for (int i = 0; i < 16; i++)
    {
        n += snprintf(line + n, sizeof(line) - n, " %02X", buf[i]);
    }
    ESP_LOGI(TAG, "%s", line);
}

/**
 * @brief 单个扇区回环：读原值 -> 写模式 -> 读回比对 -> 恢复原值
 * @return true=该扇区 PASS（写入可持久化）
 */
static bool sd_loopback(sdmmc_card_t *card, uint32_t sector)
{
    static uint8_t orig[512] __attribute__((aligned(4)));
    static uint8_t pat[512]  __attribute__((aligned(4)));
    static uint8_t chk[512]  __attribute__((aligned(4)));

    esp_err_t err = sdmmc_read_sectors(card, orig, sector, 1);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "LB[%lu]: read-before FAIL err=%s",
                 (unsigned long)sector, esp_err_to_name(err));
        return false;
    }
    sd_hex16("LB before", sector, orig);

    for (int i = 0; i < 512; i++)
    {
        pat[i] = (uint8_t)i;
    }

    err = sdmmc_write_sectors(card, pat, sector, 1);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "LB[%lu]: WRITE FAIL err=%s",
                 (unsigned long)sector, esp_err_to_name(err));
        return false;
    }

    err = sdmmc_read_sectors(card, chk, sector, 1);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "LB[%lu]: re-read FAIL err=%s",
                 (unsigned long)sector, esp_err_to_name(err));
        return false;
    }
    sd_hex16("LB read", sector, chk);

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

    bool pass = (diff == 0);
    if (pass)
    {
        ESP_LOGI(TAG, "LB[%lu]: PASS (512B identical)", (unsigned long)sector);
    }
    else
    {
        ESP_LOGE(TAG, "LB[%lu]: FAIL diff=%d first@%d",
                 (unsigned long)sector, diff, first);
    }

    /* 恢复原内容，尽量不破坏卡上已有数据 */
    err = sdmmc_write_sectors(card, orig, sector, 1);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "LB[%lu]: restore-WRITE FAIL err=%s",
                 (unsigned long)sector, esp_err_to_name(err));
    }
    else
    {
        err = sdmmc_read_sectors(card, chk, sector, 1);
        ESP_LOGI(TAG, "LB[%lu]: restore %s", (unsigned long)sector,
                 (err == ESP_OK && memcmp(chk, orig, 512) == 0) ? "OK" : "FAIL");
    }
    return pass;
}

/**
 * @brief 在关键位置做回环测试（与 Play_Master 故障卡一致的探测点）
 */
static void sd_run_probe(sdmmc_card_t *card)
{
    uint32_t total = (uint32_t)card->csd.capacity;
    uint32_t probes[] = {0, 1, 63, 64, 1000, 40960, total / 2, total - 1};
    size_t n = sizeof(probes) / sizeof(probes[0]);

    ESP_LOGI(TAG, "SD loopback test starts: total=%lu sectors, rounds=%d",
             (unsigned long)total, SD_TEST_ROUNDS);

    int pass = 0, fail = 0;
    for (int round = 1; round <= SD_TEST_ROUNDS; round++)
    {
        for (size_t i = 0; i < n; i++)
        {
            uint32_t sec = probes[i];
            if (sec >= total)
            {
                continue;
            }
            ESP_LOGI(TAG, "---- round %d/%d, sector %lu ----",
                     round, SD_TEST_ROUNDS, (unsigned long)sec);
            if (sd_loopback(card, sec))
            {
                pass++;
            }
            else
            {
                fail++;
            }
        }
    }
    ESP_LOGI(TAG, "SD loopback FINAL: PASS=%d FAIL=%d", pass, fail);
}

/**
 * @brief 官方协议栈原始扇区测试（不经过文件系统）
 */
static esp_err_t sd_run_test(void)
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.max_freq_khz = SD_TEST_FREQ_KHZ;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_TEST_MOSI_GPIO,
        .miso_io_num = SD_TEST_MISO_GPIO,
        .sclk_io_num = SD_TEST_SCK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };

    esp_err_t ret = spi_bus_initialize(host.slot, &bus_cfg, SD_TEST_DMA_CH);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* MISO 空闲悬空，开内部上拉，避免误读 */
    gpio_pullup_en((gpio_num_t)SD_TEST_MISO_GPIO);

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = (gpio_num_t)SD_TEST_CS_GPIO;
    slot_cfg.host_id = host.slot;

    sdspi_dev_handle_t dev_handle = -1;
    ret = sdspi_host_init_device(&slot_cfg, &dev_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "sdspi init device failed: %s", esp_err_to_name(ret));
        spi_bus_free(host.slot);
        return ret;
    }
    host.slot = dev_handle;   /* SDSPI: slot 指向设备句柄 */

    sdmmc_card_t *card = (sdmmc_card_t *)calloc(1, sizeof(sdmmc_card_t));
    if (card == NULL)
    {
        ESP_LOGE(TAG, "out of memory for card");
        sdspi_host_remove_device(dev_handle);
        spi_bus_free(host.slot);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "SD card init (probe 400kHz, then clock up)...");
    ret = sdmmc_card_init(&host, card);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "card init failed: %s", esp_err_to_name(ret));
        free(card);
        sdspi_host_remove_device(dev_handle);
        spi_bus_free(host.slot);
        return ret;
    }

    sdmmc_card_print_info(stdout, card);
    ESP_LOGI(TAG, "OCR=0x%08X, capacity=%lu sectors, sector_size=%u",
             (unsigned)card->ocr, (unsigned long)card->csd.capacity,
             (unsigned)card->csd.sector_size);

    sd_run_probe(card);

    sdspi_host_remove_device(dev_handle);
    free(card);
    spi_bus_free(host.slot);
    return ESP_OK;
}

/* ======================== 5 秒“嘟嘟”测试音（保留，本次未调用） ================ */

#define BEEP_FREQ_HZ        800      /* 方波频率（Hz），听感接近“嘟” */
#define BEEP_SAMPLE_RATE    44100    /* 与功放总线采样率一致（全链路固定 44.1kHz） */
#define BEEP_AMPLITUDE      6000     /* 幅度，避免大音量削波 */
#define BEEP_ON_MS          250      /* 每次鸣响时长 */
#define BEEP_OFF_MS         250      /* 每次静音时长 */
#define BEEP_TOTAL_MS       5000     /* 总时长：5 秒 → 10 声“嘟” */
#define BEEP_CHUNK_SAMPLES  512      /* 每块采样数（512/44.1k ≈ 11.6ms） */

/**
 * @brief 播放 5 秒“嘟嘟”提示音
 */
static void play_beep_test(void)
{
    ESP_LOGI(TAG, "beep test start: 800Hz square, 5s total");

    if (Amplifier_Init() != ESP_OK)
    {
        ESP_LOGE(TAG, "Amplifier_Init failed");
        return;
    }

    int16_t chunk[BEEP_CHUNK_SAMPLES];
    const int period = BEEP_SAMPLE_RATE / BEEP_FREQ_HZ;   /* 55 样本/周期（44.1k/800Hz） */
    const int half   = period / 2;                        /* 10 样本/半周期 */
    int phase = 0;
    uint32_t elapsed_ms = 0;

    while (elapsed_ms < BEEP_TOTAL_MS)
    {
        bool is_sounding = (elapsed_ms % (BEEP_ON_MS + BEEP_OFF_MS)) < BEEP_ON_MS;

        for (int i = 0; i < BEEP_CHUNK_SAMPLES; i++)
        {
            if (is_sounding)
            {
                chunk[i] = ((phase % period) < half) ? BEEP_AMPLITUDE : -BEEP_AMPLITUDE;
            }
            else
            {
                chunk[i] = 0;
            }
            phase++;
        }

        esp_err_t ret = Amplifier_Play_Buffer((const uint8_t *)chunk,
                                              sizeof(chunk), NULL,
                                              pdMS_TO_TICKS(200));
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "beep play failed: %s", esp_err_to_name(ret));
            break;
        }

        elapsed_ms += (BEEP_CHUNK_SAMPLES * 1000) / BEEP_SAMPLE_RATE;
    }

    Amplifier_Deinit();
    ESP_LOGI(TAG, "beep test finished");
}

void app_main(void)
{
    if (esp_psram_is_initialized())
    {
        printf("PSRAM 初始化成功！可用大小: %d 字节\n", esp_psram_get_size());
    }
    else
    {
        printf("PSRAM 未找到或未初始化！\n");
    }

    ESP_LOGI(TAG, "=== SD Card RAW Sector Test Start ===");
    sd_run_test();
    ESP_LOGI(TAG, "=== SD Card Test End ===");

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}