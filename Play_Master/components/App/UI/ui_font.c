/**
 * @file ui_font.c
 * @brief 运行时中文字体管理：SPIFFS 顺序预载 PSRAM + 只读内存 VFS + FreeType 三档字号
 *
 * 为什么不能直接把 /storage/font/ui_font.ttf 丢给 FreeType：
 *   SPIFFS 的 lseek 是"从对象索引页 0 开始全盘扫描"，spiffs_object_read 只在
 *   *同一索引页内连续读* 时才复用 fd->cursor_objix_spix。FreeType 打开 face 时要在
 *   head/maxp/hhea/hmtx/cmap/glyf 等表之间反复跳，每一次跨索引页的 seek 都要扫
 *   116 个 LU 页（7.25MB 分区 / 256B 页），实测单次 20~25ms：
 *     - 打开一个 face 就要 10s 以上 → 直接触发 task_wdt；
 *     - 每个新字形的 cmap 查询再付同样代价 → UI 卡死 + 反复关 cache 拖垮 BT 音频
 *       （实测 BT 播放时触发 Interrupt wdt，Music_Play_Task 卡在队列自旋锁）。
 *
 *  所以这里改成：开机把字体当"顺序文件"读一次（顺序读走 spiffs_object_read 的
 *  光标快路径，不需要全盘扫描），整块放 PSRAM；然后注册一个只读内存 VFS
 *  （/fontmem），让 FreeType 用标准 fopen/fread 读内存。渲染热路径再也不碰 flash。
 */

#include "ui_font.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ======================== 路径与常量 ======================== */

/* 降级字体：LVGL 内置 Montserrat 14（仅 ASCII）。
 * 固件里已经没有任何中文字库了（ui_font_cn_16 已移出构建），所以字体链路失败时
 * 中文必然是 □——这正是 Phase 5 的设计：不偷偷塞回中文字体，而是由 ui_run()
 * 切到英文错误页，把失败原因直接显示出来。 */
#define FONT_FALLBACK       (&lv_font_montserrat_14)

#define FONT_TAG            "UIFONT"

#define FONT_SRC_PATH       "/storage/font/ui_font.ttf"    /* SPIFFS 源文件 */
#define FONT_META_PATH      "/storage/font/ui_font.meta"   /* 启动校验依据 */
#define FONT_VFS_MOUNT      "/fontmem"                     /* 内存 VFS 挂载点 */
#define FONT_VFS_FILE       "/ui_font.ttf"                 /* 驱动内看到的相对路径 */
#define FONT_FT_PATH        FONT_VFS_MOUNT FONT_VFS_FILE   /* 交给 FreeType 的完整路径 */

#define FONT_PRELOAD_CHUNK  4096   /* 单次 read 大小：>1 个 SPIFFS 页，顺序读最快 */
#define FONT_PRELOAD_YIELD  8      /* 每 8 个 chunk（32KB）让出一次 CPU，避免 task_wdt */
#define FONT_PRELOAD_YIELD_MS 2    /* 让出时长：1ms 时 WiFi 的 TX burst 常未结束，IDLE0 仍轮不到 */
#define FONT_MIN_SIZE       (256 * 1024)
#define FONT_MAX_SIZE       (6 * 1024 * 1024)

#define FONT_MEMFD_MAX      4      /* 同时打开的内存文件句柄数（FreeType 实际只占 1） */

/* ======================== 模块状态 ======================== */

typedef struct {
    bool     used;
    uint32_t pos;
} font_memfd_t;

static const uint8_t *s_img;                    /* PSRAM 内的 TTF 镜像 */
static size_t         s_img_size;
static bool           s_vfs_ok;
static lv_font_t     *s_fonts[UI_FONT_COUNT];
static font_memfd_t   s_memfd[FONT_MEMFD_MAX];
static char           s_err[112] = "not initialized";

static const uint32_t s_sizes[UI_FONT_COUNT] = { 12, 16, 20 };

/* ======================== 小工具 ======================== */

static esp_err_t font_fail(esp_err_t code, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_err, sizeof(s_err), fmt, ap);
    va_end(ap);
    return code;
}

/* ======================== 只读内存 VFS ========================
 * 把 s_img 暴露成 FONT_VFS_FILE 一个文件。VFS 层会把 fd 平移成从 0 开始，
 * 所以这里直接用 0..FONT_MEMFD_MAX-1 作为槽位索引即可。 */

