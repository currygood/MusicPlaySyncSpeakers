/**
 * @file ui_disp.c
 * @brief UI 显示/触摸/文件系统移植层
 *
 * 移植依据（全部来自本工程实测接口与 LVGL 9.6 公共头文件，无猜测 API）：
 *   - 刷屏：middlewares/LCD_Touch 的 LCD_Set_Window() + LCD_Write_PixelData()
 *     （LCD_Touch.c 注释已预留该 LVGL flush 用法，SPI 时钟 10MHz）；
 *   - 触摸：LCD_TOUCH_FT6336G_Get_Touch_Points() 返回触摸任务缓存快照
 *     （INT 已飞线到 GPIO2，FT6336G 地址 0x38）；
 *   - 触摸总线：**GPIO 模拟 I2C**（SDA=32 / SCL=33，约 50kHz），实现在
 *     LCD_Touch.c 内部。这里刻意不再创建 i2c_master 总线——该驱动在总线异常时
 *     会让中断无限重入并触发 Interrupt WDT，而 level-1 ISR 占住 CPU 后任何
 *     软件都救不回来（2026-10-29 实测）。
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
#include "i2c_driver.h"   /* 仅为 I2C_SDA_GPIO/I2C_SCL_GPIO 引脚宏；
                            * 触摸已改 GPIO 模拟 I2C，不再创建 i2c_master 总线 */
#include "spi_driver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

/* 渲染缓冲：必须落内部 DMA RAM（LCD 经 SPI DMA 直传），是内部 DRAM的大头。
 * 关键：PARTIAL 模式下缓冲高度 = **一次重绘窗口**，整屏 240 行要拆成
 *   240 / rows 段，每一段都把整棵对象树重新遍历一遍、每个相交对象重新绘制一次。
 *   8 行 → 30 段（25 对象 × 30 = 750 次绘制调用）；
 *   16 行 → 15 段（375 次），绘制开销大致减半，且 flush 次数也减半。
 * 16 行 = 320×16×2 = 10.2KB（8 行是 5.1KB，多占 5.1KB 内部 RAM）。
 * 分配失败时仍逐级减半到最小 1 行（极端内存下保证 UI 先活过来）。 */
#define UI_FLUSH_ROWS_MAX   16
#define UI_FLUSH_ROWS_MIN   1

/* 编译期交叉校验：一笔 SPI 事务不能超过总线配置的 max_transfer_sz，
 * 否则 spi_master 会在 check_trans_valid() 直接拒收整笔事务
 * （日志 `txdata transfer > host maximum`），而 LCD 侧只看到"什么都没写进去"——
 * 屏幕停留在旧/花屏状态，且不影响程序继续跑，非常难发现。
 * 这里做成编译期断言，以后调 UI_FLUSH_ROWS_MAX 忘了同步 SPI_MAX_TRANSFER_SZ 会直接编译失败。 */
_Static_assert((size_t)(LCD_WIDTH * UI_FLUSH_ROWS_MAX * sizeof(uint16_t)) <= (size_t)SPI_MAX_TRANSFER_SZ,
               "UI_FLUSH_ROWS_MAX 过大：单笔 SPI 事务超过 SPI_MAX_TRANSFER_SZ（见 bsp/spi_driver.h）");

/* ======================== 渲染期诊断（只读，不参与显示逻辑） ========================
 * 1) 刷屏耗时：把"切页 650ms"拆成「SPI 传输」和「LVGL 软件绘制」两半。
 * 2) 真实栈深：uxTaskGetStackHighWaterMark() 靠扫栈底填充字节来判断，只要某个
 *    深层函数有大块"只写一部分"的局部缓冲，扫描就会停在未写过的填充区，报出
 *    偏大的余量（实测 20KB 栈报余 18100B，随后照样压穿栈底 canary）。
 *    所以这里直接读当前 SP：以 ui_run() 入口的 SP 为基准，取渲染过程中的最小 SP
 *    （= 最深），再加上入口处已用量，就是真实的峰值用量。 */

static uint32_t  s_flushUs;
static uint32_t  s_flushCnt;

static int64_t   s_lastFlushEnd;   /* 上一次 flush 结束时刻；0 = 本轮统计还没开始 */
static uint32_t  s_renderUs;       /* 相邻两次 flush 之间空档的累计 = LVGL 软件绘制 */
static uint32_t  s_renderMax;      /* 单窗口最长绘制耗时 */
static uint32_t  s_renderCnt;

