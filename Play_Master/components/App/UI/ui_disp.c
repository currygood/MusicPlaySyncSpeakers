/**
 * @file ui_disp.c
 * @brief UI 显示/触摸/文件系统移植层
 *
 * 移植依据（全部来自本工程实测接口与 LVGL 9.6 公共头文件，无猜测 API）：
 *   - 刷屏：middlewares/LCD_Touch 的 LCD_Set_Window() + LCD_Write_PixelData()
 *     （LCD_Touch.c 注释已预留该 LVGL flush 用法，SPI 时钟 10MHz）；
 *   - 触摸：LCD_TOUCH_FT6336G_Get_Touch_Points() 返回内部任务 20ms 轮询缓存
 *     快照（INT 未接线，FT6336G I2C 地址 0x38）；
 *   - I2C 总线：bsp/i2c_driver 的 I2c_Init_Bus(I2C_PORT, I2C_SDA_GPIO=32,
 *     I2C_SCL_GPIO=33, I2C_FREQ=100kHz)，与 LCD_Touch.c 自检同一初始化方式；
 *   - 图片文件：随固件烧录进 flash storage 分区（main/CMakeLists.txt 的
 *     spiffs_create_partition_image(storage ../storage FLASH_IN_PROJECT)），
 *     esp_vfs_spiffs_register() 挂载到 /storage 后经 lv_fs 自定义盘符 'F'
 *     （VFS stdio 适配）访问："F:/ui_img/x.png" → "/storage/ui_img/x.png"；
 *   - LVGL 9.6：lv_init/lv_tick_set_cb/lv_display_create/lv_display_set_buffers
 *     (PARTIAL)/lv_display_set_flush_cb/lv_indev_create/lv_draw_rgb565_swap。
 *
 * 注意：bsp/spi_driver.c 的 spi_bus_initialize 使用 SPI_DMA_CH_AUTO，故渲染
 * 缓冲必须分配 DMA 可达的内部 RAM（LCD_Write_PixelData 直传该缓冲给 SPI）。
 */

#include "ui_priv.h"
#include "esp_spiffs.h"
#include "i2c_driver.h"
#include "freertos/FreeRTOS.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "UI";

/* 渲染缓冲：40 行 × 320 像素 × 2B = 25.6KB（UI 界面设计 2.4；
 * 分配失败时逐级减半到最小 8 行，尽量保住可运行） */
#define UI_FLUSH_ROWS_MAX   40
#define UI_FLUSH_ROWS_MIN   8

static i2c_master_bus_handle_t s_i2cBus = NULL;

/* ======================== LVGL tick ======================== */

/** 用 esp_timer 毫秒时基驱动 LVGL tick（免单独 tick 任务）
 *  LVGL 9.6 回调原型：typedef uint32_t (*lv_tick_get_cb_t)(void)（tick/lv_tick.h） */
static uint32_t ui_tick_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ======================== 显示适配 ======================== */

static void ui_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    (void)disp;

    /* ILI9341 SPI 按 big-endian 收 RGB565：发送前整块交换字节序 */
    lv_draw_rgb565_swap(px_map, (uint32_t)(w * h));

    LCD_Set_Window((uint16_t)area->x1, (uint16_t)area->y1,
                   (uint16_t)area->x2, (uint16_t)area->y2);
    LCD_Write_PixelData((const uint16_t *)px_map, (uint32_t)(w * h));

    lv_display_flush_ready(disp);
}

/* ======================== 触摸适配 ======================== */

static void ui_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    Touch_Point_t tp;
    (void)indev;

    /* 读 LCD_Touch 内部任务的缓存快照（非阻塞、无 I2C 开销）；
     * FT6336G 支持两点，UI 只用第 1 点（设计文档 1.2） */
    if (LCD_TOUCH_FT6336G_Get_Touch_Points(&tp) && tp.touchCount > 0)
    {
        data->point.x = tp.touchX;
        data->point.y = tp.touchY;
        data->state = LV_INDEV_STATE_PRESSED;
    }
    else
    {
        data->state = LV_INDEV_STATE_RELEASED;
    }
    data->continue_reading = false;
}

/* ======================== lv_fs 'F' 盘符（flash storage 分区图片访问） ======================== */