static int memfs_open(const char *path, int flags, int mode)
{
    (void)mode;

    if (flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND)) {
        errno = EACCES;
        return -1;
    }
    if (s_img == NULL) {
        errno = ENOENT;
        return -1;
    }
    /* VFS 已经把挂载前缀剥掉了，正常看到的是 "/ui_font.ttf"；
     * 兼容直接传完整路径的调用。 */
    if (strcmp(path, FONT_VFS_FILE) != 0 && strcmp(path, FONT_FT_PATH) != 0) {
        errno = ENOENT;
        return -1;
    }
    for (int i = 0; i < FONT_MEMFD_MAX; i++) {
        if (!s_memfd[i].used) {
            s_memfd[i].used = true;
            s_memfd[i].pos  = 0;
            return i;
        }
    }
    errno = EMFILE;
    return -1;
}

static int memfs_close(int fd)
{
    if (fd < 0 || fd >= FONT_MEMFD_MAX || !s_memfd[fd].used) {
        errno = EBADF;
        return -1;
    }
    s_memfd[fd].used = false;
    s_memfd[fd].pos  = 0;
    return 0;
}

static ssize_t memfs_read(int fd, void *dst, size_t size)
{
    if (fd < 0 || fd >= FONT_MEMFD_MAX || !s_memfd[fd].used) {
        errno = EBADF;
        return -1;
    }
    size_t avail = (s_memfd[fd].pos < s_img_size) ? (s_img_size - s_memfd[fd].pos) : 0;
    if (size > avail) {
        size = avail;
    }
    if (size > 0) {
        memcpy(dst, s_img + s_memfd[fd].pos, size);
        s_memfd[fd].pos += (uint32_t)size;
    }
    return (ssize_t)size;
}

static ssize_t memfs_write(int fd, const void *src, size_t size)
{
    (void)fd; (void)src; (void)size;
    errno = EPERM;      /* 只读文件系统 */
    return -1;
}

static off_t memfs_lseek(int fd, off_t offset, int whence)
{
    if (fd < 0 || fd >= FONT_MEMFD_MAX || !s_memfd[fd].used) {
        errno = EBADF;
        return -1;
    }
    int64_t base = 0;
    switch (whence) {
    case SEEK_SET: base = 0;                     break;
    case SEEK_CUR: base = (int64_t)s_memfd[fd].pos; break;
    case SEEK_END: base = (int64_t)s_img_size;   break;
    default:       errno = EINVAL; return -1;
    }
    int64_t target = base + (int64_t)offset;
    if (target < 0) {
        errno = EINVAL;
        return -1;
    }
    if (target > (int64_t)s_img_size) {
        target = (int64_t)s_img_size;   /* 允许 seek 到末尾，不报错（POSIX 语义） */
    }
    s_memfd[fd].pos = (uint32_t)target;
    return (off_t)target;
}

static int memfs_fstat(int fd, struct stat *st)
{
    if (fd < 0 || fd >= FONT_MEMFD_MAX || !s_memfd[fd].used || st == NULL) {
        errno = EBADF;
        return -1;
    }
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFREG | 0444;
    st->st_size = (off_t)s_img_size;
    return 0;
}

static int memfs_fcntl(int fd, int cmd, int arg)
{
    (void)fd; (void)arg;
    if (cmd == F_GETFL) {
        return O_RDONLY;
    }
    return 0;
}

static int memfs_fsync(int fd)
{
    (void)fd;
    return 0;
}

static const esp_vfs_t s_memfs = {
    .flags = ESP_VFS_FLAG_DEFAULT,
    .open  = &memfs_open,
    .close = &memfs_close,
    .read  = &memfs_read,
    .write = &memfs_write,
    .fstat = &memfs_fstat,
    .lseek = &memfs_lseek,
    .fcntl = &memfs_fcntl,
    .fsync = &memfs_fsync,
};

/* ======================== 预载与校验 ======================== */