static uintptr_t s_spEntry;      /* ui_run() 入口的 SP（基准） */
static uint32_t  s_usedEntry;    /* ui_run() 入口已用的栈字节 */
static uintptr_t s_spMin;        /* 渲染过程中观察到的最小 SP（最深点） */
static bool      s_spWarned;     /* 深度告警只打一次 */
static bool      s_oversizeWarned; /* 单笔事务超限告警只打一次 */

/** 在 ui_run() 最开头（还没建任何页面）调用，建立深度基准 */
void ui_disp_sp_probe_init(void)
{
    s_spEntry      = (uintptr_t)esp_cpu_get_sp();
    s_usedEntry    = (uint32_t)UI_TASK_STACK_BYTES -
                     (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    s_spMin        = s_spEntry;
    s_spWarned     = false;
    s_lastFlushEnd = 0;
}

/** 取走「自上次调用以来」的刷新统计 + 渲染期峰值栈用量，并复位 */
void ui_disp_stat_take(ui_disp_stat_t *st)
{
    if (st != NULL) {
        st->flush_us      = s_flushUs;
        st->flush_calls   = s_flushCnt;
        st->render_us     = s_renderUs;
        st->render_count  = s_renderCnt;
        st->render_max_us = s_renderMax;
        st->depth_peak    = s_usedEntry + (uint32_t)(s_spEntry - s_spMin);
    }
    s_flushUs      = 0;
    s_flushCnt     = 0;
    s_renderUs     = 0;
    s_renderMax    = 0;
    s_renderCnt    = 0;
    s_lastFlushEnd = 0;
    s_spMin        = s_spEntry;
    s_spWarned     = false;
}

/** 在渲染最深的地方（flush 回调）采样 SP；接近撞底时立刻打一条错误日志 */
static void ui_sp_sample(const char *where)
{
    uintptr_t sp;
    uint32_t  depth;

    if (s_spEntry == 0) {
        return;
    }
    sp = (uintptr_t)esp_cpu_get_sp();
    if (sp < s_spMin) {
        s_spMin = sp;
    }
    depth = s_usedEntry + (uint32_t)(s_spEntry - s_spMin);

    /* 只剩 2KB 就报——这条日志会打在 abort 之前，是定位"渲染期栈爆"的关键证据 */
    if (!s_spWarned && (depth + 2048U > (uint32_t)UI_TASK_STACK_BYTES)) {
        s_spWarned = true;
        ESP_LOGE("UI", "[SP] %s depth=%u B of %u B (headroom %u B) flush#%u",
                 where, (unsigned)depth, (unsigned)UI_TASK_STACK_BYTES,
                 (unsigned)((uint32_t)UI_TASK_STACK_BYTES - depth),
                 (unsigned)s_flushCnt);
    }
}


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
    int64_t t0 = esp_timer_get_time();
    (void)disp;

    /* flush 是从"屏幕重绘"这条链一路调进来的，处在渲染的比较深处，
     * 在这里采 SP 最接近渲染期的真实最深点。 */
    ui_sp_sample("flush");

    /* 上一段 flush 结束 → 现在这段开始，这段时间就是"这一段的绘制耗时"。
     * 首段没有前一次 flush（s_lastFlushEnd==0），不计入；
     * 超过 2s 视为空闲间隔，不计入，避免污染平均/最大值。 */
    if (s_lastFlushEnd != 0)
    {
        int64_t gap = t0 - s_lastFlushEnd;
        if (gap > 0 && gap < 2000000)
        {
            s_renderUs += (uint32_t)gap;
            if ((uint32_t)gap > s_renderMax)
            {
                s_renderMax = (uint32_t)gap;
            }
            s_renderCnt++;
        }
    }

    /* 运行时兜底：虽然上面有 _Static_assert，但如果哪天 LVGL 传进来的区域
     * 比缓冲还大（理论上不会），这里必须吭一声，否则就是"屏幕不动但程序正常"的悬案。 */
    {
        size_t bytes = (size_t)w * (size_t)h * sizeof(uint16_t);
        if (bytes > (size_t)SPI_MAX_TRANSFER_SZ)
        {
            if (!s_oversizeWarned)
            {
                s_oversizeWarned = true;
                ESP_LOGE("UI", "[FLUSH] area %dx%d = %u B > SPI_MAX_TRANSFER_SZ %u B，"
                         "整笔事务会被 spi_master 拒收（屏幕不更新）",
                         (int)w, (int)h, (unsigned)bytes, (unsigned)SPI_MAX_TRANSFER_SZ);
            }
            lv_display_flush_ready(disp);
            return;
        }
    }

    /* ILI9341 SPI 按 big-endian 收 RGB565：发送前整块交换字节序 */
    lv_draw_rgb565_swap(px_map, (uint32_t)(w * h));

    LCD_Set_Window((uint16_t)area->x1, (uint16_t)area->y1,
                   (uint16_t)area->x2, (uint16_t)area->y2);
    LCD_Write_PixelData((const uint16_t *)px_map, (uint32_t)(w * h));

    s_flushUs += (uint32_t)(esp_timer_get_time() - t0);
    s_flushCnt++;
    s_lastFlushEnd = esp_timer_get_time();

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
        UI_LOGD( "img open miss: %s", full);
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
    UI_LOGI( "lv_fs drive 'F:' -> %s/ui_img (flash storage 分区)", UI_IMG_MOUNT_POINT);
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
    int64_t   t_stage = esp_timer_get_time();

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
            UI_LOGW( "SPIFFS storage mount failed: %s（UI 图片将使用占位显示；"
                          "请确认 idf.py flash 已烧录分区镜像）", esp_err_to_name(ret));
        }
        else
        {
            UI_LOGI( "SPIFFS storage mounted: %s (partition 'storage')",
                     UI_IMG_MOUNT_POINT);
        }
    }
    UI_LOGI( "stage: spiffs mount %u ms",
             (unsigned)((esp_timer_get_time() - t_stage) / 1000));

    /* 2) LCD + 触摸初始化（内部注册 LCD SPI 设备、创建触摸轮询任务）。
     *
     * 注意：这里**不再创建 i2c_master 总线**。触摸已改为 GPIO 模拟 I2C
     * （见 LCD_Touch.c 顶部说明）：IDF 的 i2c_master 在总线异常时会让中断
     * 无限重入，触发 Interrupt WDT 直接重启（2026-10-29 实测 up=8453s 崩一次），
     * 而那是 level-1 ISR，任何软件都救不回来。 */
    t_stage = esp_timer_get_time();
    LCD_TOUCH_Init();
    UI_LOGI( "LCD(%dx%d landscape) + FT6336G ready (%u ms)",
             LCD_WIDTH, LCD_HEIGHT,
             (unsigned)((esp_timer_get_time() - t_stage) / 1000));
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
        UI_LOGE( "lv_display_create failed");
        return ESP_FAIL;
    }
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, ui_flush_cb);

    /* 渲染缓冲：DMA 可达的内部 RAM（LCD 直传给 SPI DMA） */
    UI_LOGI( "DMA heap before flush buffer: free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    while (flushBuf == NULL && rows >= UI_FLUSH_ROWS_MIN)
    {
        flushBuf = heap_caps_malloc(LCD_WIDTH * rows * sizeof(uint16_t),
                                    MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (flushBuf == NULL)
        {
            rows--;
        }
    }
    if (flushBuf == NULL)
    {
        UI_LOGE( "DMA flush buffer alloc failed (min=%u bytes, int largest=%u)",
                 (unsigned)(LCD_WIDTH * UI_FLUSH_ROWS_MIN * sizeof(uint16_t)),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
        return ESP_ERR_NO_MEM;
    }
    bufBytes = LCD_WIDTH * rows * sizeof(uint16_t);
    UI_LOGI( "flush buffer: %u rows (%u bytes, DMA internal RAM)",
             (unsigned)rows, (unsigned)bufBytes);
    lv_display_set_buffers(disp, flushBuf, NULL, (uint32_t)bufBytes,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    /* 触摸：指针设备，读 LCD_Touch 缓存快照 */
    indev = lv_indev_create();
    if (indev == NULL)
    {
        UI_LOGE( "lv_indev_create failed");
        return ESP_FAIL;
    }
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, ui_touch_read_cb);
    lv_indev_set_display(indev, disp);

    UI_LOGI( "LVGL %d.%d.%d ported: display 320x240 RGB565 + FT6336G pointer",
             LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
    return ESP_OK;
}