static lv_fs_drv_t s_fsDrv;

/** "ui_img/x.png"（或 "/ui_img/x.png"）→ "/storage/ui_img/x.png" 后经 VFS 打开 */
static void *ui_fs_open_cb(lv_fs_drv_t *drv, const char *path, lv_fs_mode_t mode)
{
    char full[128];
    FILE *fp;
    (void)drv;

    if (path == NULL || mode != LV_FS_MODE_RD)
    {
        return NULL;
    }
    if (path[0] == '/')
    {
        path++;
    }
    snprintf(full, sizeof(full), "%s/%s", UI_IMG_MOUNT_POINT, path);

    fp = fopen(full, "rb");
    if (fp == NULL)
    {
        ESP_LOGD(TAG, "img open miss: %s", full);
        return NULL;
    }
    return (void *)fp;
}

static lv_fs_res_t ui_fs_close_cb(lv_fs_drv_t *drv, void *file_p)
{
    (void)drv;
    return (fclose((FILE *)file_p) == 0) ? LV_FS_RES_OK : LV_FS_RES_HW_ERR;
}

static lv_fs_res_t ui_fs_read_cb(lv_fs_drv_t *drv, void *file_p, void *buf,
                                 uint32_t btr, uint32_t *br)
{
    FILE *fp = (FILE *)file_p;
    (void)drv;

    *br = (uint32_t)fread(buf, 1, btr, fp);
    /* 文件尾读取字数变少也属正常（LODEPNG 按 *br 判断 EOF），仅错误才报错 */
    return (ferror(fp) != 0) ? LV_FS_RES_HW_ERR : LV_FS_RES_OK;
}

static lv_fs_res_t ui_fs_seek_cb(lv_fs_drv_t *drv, void *file_p, uint32_t pos,
                                 lv_fs_whence_t whence)
{
    int stdio_whence;
    (void)drv;

    /* LV_FS_SEEK_SET/CUR/END 与 stdio SEEK_SET/CUR/END 同义，显式映射 */
    switch (whence)
    {
    case LV_FS_SEEK_SET: stdio_whence = SEEK_SET; break;
    case LV_FS_SEEK_CUR: stdio_whence = SEEK_CUR; break;
    case LV_FS_SEEK_END: stdio_whence = SEEK_END; break;
    default: return LV_FS_RES_INV_PARAM;
    }
    return (fseek((FILE *)file_p, (long)pos, stdio_whence) == 0)
               ? LV_FS_RES_OK : LV_FS_RES_HW_ERR;
}

static lv_fs_res_t ui_fs_tell_cb(lv_fs_drv_t *drv, void *file_p, uint32_t *pos_p)
{
    long pos;
    (void)drv;

    if (pos_p == NULL)
    {
        return LV_FS_RES_INV_PARAM;
    }
    pos = ftell((FILE *)file_p);
    if (pos < 0)
    {
        return LV_FS_RES_HW_ERR;
    }
    *pos_p = (uint32_t)pos;
    return LV_FS_RES_OK;
}

static void ui_fs_register(void)
{
    lv_fs_drv_init(&s_fsDrv);
    s_fsDrv.letter = 'F';
    s_fsDrv.cache_size = 0;
    s_fsDrv.open_cb = ui_fs_open_cb;
    s_fsDrv.close_cb = ui_fs_close_cb;
    s_fsDrv.read_cb = ui_fs_read_cb;
    s_fsDrv.seek_cb = ui_fs_seek_cb;
    s_fsDrv.tell_cb = ui_fs_tell_cb;
    lv_fs_drv_register(&s_fsDrv);
    ESP_LOGI(TAG, "lv_fs drive 'F:' -> %s/ui_img (flash storage 分区)", UI_IMG_MOUNT_POINT);
}

/* ======================== 对内服务 ======================== */

bool ui_img_file_ok(const char *lv_path)
{
    char full[128];
    FILE *fp;

    if (lv_path == NULL)
    {
        return false;
    }
    /* "F:/ui_img/x.png" → "/ui_img/x.png" → "/storage/ui_img/x.png" */
    if (strncmp(lv_path, "F:/", 3) == 0)
    {
        lv_path += 2;
    }
    snprintf(full, sizeof(full), "%s%s", UI_IMG_MOUNT_POINT, lv_path);

    fp = fopen(full, "rb");
    if (fp == NULL)
    {
        return false;
    }
    return (fclose(fp) == 0);
}