/** 顺序读整个 TTF 到 PSRAM。禁止落内部 RAM（1.5MB 会直接撑爆 DRAM）。 */
static esp_err_t font_image_load(void)
{
    int fd = open(FONT_SRC_PATH, O_RDONLY);
    if (fd < 0) {
        int e = errno;
        ESP_LOGE(FONT_TAG, "open %s failed, errno=%d", FONT_SRC_PATH, e);
        return font_fail(ESP_ERR_NOT_FOUND, "open %s failed (errno=%d)", FONT_SRC_PATH, e);
    }

    off_t sz = lseek(fd, 0, SEEK_END);
    if (sz <= 0 || lseek(fd, 0, SEEK_SET) != 0) {
        ESP_LOGE(FONT_TAG, "size probe failed, size=%ld", (long)sz);
        close(fd);
        return font_fail(ESP_ERR_INVALID_SIZE, "size probe failed (size=%ld)", (long)sz);
    }
    if (sz < FONT_MIN_SIZE || sz > FONT_MAX_SIZE) {
        ESP_LOGE(FONT_TAG, "unexpected size %ld (expect %d..%d)", (long)sz, FONT_MIN_SIZE, FONT_MAX_SIZE);
        close(fd);
        return font_fail(ESP_ERR_INVALID_SIZE, "unexpected size %ld", (long)sz);
    }

    uint8_t *buf = (uint8_t *)heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        ESP_LOGE(FONT_TAG, "psram alloc %ld failed: psram free=%u",
                 (long)sz, (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        close(fd);
        return font_fail(ESP_ERR_NO_MEM, "psram alloc %ld failed", (long)sz);
    }

    size_t   off    = 0;
    int      chunks = 0;
    int64_t  t0     = esp_timer_get_time();

    while (off < (size_t)sz) {
        size_t want = (size_t)sz - off;
        if (want > FONT_PRELOAD_CHUNK) {
            want = FONT_PRELOAD_CHUNK;
        }
        ssize_t n = read(fd, buf + off, want);
        if (n <= 0) {
            break;      /* 读中断/出错：下面按长度判定失败 */
        }
        off += (size_t)n;
        /* 长循环里必须让出 CPU：本任务跑在 CPU0，阻塞太久会饿死 IDLE0 → task_wdt */
        if ((++chunks % FONT_PRELOAD_YIELD) == 0) {
            vTaskDelay(FONT_PRELOAD_YIELD_MS);
        }
    }
    close(fd);

    int64_t dt_us = esp_timer_get_time() - t0;

    if (off != (size_t)sz) {
        heap_caps_free(buf);
        ESP_LOGE(FONT_TAG, "preload short: %u/%ld B", (unsigned)off, (long)sz);
        return font_fail(ESP_ERR_INVALID_SIZE, "preload short %u/%ld B", (unsigned)off, (long)sz);
    }

    bool magic_ok = (buf[0] == 0x00 && buf[1] == 0x01 && buf[2] == 0x00 && buf[3] == 0x00) ||
                    (memcmp(buf, "true", 4) == 0) ||
                    (memcmp(buf, "OTTO", 4) == 0) ||
                    (memcmp(buf, "ttcf", 4) == 0);
    if (!magic_ok) {
        ESP_LOGE(FONT_TAG, "bad ttf magic: %02X %02X %02X %02X",
                 buf[0], buf[1], buf[2], buf[3]);
        heap_caps_free(buf);
        return font_fail(ESP_ERR_INVALID_STATE, "bad ttf magic %02X%02X%02X%02X",
                         buf[0], buf[1], buf[2], buf[3]);
    }

    s_img      = buf;
    s_img_size = (size_t)sz;

    ESP_LOGI(FONT_TAG, "preload %u B in %lld ms (%.0f KB/s), psram free=%u",
             (unsigned)s_img_size, (long long)(dt_us / 1000),
             (dt_us > 0) ? (1000.0f * (float)s_img_size / (float)dt_us) : 0.0f,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return ESP_OK;
}

/** 从 ui_font.meta 里取 "size=" 的值；取不到返回 -1（meta 缺失不算致命） */
static long font_meta_size(void)
{
    FILE *f = fopen(FONT_META_PATH, "r");
    if (f == NULL) {
        return -1;
    }
    char line[96];
    long val = -1;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, "size=", 5) == 0) {
            val = strtol(line + 5, NULL, 10);
            break;
        }
    }
    fclose(f);
    return val;
}

esp_err_t ui_font_validate(void)
{
    if (s_img == NULL) {
        esp_err_t err = font_image_load();
        if (err != ESP_OK) {
            return err;
        }
    }

    long meta = font_meta_size();
    if (meta < 0) {
        ESP_LOGW(FONT_TAG, "%s missing, skip size check", FONT_META_PATH);
        return ESP_OK;
    }
    if ((long)s_img_size != meta) {
        ESP_LOGE(FONT_TAG, "size mismatch: file=%u meta=%ld", (unsigned)s_img_size, meta);
        return font_fail(ESP_ERR_INVALID_SIZE, "size mismatch file=%u meta=%ld",
                         (unsigned)s_img_size, meta);
    }
    ESP_LOGI(FONT_TAG, "validate ok: %u B matches meta", (unsigned)s_img_size);
    return ESP_OK;
}