/* ======================== 硬件自举与 LVGL 移植入口 ======================== */

esp_err_t ui_hw_bringup(void)
{
    esp_err_t ret;

    /* 1) 挂载 flash storage 分区（UI 图片随固件烧录，见 main/CMakeLists.txt 的
     *    spiffs_create_partition_image）。挂载失败不阻塞 UI（占位降级）。 */
    {
        esp_vfs_spiffs_conf_t spiffsCfg = {
            .base_path = UI_IMG_MOUNT_POINT,
            .partition_label = "storage",
            .max_files = 4,
            .format_if_mount_failed = false,   /* 镜像随固件烧录，不做破坏性格式化 */
        };
        ret = esp_vfs_spiffs_register(&spiffsCfg);
        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG, "SPIFFS storage mount failed: %s（UI 图片将使用占位显示；"
                          "请确认 idf.py flash 已烧录分区镜像）", esp_err_to_name(ret));
        }
        else
        {
            ESP_LOGI(TAG, "SPIFFS storage mounted: %s (partition 'storage')",
                     UI_IMG_MOUNT_POINT);
        }
    }

    /* 2) I2C 总线（FT6336G 触摸用，引脚宏来自 bsp/i2c_driver.h） */
    ret = I2c_Init_Bus(I2C_PORT, (gpio_num_t)I2C_SDA_GPIO,
                       (gpio_num_t)I2C_SCL_GPIO, I2C_FREQ, &s_i2cBus);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "I2c_Init_Bus failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 3) LCD + 触摸初始化（内部注册 LCD SPI 设备、创建触摸轮询任务） */
    LCD_TOUCH_Init(s_i2cBus);
    ESP_LOGI(TAG, "LCD(%dx%d landscape) + FT6336G ready", LCD_WIDTH, LCD_HEIGHT);
    return ESP_OK;
}

esp_err_t ui_lvgl_port_init(void)
{
    static uint8_t *flushBuf = NULL;
    size_t rows = UI_FLUSH_ROWS_MAX;
    size_t bufBytes;
    lv_display_t *disp;
    lv_indev_t *indev;

    lv_init();
    lv_tick_set_cb(ui_tick_cb);
    ui_fs_register();

    /* 显示：320x240 横屏 + RGB565 + 局部渲染（设计文档 2.4） */
    disp = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
    if (disp == NULL)
    {
        ESP_LOGE(TAG, "lv_display_create failed");
        return ESP_FAIL;
    }
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, ui_flush_cb);

    /* 渲染缓冲：DMA 可达的内部 RAM（LCD 直传给 SPI DMA） */
    while (flushBuf == NULL && rows >= UI_FLUSH_ROWS_MIN)
    {
        flushBuf = heap_caps_malloc(LCD_WIDTH * rows * sizeof(uint16_t),
                                    MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (flushBuf == NULL)
        {
            rows /= 2;
        }
    }
    if (flushBuf == NULL)
    {
        ESP_LOGE(TAG, "DMA flush buffer alloc failed");
        return ESP_ERR_NO_MEM;
    }
    bufBytes = LCD_WIDTH * rows * sizeof(uint16_t);
    ESP_LOGI(TAG, "flush buffer: %u rows (%u bytes, DMA internal RAM)",
             (unsigned)rows, (unsigned)bufBytes);
    lv_display_set_buffers(disp, flushBuf, NULL, (uint32_t)bufBytes,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    /* 触摸：指针设备，读 LCD_Touch 缓存快照 */
    indev = lv_indev_create();
    if (indev == NULL)
    {
        ESP_LOGE(TAG, "lv_indev_create failed");
        return ESP_FAIL;
    }
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, ui_touch_read_cb);
    lv_indev_set_display(indev, disp);

    ESP_LOGI(TAG, "LVGL %d.%d.%d ported: display 320x240 RGB565 + FT6336G pointer",
             LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
    return ESP_OK;
}