/* ======================== 对外接口 ======================== */

uint32_t ui_font_size_of(ui_font_id_t id)
{
    if (id < 0 || id >= UI_FONT_COUNT) {
        return 0;
    }
    return s_sizes[id];
}

const char *ui_font_last_error(void)
{
    return s_err;
}

esp_err_t ui_font_init(void)
{
    if (s_fonts[UI_FONT_TEXT_16] != NULL) {
        return ESP_OK;      /* 幂等 */
    }

    int64_t t_all = esp_timer_get_time();

    esp_err_t err = ui_font_validate();
    if (err != ESP_OK) {
        ESP_LOGE(FONT_TAG, "validate FAILED: %s", s_err);
        return err;
    }
    /* validate 里的 ui_font.meta 是另一个小文件，SPIFFS 为它做一次全盘 LU 扫描
     * （实测 ~1.1s，无法避免：页大小 1024 已是 Kconfig 上限）。
     * 紧接着又要建 3 个 FT face，这段连着不让 CPU，会让 CPU0 的 IDLE0 排不上队。 */
    vTaskDelay(FONT_PRELOAD_YIELD_MS);

    if (!s_vfs_ok) {
        err = esp_vfs_register(FONT_VFS_MOUNT, &s_memfs, NULL);
        if (err != ESP_OK) {
            ESP_LOGE(FONT_TAG, "vfs register %s failed: %s", FONT_VFS_MOUNT, esp_err_to_name(err));
            return font_fail(err, "vfs register %s failed", FONT_VFS_MOUNT);
        }
        s_vfs_ok = true;
    }

    for (int i = 0; i < UI_FONT_COUNT; i++) {
        int64_t t0 = esp_timer_get_time();
        s_fonts[i] = lv_freetype_font_create(FONT_FT_PATH,
                                            LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                            s_sizes[i],
                                            LV_FREETYPE_FONT_STYLE_NORMAL);
        int64_t dt = esp_timer_get_time() - t0;

        if (s_fonts[i] == NULL) {
            ESP_LOGE(FONT_TAG, "%upx create FAILED in %lld us", (unsigned)s_sizes[i], (long long)dt);
            /* 已建好的先留着，调用方判空即可降级；这里只报告失败码 */
            return font_fail(ESP_FAIL, "%upx font create failed", (unsigned)s_sizes[i]);
        }
        ESP_LOGI(FONT_TAG, "%2upx created in %lld us: line_height=%d base_line=%d",
                 (unsigned)s_sizes[i], (long long)dt,
                 (int)s_fonts[i]->line_height, (int)s_fonts[i]->base_line);
    }

    ESP_LOGI(FONT_TAG, "init ok in %lld ms | internal free=%u largest=%u | psram free=%u",
             (long long)((esp_timer_get_time() - t_all) / 1000),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    snprintf(s_err, sizeof(s_err), "ok");
    return ESP_OK;
}

lv_font_t *ui_font_get(ui_font_id_t id)
{
    if (id < 0 || id >= UI_FONT_COUNT) {
        return NULL;
    }
    return s_fonts[id];
}

const lv_font_t *ui_font_text(ui_font_id_t id)
{
    lv_font_t *f = ui_font_get(id);
    if (f != NULL) {
        return f;
    }
    /* 兜底：LVGL 内置 Montserrat 14（ASCII）。中文字形会显示为 □，
     * 但正常流程下 ui_run() 在 ui_font_init() 失败时就直接切英文错误页，
     * 根本不会构建中文页面，所以这里只是防止单个字号取空时崩掉。 */
    return FONT_FALLBACK;
}

void ui_font_deinit(void)
{
    for (int i = 0; i < UI_FONT_COUNT; i++) {
        if (s_fonts[i] != NULL) {
            lv_freetype_font_delete(s_fonts[i]);
            s_fonts[i] = NULL;
        }
    }
    if (s_vfs_ok) {
        esp_vfs_unregister(FONT_VFS_MOUNT);
        s_vfs_ok = false;
    }
    if (s_img != NULL) {
        heap_caps_free((void *)s_img);
        s_img      = NULL;
        s_img_size = 0;
    }
}
