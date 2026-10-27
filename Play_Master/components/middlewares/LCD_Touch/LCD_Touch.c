#include "LCD_Touch.h"
#include "i2c_driver.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

#if LCD_TOUCH_DEBUG_LOGS
static const char *TAG = "LCD_Touch";
#define LCD_TOUCH_LOGE(...) ESP_LOGE(TAG, __VA_ARGS__)
#define LCD_TOUCH_LOGW(...) ESP_LOGW(TAG, __VA_ARGS__)
#define LCD_TOUCH_LOGI(...) ESP_LOGI(TAG, __VA_ARGS__)
#else
#define LCD_TOUCH_LOGE(...) do { } while (0)
#define LCD_TOUCH_LOGW(...) do { } while (0)
#define LCD_TOUCH_LOGI(...) do { } while (0)
#endif
static Spi_Handle_t LcdSpiHandle;
static i2c_master_dev_handle_t Ft6336I2cHandle = NULL;
static TaskHandle_t TouchTaskHandle = NULL;
static Touch_Point_t LastTouchData = {0};
static uint32_t s_touchReadFailCount = 0;   /* 连续读失败计数，用于限流打印与慢探测 */

/* 触摸状态告警用独立 tag：LCD_TOUCH_DEBUG_LOGS 关掉时业务日志全部编译掉，
 * 但"触摸彻底读不到（总线不通）"这件事必须能看到一次，否则用户只会觉得
 * 屏幕点了没反应，却在串口里找不到任何线索。 */
static const char *TOUCH_TAG = "Touch";

/* 触摸采样策略 -------------------------------------------------------------
 * 2026-10-27 起 CTP_INT 已飞线到 GPIO2（见 LCD_Touch.h），采样改为：
 *   INT 边沿（实时性） + 兜底轮询（补 NACK 吃掉的边沿与松手事件）。
 *
 * 轮询频率曾经是这个系统最大的风险敞口：一次 I2c_Read_Bytes 在 IDF 6.0 的
 * i2c_master 里是"队列 + 信号量 + ISR"，读 6 字节要 2~3 次 ISR 进出，每次
 * ISR 都要抢 intr_alloc 的全局 spinlock；而 ~6% 的 NACK 率会把驱动推进
 * "任务与硬件 FSM 脱同步"的状态，之后中断在派发层"关中断 + 无限等锁"
 * = Interrupt WDT（i2c_driver.c 顶部注释 + 2026-10-27 的 IDF 本地补丁）。
 * 接 INT 后静止时事务量 ≈ 兜底轮询频率（200ms 一次，5 次/s），只有真正
 * 摸屏才产生额外事务 —— 比纯轮询 25 次/s 低一个数量级。
 *
 * **绝不复位总线**：
 *   I2c_Reset_Bus() → i2c_master_bus_reset() → s_i2c_hw_fsm_reset(clear_bus=true)
 * 会把 GPIO 从 I2C 外设手里抢过来、手动敲 9 个 SCL、再切回去、复位 FSM 寄存器。
 * 而 2026-10-26 那次崩溃（i2c f=2195 / r=0）已经证明：一次总线复位都没做过
 * 照样崩 —— 说明这条路径既不是必要条件、也没有任何收益，直接删掉。
 * （IDF 侧 s_i2c_hw_fsm_reset 内部的自动 reset 无法由应用关闭，已用本地
 *   补丁给它套上 esp_intr_disable/enable，见 IDF i2c_master.c 顶部注释。）
 *
 * 失败策略只剩两条：退避 + 慢探测，目标都是"故障期间把事务量压到最低"。
 *   - 首次失败起：40 → 80 → … → 1000ms；
 *   - 连续失败 >= LCD_TOUCH_DEAD_AFTER_FAILS（配合退避约 3 分钟读不到）：
 *     认为总线长期不通，降到 5s 慢探测 —— 只保留"总线是否恢复"的探针；
 *   - 一旦读成功：INT 模式回到 200ms 兜底（有触摸期间 40ms），轮询模式 40ms。 */
#define LCD_TOUCH_POLL_MS                 40
#define LCD_TOUCH_FALLBACK_POLL_MS        200   /* INT 模式下的静止兜底轮询周期 */
#define LCD_TOUCH_BACKOFF_MAX_MS          1000
#define LCD_TOUCH_DEAD_AFTER_FAILS        200   /* 连续失败到此数 → 进入慢探测 */
#define LCD_TOUCH_BACKOFF_DEAD_MS         5000  /* 慢探测周期（约 5s 一次事务） */
#define LCD_TOUCH_WARN_FAILS              20    /* 连续失败到此数量时告警一次 */


/**
 * @brief 设置D/CX引脚为命令模式（低电平）
 * 
 * 内部宏函数，用于在发送命令前将RS(D/CX)引脚拉低。
 */
#define WRITE_CMD_TO_SET_RS() gpio_set_level((gpio_num_t)LCD_TOUCH_SPI_RS, 0);

/**
 * @brief 设置D/CX引脚为数据模式（高电平）
 *
 * 内部宏函数，用于在发送数据前将RS(D/CX)引脚拉高。
 */
#define WRITE_DATA_TO_SET_RS() gpio_set_level((gpio_num_t)LCD_TOUCH_SPI_RS, 1);

/**
 * @brief 初始化LCD相关的GPIO引脚
 * 
 * 配置LCD模块所需的3个GPIO引脚为输出模式：
 * - RST：硬件复位引脚
 * - RS (D/CX)：命令/数据选择引脚  
 * - LED：背光控制引脚
 * 
 * 初始化后默认关闭背光（LED引脚输出低电平）。
 * 
 * @note 此函数仅供LCD_TOUCH_Init()内部调用
 */
static void LCD_GPIO_Init(void)
{
    gpio_config_t gpioConf = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << LCD_TOUCH_SPI_RST) | (1ULL << LCD_TOUCH_SPI_RS) | (1ULL << LCD_TOUCH_SPI_LED)
    };
    gpio_config(&gpioConf);

    gpio_set_level((gpio_num_t)LCD_TOUCH_SPI_LED, 0);
}

/**
 * @brief 初始化SPI总线并注册LCD设备
 * 
 * 配置SPI2总线参数并注册LCD作为SPI从设备：
 * - SPI时钟频率：10MHz（保守值，可提升至20-40MHz）
 * - 使用DMA通道自动分配
 * - 设备队列深度：7个事务
 * 
 * 初始化成功后将设备句柄保存到LcdSpiHandle全局变量，
 * 后续所有SPI传输都使用该句柄。
 * 
 * @note 此函数仅供LCD_TOUCH_Init()内部调用
 * @note 如果初始化失败会输出错误日志
 */
static void LCD_SPI_Init(void)
{
    Spi_Config_t spiConfig = {
        .clockSpeedHz = LCD_TOUCH_SPI_FREQ_HZ,
        .sckPin = SPI_SCK_GPIO,
        .mosiPin = SPI_MOSI_GPIO,
        .misoPin = SPI_MISO_GPIO
    };

    ESP_ERROR_CHECK(Spi_Init(&spiConfig));

    Spi_DeviceConfig_t lcdDevCfg = {
        .csPin        = LCD_TOUCH_SPI_CS,
        .clockSpeedHz = LCD_TOUCH_SPI_FREQ_HZ,
        .mode         = 0,
        /* LVGL 刷屏走阻塞式 Spi_Transmit，同一设备不会有多笔事务同时在飞，
         * 队列深度 2 足够；队列对象本身占内部 RAM，不必按默认值放大 */
        .queueSize    = 2,
        /* 仲裁：LCD 是实时消费者，优先级最高；SD 卡在安全点让出时优先服务它 */
        .priority     = SPI_PRIO_HIGH,
        .label        = "lcd",
    };
    LcdSpiHandle = Spi_Register_Device(&lcdDevCfg);

    if(LcdSpiHandle == NULL)
    {
        LCD_TOUCH_LOGE("Failed to register LCD SPI device");
    }
}

/**
 * @brief 执行LCD硬件复位序列
 * 
 * 通过控制RST引脚产生低电平脉冲来复位ILI9341芯片：
 * 1. RST拉低 → 进入复位状态
 * 2. 延时30ms → 确保复位完成（见下方说明）
 * 3. RST拉高 → 退出复位状态
 * 4. 延时120ms → 等待芯片内部稳定
 * 
 * 复位后所有寄存器恢复默认值，需要重新配置。
 * 
 * @note 此函数仅供LCD_TOUCH_Init()内部调用
 * @note 总耗时约150ms
 * @see ILI9341数据手册8.2.2节（Software Reset时序要求）
 *
 * @warning 这个 RST 是 **LCD 与 CTP（FT6336G）共用** 的 GPIO25
 *          （见《引脚分配表》），而 CTP 要求低电平 **>20ms** 才算有效复位。
 *          原来只拉低 10ms：LCD 能复位，触摸芯片却可能进不了复位 →
 *          上电后 I2C 上完全没有 ACK，总线一直被拉死（表现为
 *          "i2c.master: bus is still busy / hardware timeout" 连续刷屏）。
 *          所以这里统一给到 30ms，同时满足两颗芯片的数据手册要求。
 */
static void LCD_Hardware_Reset(void)
{
    gpio_set_level((gpio_num_t)LCD_TOUCH_SPI_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(30));
    gpio_set_level((gpio_num_t)LCD_TOUCH_SPI_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
}	


/** 
 * @brief 处理LCD_TOUCH_FT6336G触摸中断服务函数
 * 
 * 当LCD_TOUCH_FT6336G触摸屏检测到触摸事件时，该函数会被调用。
 * 它会将触摸事件通知给触摸处理任务，
 * 并在中断处理完成后返回到主任务。
 * @param arg 未使用参数，保持与中断服务函数签名一致
 */
#if LCD_TOUCH_FT6336_INT >= 0
static void IRAM_ATTR LCD_TOUCH_FT6336G_Int_Isr_Handler(void *arg)
{
    /* 触摸任务在 Step 6 才创建，而中断在 Step 5 已使能；且 CTP_RST 与
     * LCD_RST 共用 GPIO25，复位释放瞬间 CTP 就可能拉低 INT。任务句柄为空
     * 时 vTaskNotifyGiveFromISR(NULL) 会解引用空指针，必须挡掉。 */
    if(TouchTaskHandle == NULL)
    {
        return;
    }
    BaseType_t highPriorityTaskWoken = pdFALSE;
    vTaskNotifyGiveFromISR(TouchTaskHandle, &highPriorityTaskWoken);
    if(highPriorityTaskWoken == pdTRUE)
    {
        portYIELD_FROM_ISR();
    }
}
#endif

/**
 * @brief 处理LCD_TOUCH_FT6336G触摸事件的任务
 * 
 * 该任务负责从LCD_TOUCH_FT6336G触摸屏读取触摸事件，
 * 并将事件数据存储到LastTouchData结构体中。
 * 
 * 注意：某些FT6336G兼容芯片直接输出像素坐标（0~239, 0~319），
 * 而不是标准的12位ADC值（0~4095）。本驱动会自动检测并适配。
 * 
 * @param pvParam 未使用参数，保持与任务创建时的参数一致
 */
static void LCD_TOUCH_FT6336G_Process_Task(void *pvParam)
{
    uint8_t touchBuf[6];
    uint16_t rawX = 0;
    uint16_t rawY = 0;
    uint32_t poll_ms = LCD_TOUCH_POLL_MS;   /* 失败时按指数退避放大 */

    while(1)
    {
        /* CTP_INT 未接（轮询模式）：定时唤醒读点；以后若接上 INT，notify 仍可立即唤醒 */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(poll_ms));

        if(Ft6336I2cHandle == NULL)
        {
            continue;
        }

        // esp_rom_delay_us(100);

        memset(touchBuf, 0, sizeof(touchBuf));

        esp_err_t ret = I2c_Read_Bytes(Ft6336I2cHandle, LCD_TOUCH_FT6336G_REG_TD_STATUS, touchBuf, sizeof(touchBuf));
        if(ret != ESP_OK)
        {
            /* 读失败（总线被从设备拉死 / 芯片未就绪）：清除假的按下状态 */
            LastTouchData.touchCount = 0;
            s_touchReadFailCount++;

            if(s_touchReadFailCount == 1 || s_touchReadFailCount % 50 == 0)
            {
                LCD_TOUCH_LOGW("[FT6336G] read fail #%lu: %s",
                         (unsigned long)s_touchReadFailCount, esp_err_to_name(ret));
            }
            /* 只告警一次：总线不通时这条信息量最大，且必须能在
             * LCD_TOUCH_DEBUG_LOGS=0 的正式固件里看到 */
            if(s_touchReadFailCount == LCD_TOUCH_WARN_FAILS)
            {
                ESP_LOGW(TOUCH_TAG, "I2C 连续读失败 %lu 次（%s），总线疑似不通："
                                    "触摸暂不可用，轮询已退避到 %dms；"
                                    "请检查 CTP_SDA/SCL(32/33) 上拉、接插件与 CTP_RST",
                         (unsigned long)s_touchReadFailCount, esp_err_to_name(ret),
                         (int)LCD_TOUCH_BACKOFF_MAX_MS);
            }

            /* 失败时只做一件事：退避。**绝不动总线**（原因见文件头注释：
             * clear_bus 是最危险窗口，而实测"一次都不复位也照样崩"，说明
             * 它既不是必要条件、也没有收益）。
             * 退避目标是把故障期间的 I2C 事务量压到最低 —— 事务数就是撞上
             * ISR 自锁的样本数。 */
            uint32_t cap = (s_touchReadFailCount >= LCD_TOUCH_DEAD_AFTER_FAILS)
                           ? (uint32_t)LCD_TOUCH_BACKOFF_DEAD_MS
                           : (uint32_t)LCD_TOUCH_BACKOFF_MAX_MS;
            if(poll_ms < cap)
            {
                poll_ms <<= 1;
                if(poll_ms > cap)
                {
                    poll_ms = cap;
                }
            }
            continue;
        }

        /* 读成功：恢复正常采样节奏。
         * INT 模式：静止时兜底 200ms（边沿负责实时性，兜底只补丢失的
         * 边沿/松手事件），有触摸期间 40ms 保证松手能被及时读到；
         * 轮询模式：回到 40ms。
         * 这里不打日志：触摸总线接触不良时会出现"失败 1 次→成功→失败"
         * 的高频抖动，逐次打印会重新把串口刷满。 */
        s_touchReadFailCount = 0;
#if LCD_TOUCH_FT6336_INT >= 0
        poll_ms = (LastTouchData.touchCount > 0) ? LCD_TOUCH_POLL_MS
                                                 : LCD_TOUCH_FALLBACK_POLL_MS;
#else
        poll_ms = LCD_TOUCH_POLL_MS;
#endif

        uint8_t touchCount = touchBuf[0] & 0x0F;
        uint8_t eventFlag = (touchBuf[1] >> 6) & 0x03;

        LastTouchData.touchCount = touchCount;

        if(touchCount > 0 && eventFlag != 0x01)
        {
            rawX = ((uint16_t)(touchBuf[1] & 0x0F) << 8) | touchBuf[2];
            rawY = ((uint16_t)(touchBuf[3] & 0X0F) << 8) | touchBuf[4];

			// ESP_LOGI(TAG, "Touch point: (%d, %d)", rawX, rawY);

            if(rawX <= 240 && rawY <= 320)   /* 触摸屏原始 240x320(竖屏) */
            {
                
                /* 原始触摸坐标 -> 横屏显示坐标(320x240)：
                 * 实测角点 左上角(0,320)->(0,0)、右下角(240,0)->(319,239) */
                LastTouchData.touchX = (rawY >= 320) ? 0 : (319 - rawY);   // 0..319 左->右
                LastTouchData.touchY = (rawX >= 240) ? 239 : rawX;         // 0..239 上->下
            }
        }
        else	// 无触摸事件
        {
            LastTouchData.touchX = 0;
            LastTouchData.touchY = 0;
            
            if(touchCount == 0)
            {
                LastTouchData.touchCount = 0;
            }
        }
    }
}

/**
 * @brief 初始化LCD_TOUCH_FT6336G触摸屏
 * 
 * 配置LCD_TOUCH_FT6336G触摸屏的GPIO引脚、I2C总线、中断服务函数、任务等。
 * 按照FT6336G数据手册第3节的初始化流程执行：
 * 1. 硬件复位
 * 2. 添加I2C设备
 * 3. 验证厂商ID（0x36）
 * 4. 配置工作模式
 * 5. 设置中断和任务
 * 
 * @note 此函数仅供LCD_TOUCH_Init()内部调用
 */
static void LCD_TOUCH_FT6336G_Init(i2c_master_bus_handle_t i2cBusHandle)
{
#if LCD_TOUCH_FT6336_RST != LCD_TOUCH_SPI_RST
    LCD_TOUCH_LOGI("[FT6336G] Step 1: Hardware reset...");

    gpio_config_t rstConf = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << LCD_TOUCH_FT6336_RST)
    };
    gpio_config(&rstConf);

    gpio_set_level((gpio_num_t)LCD_TOUCH_FT6336_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level((gpio_num_t)LCD_TOUCH_FT6336_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    LCD_TOUCH_LOGI("[FT6336G] Step 1: Reset complete");
#else
    /* CTP_RST 与 LCD_RST 共用 GPIO25：LCD 复位时已同时复位触摸，这里不能再拉低 */
    LCD_TOUCH_LOGI("[FT6336G] Step 1: reset shared with LCD_RST, skip");
#endif

    if(i2cBusHandle == NULL)
    {
        LCD_TOUCH_LOGE("[FT6336G] Error: I2C bus handle is NULL");
        return;
    }

    LCD_TOUCH_LOGI("[FT6336G] Step 2: Adding I2C device (addr: 0x%02X)...", LCD_TOUCH_FT6336G_DEV_ADDR);
    esp_err_t ret = I2c_Add_Device(i2cBusHandle, LCD_TOUCH_FT6336G_DEV_ADDR, I2C_FREQ, &Ft6336I2cHandle);
    if(ret != ESP_OK || Ft6336I2cHandle == NULL)
    {
        LCD_TOUCH_LOGE("[FT6336G] Error: Failed to add FT6336G device");
        return;
    }
    LCD_TOUCH_LOGI("[FT6336G] Step 2: Device added successfully");

    vTaskDelay(pdMS_TO_TICKS(30));

    uint8_t chipId = 0xFF;
    ret = I2c_Read_Bytes(Ft6336I2cHandle, LCD_TOUCH_FT6336G_REG_FOCALTECH_ID, &chipId, 1);
    
    if(ret == ESP_OK && chipId == 0x36)
    {
        LCD_TOUCH_LOGI("[FT6336G] Step 3: Chip ID verified (0x%02X)", chipId);
    }
    else
    {
        LCD_TOUCH_LOGW("[FT6336G] Step 3: Chip ID = 0x%02X (continuing...)", chipId);
    }

    LCD_TOUCH_LOGI("[FT6336G] Step 4: Configuring registers...");
    ret = I2c_Write_Reg(Ft6336I2cHandle, LCD_TOUCH_FT6336G_REG_G_MODE, 0x01);
    if(ret != ESP_OK)
    {
        LCD_TOUCH_LOGE("[FT6336G] Error: Failed to write G_MODE register (%s)", esp_err_to_name(ret));
        return;
    }

    ret = I2c_Write_Reg(Ft6336I2cHandle, LCD_TOUCH_FT6336G_REG_PWR_MODE, 0x00);
    if(ret != ESP_OK)
    {
        LCD_TOUCH_LOGW("[FT6336G] Warning: Failed to write PWR_MODE register (%s)", esp_err_to_name(ret));
    }
    LCD_TOUCH_LOGI("[FT6336G] Step 4: Registers configured successfully");

#if LCD_TOUCH_FT6336_INT >= 0
    LCD_TOUCH_LOGI("[FT6336G] Step 5: Setting up interrupt (GPIO%d)...", LCD_TOUCH_FT6336_INT);

    gpio_install_isr_service(ESP_INTR_FLAG_IRAM);

    gpio_config_t intConf = {
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << LCD_TOUCH_FT6336_INT),
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE
    };
    gpio_config(&intConf);

    gpio_isr_handler_add((gpio_num_t)LCD_TOUCH_FT6336_INT, LCD_TOUCH_FT6336G_Int_Isr_Handler, NULL);
    gpio_intr_enable((gpio_num_t)LCD_TOUCH_FT6336_INT);
    LCD_TOUCH_LOGI("[FT6336G] Step 5: Interrupt configured (falling edge)");
#else
    LCD_TOUCH_LOGI("[FT6336G] Step 5: INT not wired, polling every 20ms");
#endif

    LCD_TOUCH_LOGI("[FT6336G] Step 6: Creating touch processing task...");
    /* FT6336G 只做 I2C 轮询 + 快照缓存（栈上仅 6B 缓冲），2KB 足够 */
    xTaskCreatePinnedToCore(LCD_TOUCH_FT6336G_Process_Task, "Touch_Task", 2048, NULL, 3, &TouchTaskHandle, 0);
    LCD_TOUCH_LOGI("[FT6336G] Step 6: Task created (Core 0, Priority 3)");

    LCD_TOUCH_LOGI("[FT6336G] Initialization complete!");
}

/**
 * @brief 向ILI9341发送命令字节
 * 
 * 通过SPI总线向LCD发送单字节的命令代码。
 * 发送前会自动将D/CX引脚拉低（命令模式）。
 * 
 * @param cmd 要发送的命令代码（如0x36表示MADCTL，0x2C表示RAM写入）
 * 
 * @note 命令模式：D/CX = 0（低电平）
 * @see ILI9341数据手册第8章命令列表
 */
void LCD_Write_Cmd(uint8_t cmd)
{
    WRITE_CMD_TO_SET_RS();
    if (Spi_Transmit(LcdSpiHandle, &cmd, 1) != ESP_OK)
    {
        LCD_TOUCH_LOGE("write cmd 0x%02X failed", cmd);
    }
}

/**
 * @brief 向ILI9341发送8位参数数据
 * 
 * 通过SPI总线向LCD发送单字节的参数或数据。
 * 发送前会自动将D/CX引脚拉高（数据模式）。
 * 通常用于设置命令的参数（如MADCTL的值、像素格式等）。
 * 
 * @param data 要发送的8位数据（0x00-0xFF）
 * 
 * @note 数据模式：D/CX = 1（高电平）
 * @note 必须在LCD_Write_Cmd()之后调用以发送参数
 */
void LCD_Write_Data(uint8_t data)
{
    WRITE_DATA_TO_SET_RS();
    if (Spi_Transmit(LcdSpiHandle, &data, 1) != ESP_OK)
    {
        LCD_TOUCH_LOGE("write data 0x%02X failed", data);
    }
}

/**
 * @brief 向ILI9341发送16位像素数据
 * 
 * 通过SPI总线向LCD发送16位的颜色数据（RGB565格式）。
 * 发送前会自动将D/CX引脚拉高（数据模式）。
 * 数据按高位在前、低位在后的顺序传输。
 * 
 * @param data 要发送的16位像素颜色值（RGB565格式）
 *             可使用RGB565(r,g,b)宏生成，例如：
 *             - 红色：RGB565(255, 0, 0) = 0xF800
 *             - 绿色：RGB565(0, 255, 0) = 0x07E0
 *             - 蓝色：RGB565(0, 0, 255) = 0x001F
 * 
 * @note 用于单个像素的颜色填充或测试
 * @note 数据模式：D/CX = 1（高电平）
 * @see RGB565()
 */
void LCD_Write_Data_16Bit(uint16_t data)
{
    WRITE_DATA_TO_SET_RS();
    if (Spi_Transmit_16Bit(LcdSpiHandle, data) != ESP_OK)
    {
        LCD_TOUCH_LOGE("write data 0x%04X failed", data);
    }
}

/**
 * @brief 设置LCD显示窗口区域
 * 
 * 配置ILI9341的列地址范围（CASET，命令0x2A）和页地址范围（PASET，命令0x2B），
 * 指定后续像素数据写入的目标矩形区域。
 * 设置完成后会自动发送RAMWR命令（0x2C）准备接收像素数据。
 * 
 * 该函数是LVGL刷新回调的核心前置操作，用于指定脏矩形区域。
 * 
 * @param xStart 窗口左上角X坐标（列地址），范围：0 ~ LCD_WIDTH-1 (0~239)
 * @param yStart 窗口左上角Y坐标（行地址），范围：0 ~ LCD_HEIGHT-1 (0~319)  
 * @param xEnd   窗口右下角X坐标（列地址），范围：0 ~ LCD_WIDTH-1 (0~239)
 * @param yEnd   窗口右下角Y坐标（行地址），范围：0 ~ LCD_HEIGHT-1 (0~319)
 * 
 * @note 坐标系原点(0,0)位于屏幕左上角
 * @note 必须满足：xStart <= xEnd 且 yStart <= yEnd
 * @note 调用后应立即调用LCD_Write_PixelData()写入像素数据
 * @note 内部发送顺序：
 *       1. CASET命令 + 4字节参数（起始/结束列地址）
 *       2. PASET命令 + 4字节参数（起始/结束行地址）
 *       3. RAMWR命令（准备接收像素数据）
 * 
 * @see ILI9341数据手册8.2.22节（Memory Write时序）
 * @see ILI9341数据手册8.2.29节（MADCTL对地址递增方向的影响）
 * 
 * @code
 * // 示例：设置屏幕中心100×100的区域
 * LCD_Set_Window(70, 110, 170, 210);  // X:70~170, Y:110~210
 * // 接下来写入10000个像素数据...
 * @endcode
 */
void LCD_Set_Window(uint16_t xStart, uint16_t yStart, uint16_t xEnd, uint16_t yEnd)
{
    LCD_Write_Cmd(ILI9341_CASET);
    LCD_Write_Data((xStart >> 8) & 0xFF);
    LCD_Write_Data(xStart & 0xFF);
    LCD_Write_Data((xEnd >> 8) & 0xFF);
    LCD_Write_Data(xEnd & 0xFF);

    LCD_Write_Cmd(ILI9341_PASET);
    LCD_Write_Data((yStart >> 8) & 0xFF);
    LCD_Write_Data(yStart & 0xFF);
    LCD_Write_Data((yEnd >> 8) & 0xFF);
    LCD_Write_Data(yEnd & 0xFF);

    LCD_Write_Cmd(ILI9341_RAMWR);
}

/**
 * @brief 全屏填充指定颜色
 * 
 * 将整个LCD屏幕（240×320像素）填充为指定的单一颜色。
 * 内部会先调用LCD_Set_Window()设置全屏窗口（0,0到239,319），
 * 然后逐个像素循环写入颜色数据。
 * 
 * 对于2.8寸ILI9341屏幕（分辨率240×320），共需写入：
 * - 像素总数：240 × 320 = 76,800 个像素
 * - 数据总量：76,800 × 2字节 = 153,600 字节
 * - 预估耗时：约 12~50ms（取决于SPI时钟频率）
 * 
 * @param color 要填充的16位颜色值（RGB565格式）
 *              常用颜色示例（使用RGB565宏生成）：
 *              - 黑色：RGB565(0, 0, 0)     = 0x0000
 *              - 白色：RGB565(255, 255, 255) = 0xFFFF  
 *              - 红色：RGB565(255, 0, 0)   = 0xF800
 *              - 绿色：RGB565(0, 255, 0)   = 0x07E0
 *              - 蓝色：RGB565(0, 0, 255)   = 0x001F
 *              - 黄色：RGB565(255, 255, 0) = 0xFFE0
 *              - 青色：RGB565(0, 255, 255) = 0x07FF
 *              - 品红：RGB565(255, 0, 255) = 0xF81F
 * 
 * @warning 此操作会遍历全部76,800个像素，在低SPI时钟下可能耗时较长（>100ms）
 * @note 适用于清屏操作或纯色背景场景
 * @note 如果需要高性能大面积填充，建议使用LCD_Write_PixelData()批量传输
 * @note 内部实现：for循环 + Spi_Transmit_16Bit() 逐像素发送
 * 
 * @see RGB565()
 * @see LCD_Write_PixelData()
 * 
 * @code
 * // 示例用法
 * LCD_Fill_Color(RGB565(0, 0, 0));      // 清屏为黑色
 * LCD_Fill_Color(RGB565(64, 64, 64));   // 填充深灰色背景
 * LCD_Fill_Color(0xF800);               // 直接使用十六进制红色
 * @endcode
 */
void LCD_Fill_Color(uint16_t color)
{
    LCD_Set_Window(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);

    WRITE_DATA_TO_SET_RS();

    uint32_t totalPixels = (uint32_t)LCD_WIDTH * LCD_HEIGHT;

    for (uint32_t idx = 0; idx < totalPixels; idx++)
    {
        Spi_Transmit_16Bit(LcdSpiHandle, color);
    }
}

/**
 * @brief 批量写入像素数据到当前窗口
 * 
 * 将连续的像素颜色数组通过SPI总线高速传输到LCD显存（GRAM）。
 * 这是LVGL刷新回调的核心接口，支持DMA批量传输以获得最佳性能。
 * 调用前必须先通过LCD_Set_Window()设置目标窗口区域。
 * 
 * 传输机制：
 * - 数据模式：D/CX保持高电平（数据模式）
 * - 传输方式：单次DMA事务完成所有像素传输
 * - 字节顺序：高位在前（MSB First）
 * - 地址递增：由MADCTL配置决定方向（当前为MX+BGR模式）
 * 
 * 性能特点：
 * - 支持任意大小的数据块（受限于SPI max_transfer_sz 配置）
 * - DMA自动处理，不占用CPU
 * - 典型性能：10MHz SPI下约 15ms/帧（全屏刷新）
 * 
 * @param data        指向像素数据的数组指针（const uint16_t* 类型）
 *                    数据格式为RGB565，每个元素占2字节
 *                    内存布局示例（3个像素）：[R1G1B1][R2G2B2][R3G3B3]
 *                    其中每个16位值为：[R4-R0][G5-G3][G2-G0][B4-B0]
 * @param pixelCount  要写入的像素数量（注意：是像素数，不是字节数！）
 *                    实际传输的字节数 = pixelCount × 2 字节
 *                    例如：100×100区域 → pixelCount=10,000 → 传输20,000字节
 * 
 * @note 传入的数据指针通常来自LVGL渲染缓冲区（lv_color_t数组）
 * @note 调用者需确保data指针有效且包含至少pixelCount个像素数据
 * @note 传输过程中CSX保持低电平，D/CX保持高电平
 * @note 该函数不会自动换行或处理边界，完全依赖LCD_Set_Window()设置的区域
 * 
 * @warning pixelCount过大时可能导致DMA缓冲区溢出，请确保在max_transfer_sz范围内
 * 
 * @see LCD_Set_Window() - 必须先调用此函数设置目标区域
 * @see ILI9341数据手册8.2.22节（Memory Write命令详解）
 * @see ILI9341数据手册9.3节（GRAM地址自动递增机制）
 * 
 * @code
 * // LVGL flush回调中的典型用法（完整示例）
 * void my_flush_callback(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
 * {
 *     // 1. 设置显示窗口（脏矩形区域）
 *     LCD_Set_Window(area->x1, area->y1, area->x2, area->y2);
 *     
 *     // 2. 计算像素数量
 *     int32_t width  = (area->x2 - area->x1 + 1);   // 区域宽度
 *     int32_t height = (area->y2 - area->y1 + 1);   // 区域高度
 *     int32_t count  = width * height;              // 总像素数
 *     
 *     // 3. 批量写入像素数据
 *     LCD_Write_PixelData((const uint16_t *)px_map, (uint32_t)count);
 *     
 *     // 4. 通知LVGL刷新完成
 *     lv_display_flush_ready(disp);
 * }
 * 
 * // 独立使用示例（绘制矩形色块）
 * void draw_color_block(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
 * {
 *     static uint16_t buffer[1024];  // 临时缓冲区
 *     
 *     // 准备颜色数据
 *     for(int i = 0; i < w * h && i < 1024; i++) {
 *         buffer[i] = color;
 *     }
 *     
 *     // 设置窗口并写入
 *     LCD_Set_Window(x, y, x + w - 1, y + h - 1);
 *     LCD_Write_PixelData(buffer, w * h);
 * }
 * @endcode
 */
void LCD_Write_PixelData(const uint16_t *data, uint32_t pixelCount)
{
    WRITE_DATA_TO_SET_RS();
    if (Spi_Transmit(LcdSpiHandle, (const uint8_t *)data, pixelCount * 2) != ESP_OK)
    {
        LCD_TOUCH_LOGE("write %lu pixels failed", (unsigned long)pixelCount);
    }
}

/**
 * @brief 控制LCD背光开关
 * 
 * 通过控制LED背光引脚的电平来开启或关闭屏幕背光照明。
 * 关闭背光后，LCD仍可正常接收和显示数据，只是肉眼不可见。
 * 此功能可用于：
 * - 省电模式（降低功耗约50-80%）
 * - 屏幕闪烁效果（报警提示）
 * - 隐私保护（快速关闭显示）
 * - OLED防烧屏（长时间静态画面时）
 * 
 * @param state 背光状态标志
 *              - true  : 开启背光（LED引脚输出高电平，通常3.3V）
 *              - false : 关闭背光（LED引脚输出低电平，0V）
 * 
 * @note 背光引脚由LCD_TOUCH_LED宏定义（默认GPIO41，可在LCD_Touch.h中修改）
 * @note 背光响应时间：< 1ms（几乎瞬时）
 * @note 频繁开关不会损坏硬件（LED寿命通常>50,000小时）
 * @note 某些LCD模块可能使用PWM调光而非简单的开关控制
 * 
 * @code
 * // 示例：省电模式实现
 * void enter_power_save_mode(void)
 * {
 *     LCD_Set_Backlight(false);  // 关闭背光
 *     // 其他省电操作...
 * }
 * 
 * void exit_power_save_mode(void)
 * {
 *     // 恢复其他功能...
 *     LCD_Set_Backlight(true);   // 开启背光
 * }
 * 
 * // 示例：闪烁报警（500ms间隔）
 * void blink_alarm(uint8_t times)
 * {
 *     for(int i = 0; i < times; i++) {
 *         LCD_Set_Backlight(false);
 *         vTaskDelay(pdMS_TO_TICKS(250));
 *         LCD_Set_Backlight(true);
 *         vTaskDelay(pdMS_TO_TICKS(250));
 *     }
 * }
 * @endcode
 */
void LCD_Set_Backlight(bool state)
{
    gpio_set_level((gpio_num_t)LCD_TOUCH_SPI_LED, state ? 1 : 0);
}

/**
 * @brief 实现LCD_TOUCH_Init()函数
 * 
 * 完整的ILI9341初始化序列，严格按照数据手册时序要求：
 * 
 * 初始化流程：
 * 1. GPIO和SPI硬件初始化
 * 2. 硬件复位（RST引脚脉冲）
 * 3. 软件复位（命令0x01）+ 延时5ms
 * 4. 退出睡眠模式（命令0x11）+ 延时120ms
 * 5. 配置内存访问控制（MADCTL）- 设置MX和BGR位
 * 6. 设置像素格式为16位RGB565（命令0x3A）
 * 7. 配置帧率控制（FRMCTR1）
 * 8. 配置电源控制参数（PWCTRL1/2）
 * 9. 配置电压控制（VMCTRL1/2）
 * 10. 配置显示功能（DFUNCTR）
 * 11. 设置正负伽马校正曲线（PGAMCTRL/NGAMCTRL）
 * 12. 配置接口控制（IFCTL）
 * 13. 开启背光
 * 14. 开启显示（命令0x29）
 * 
 * @note 总耗时约250-300ms（主要是延时等待）
 * @note 所有配置参数均参考ILI9341数据手册推荐值
 * @see ILI9341数据手册第8章（命令列表）
 * @see datasheet/LCD屏幕方向调试指南.md（MADCTL配置说明）
 */
void LCD_TOUCH_Init(i2c_master_bus_handle_t i2cBusHandle)
{
    LCD_GPIO_Init();
    LCD_SPI_Init();
    LCD_Hardware_Reset();

    // 软件复位：初始化所有寄存器，恢复默认值
    LCD_Write_Cmd(ILI9341_SWRESET);
    vTaskDelay(pdMS_TO_TICKS(5));  // 等待5ms确保复位完成

    // 退出睡眠模式：唤醒显示屏，启动内部振荡器和DC/DC转换器
    LCD_Write_Cmd(ILI9341_SLPOUT);
    vTaskDelay(pdMS_TO_TICKS(120));  // 必须等待120ms让电源稳定

    // 内存访问控制：设置像素扫描方向和颜色格式
    // MV=1: X/Y轴交换（横屏模式）
    // BGR=1: 红蓝颜色通道交换（硬件要求）
    LCD_Write_Cmd(ILI9341_MADCTL);
    LCD_Write_Data(MADCTL_MV | MADCTL_BGR | MADCTL_MX | MADCTL_MY);

    // 像素格式设置：配置为16位RGB565模式（每个像素2字节）
    LCD_Write_Cmd(ILI9341_PIXFMT);
    LCD_Write_Data(PIXEL_FORMAT_16BIT);  // 0x55 = 16-bit/pixel

    // 帧率控制：设置刷新频率参数（影响显示流畅度）
    LCD_Write_Cmd(ILI9341_FRMCTR1);
    LCD_Write_Data(0x00);  // 分频系数
    LCD_Write_Data(0x18);  // 刷新周期

    // 关闭显示：在配置其他参数前先关闭显示输出（避免闪烁）
    LCD_Write_Cmd(ILI9341_DISPOFF);
    vTaskDelay(pdMS_TO_TICKS(10));  // 等待10ms

    // 电源控制1：配置AVDD（模拟电源）和VGH/VGL（栅极驱动电压）
    LCD_Write_Cmd(ILI9341_PWCTRL1);
    LCD_Write_Data(0x23);  // AVDD电压设置（典型值）

    // 电源控制2：配置VCI（逻辑电路电源）电压
    LCD_Write_Cmd(ILI9341_PWCTRL2);
    LCD_Write_Data(0x10);  // VCI电压设置（典型值）

    // 电压控制1：设置VCOM（公共电极）电压参数
    // VCOM电压影响对比度和显示质量
    LCD_Write_Cmd(ILI9341_VMCTRL1);
    LCD_Write_Data(0x3E);  // VCOMH电压（高电平）
    LCD_Write_Data(0x28);  // VCOML电压（低电平）

    // 电压控制2：设置VCOM偏置和源驱动电压
    LCD_Write_Cmd(ILI9341_VMCTRL2);
    LCD_Write_Data(0x86);  // VCOM偏置 + 源驱动电压

    // 显示功能控制：配置扫描方向、源/栅极驱动模式、极性反转等
    LCD_Write_Cmd(ILI9341_DFUNCTR);
    LCD_Write_Data(0x08);  // 源驱动输出方向
    LCD_Write_Data(0x82);  // 栅极驱动波形
    LCD_Write_Data(0x27);  // 极性反转频率

    // 正伽马校正：调整颜色曲线使显示更自然（15个参数）
    // 影响亮度和色彩还原度
    uint8_t pGammaArr[15] = {0x00, 0x0C, 0x11, 0x0D, 0x0E, 0x15, 0x11, 0x28,
                             0x3F, 0x42, 0x07, 0x12, 0x16, 0x18, 0x08};
    LCD_Write_Cmd(ILI9341_PGAMCTRL);
    for (uint8_t idx = 0; idx < 15; idx++)
    {
        LCD_Write_Data(pGammaArr[idx]);
    }

    // 负伽马校正：调整暗部颜色曲线（15个参数）
    // 与正伽马配合实现完整的颜色校正
    uint8_t nGammaArr[15] = {0x00, 0x0C, 0x11, 0x0D, 0x0E, 0x06, 0x11, 0x28,
                             0x3F, 0x44, 0x07, 0x12, 0x16, 0x19, 0x08};
    LCD_Write_Cmd(ILI9341_NGAMCTRL);
    for (uint8_t idx = 0; idx < 15; idx++)
    {
        LCD_Write_Data(nGammaArr[idx]);
    }

    // 显示反转控制：设置帧反转方式（减少闪烁和残影）
    LCD_Write_Cmd(ILI9341_INVCTR);
    LCD_Write_Data(0x07);  // 1-dot inversion（逐点反转，推荐值）

    // 接口控制：配置接口类型、数据格式、时序参数
    LCD_Write_Cmd(ILI9341_IFCTL);
    LCD_Write_Data(0x01);  // 接口模式（16位RGB/I80）
    LCD_Write_Data(0x00);  // 时序参数

    // 开启背光：点亮LED背光照明
    LCD_Set_Backlight(true);

    // 显示反转：启用数据取反（根据测试，硬件需要此模式）
    // 0x0000(黑)→显示白色, 0xFFFF(白)→显示黑色, 所有颜色按位取反
    LCD_Write_Cmd(ILI9341_INVON);  // 0x21: 显示数据反转
    vTaskDelay(pdMS_TO_TICKS(10));

    // 开启显示：激活屏幕输出，开始正常显示内容
    LCD_Write_Cmd(ILI9341_DISPON);
    vTaskDelay(pdMS_TO_TICKS(10));  // 等待10ms确保显示稳定

    LCD_TOUCH_LOGI("LCD initialized successfully");

    LCD_TOUCH_FT6336G_Init(i2cBusHandle);
}


/**
 * @brief 获取LCD_TOUCH_FT6336G触摸屏的触摸点数据
 * @param touchPoint 指向Touch_Point_t结构体的指针，用于存储触摸点数据
 * @return true 如果有触摸点数据，否则返回false
 * 
 * 该函数从LCD_TOUCH_FT6336G触摸屏读取触摸点数据，
 * 并将数据存储到touchPoint结构体中。
 */
bool LCD_TOUCH_FT6336G_Get_Touch_Points(Touch_Point_t *touchPoint)
{
    if(touchPoint == NULL || Ft6336I2cHandle == NULL)
    {
        return false;
    }

    *touchPoint = LastTouchData;

    return LastTouchData.touchCount > 0;
}


/**
 * @brief 获取LCD_TOUCH_FT6336G触摸屏的触摸处理任务句柄
 * @return TaskHandle_t 指向触摸处理任务的句柄
 */
TaskHandle_t LCD_TOUCH_FT6336G_Get_Task_Handle(void)
{
    return TouchTaskHandle;
}

#if LCD_TOUCH_SELF_TEST    /* 无 LVGL 时的点亮/触摸自测；接入 LVGL 后改 0 → 整段不编译 */
/* ======================== LCD + 触摸屏测试（不用 LVGL） =========================
 * 显示一行字并轮询触摸：按下时屏幕显示坐标 + 画色块，松开恢复。
 * 注意：LCD 的 SPI 与 SD 卡共用总线，且触摸 I2C 的 SDA 与 SD 卡 MISO
 * 都在 GPIO21，本测试与 SD 卡测试不能同时跑。
 */
static const uint8_t Lcd_Test_Font5x7[95][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, /* ' ' 0x20 */
    {0x00, 0x00, 0x5F, 0x00, 0x00}, /* ! */
    {0x00, 0x07, 0x00, 0x07, 0x00}, /* " */
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, /* # */
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, /* $ */
    {0x23, 0x13, 0x08, 0x64, 0x62}, /* % */
    {0x36, 0x49, 0x55, 0x22, 0x50}, /* & */
    {0x00, 0x05, 0x03, 0x00, 0x00}, /* ' */
    {0x00, 0x1C, 0x22, 0x41, 0x00}, /* ( */
    {0x00, 0x41, 0x22, 0x1C, 0x00}, /* ) */
    {0x14, 0x08, 0x3E, 0x08, 0x14}, /* * */
    {0x08, 0x08, 0x3E, 0x08, 0x08}, /* + */
    {0x00, 0x50, 0x30, 0x00, 0x00}, /* , */
    {0x08, 0x08, 0x08, 0x08, 0x08}, /* - */
    {0x00, 0x60, 0x60, 0x00, 0x00}, /* . */
    {0x20, 0x10, 0x08, 0x04, 0x02}, /* / */
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, /* 0 */
    {0x00, 0x42, 0x7F, 0x40, 0x00}, /* 1 */
    {0x42, 0x61, 0x51, 0x49, 0x46}, /* 2 */
    {0x21, 0x41, 0x45, 0x4B, 0x31}, /* 3 */
    {0x18, 0x14, 0x12, 0x7F, 0x10}, /* 4 */
    {0x27, 0x45, 0x45, 0x45, 0x39}, /* 5 */
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, /* 6 */
    {0x01, 0x71, 0x09, 0x05, 0x03}, /* 7 */
    {0x36, 0x49, 0x49, 0x49, 0x36}, /* 8 */
    {0x06, 0x49, 0x49, 0x29, 0x1E}, /* 9 */
    {0x00, 0x36, 0x36, 0x00, 0x00}, /* : */
    {0x00, 0x56, 0x36, 0x00, 0x00}, /* ; */
    {0x00, 0x08, 0x14, 0x22, 0x41}, /* < */
    {0x14, 0x14, 0x14, 0x14, 0x14}, /* = */
    {0x41, 0x22, 0x14, 0x08, 0x00}, /* > */
    {0x02, 0x01, 0x51, 0x09, 0x06}, /* ? */
    {0x32, 0x49, 0x79, 0x41, 0x3E}, /* @ */
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, /* A */
    {0x7F, 0x49, 0x49, 0x49, 0x36}, /* B */
    {0x3E, 0x41, 0x41, 0x41, 0x22}, /* C */
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, /* D */
    {0x7F, 0x49, 0x49, 0x49, 0x41}, /* E */
    {0x7F, 0x09, 0x09, 0x01, 0x01}, /* F */
    {0x3E, 0x41, 0x41, 0x51, 0x32}, /* G */
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, /* H */
    {0x00, 0x41, 0x7F, 0x41, 0x00}, /* I */
    {0x20, 0x40, 0x41, 0x3F, 0x01}, /* J */
    {0x7F, 0x08, 0x14, 0x22, 0x41}, /* K */
    {0x7F, 0x40, 0x40, 0x40, 0x40}, /* L */
    {0x7F, 0x02, 0x04, 0x02, 0x7F}, /* M */
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, /* N */
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, /* O */
    {0x7F, 0x09, 0x09, 0x09, 0x06}, /* P */
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, /* Q */
    {0x7F, 0x09, 0x19, 0x29, 0x46}, /* R */
    {0x26, 0x49, 0x49, 0x49, 0x32}, /* S */
    {0x01, 0x01, 0x7F, 0x01, 0x01}, /* T */
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, /* U */
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, /* V */
    {0x7F, 0x20, 0x18, 0x20, 0x7F}, /* W */
    {0x63, 0x14, 0x08, 0x14, 0x63}, /* X */
    {0x03, 0x04, 0x78, 0x04, 0x03}, /* Y */
    {0x61, 0x51, 0x49, 0x45, 0x43}, /* Z */
    {0x00, 0x00, 0x7F, 0x41, 0x41}, /* [ */
    {0x02, 0x04, 0x08, 0x10, 0x20}, /* \ */
    {0x41, 0x41, 0x7F, 0x00, 0x00}, /* ] */
    {0x04, 0x02, 0x01, 0x02, 0x04}, /* ^ */
    {0x40, 0x40, 0x40, 0x40, 0x40}, /* _ */
    {0x00, 0x01, 0x02, 0x04, 0x00}, /* ` */
    {0x20, 0x54, 0x54, 0x54, 0x78}, /* a */
    {0x7F, 0x48, 0x44, 0x44, 0x38}, /* b */
    {0x38, 0x44, 0x44, 0x44, 0x20}, /* c */
    {0x38, 0x44, 0x44, 0x48, 0x7F}, /* d */
    {0x38, 0x54, 0x54, 0x54, 0x18}, /* e */
    {0x08, 0x7E, 0x09, 0x01, 0x02}, /* f */
    {0x08, 0x14, 0x54, 0x54, 0x3C}, /* g */
    {0x7F, 0x08, 0x04, 0x04, 0x78}, /* h */
    {0x00, 0x44, 0x7D, 0x40, 0x00}, /* i */
    {0x20, 0x40, 0x44, 0x3D, 0x00}, /* j */
    {0x00, 0x7F, 0x10, 0x28, 0x44}, /* k */
    {0x00, 0x41, 0x7F, 0x40, 0x00}, /* l */
    {0x7C, 0x04, 0x18, 0x04, 0x78}, /* m */
    {0x7C, 0x08, 0x04, 0x04, 0x78}, /* n */
    {0x38, 0x44, 0x44, 0x44, 0x38}, /* o */
    {0x7C, 0x14, 0x14, 0x14, 0x08}, /* p */
    {0x08, 0x14, 0x14, 0x18, 0x7C}, /* q */
    {0x7C, 0x08, 0x04, 0x04, 0x08}, /* r */
    {0x48, 0x54, 0x54, 0x54, 0x20}, /* s */
    {0x04, 0x3F, 0x44, 0x40, 0x20}, /* t */
    {0x3C, 0x40, 0x40, 0x20, 0x7C}, /* u */
    {0x1C, 0x20, 0x40, 0x20, 0x1C}, /* v */
    {0x3C, 0x40, 0x30, 0x40, 0x3C}, /* w */
    {0x44, 0x28, 0x10, 0x28, 0x44}, /* x */
    {0x0C, 0x50, 0x50, 0x50, 0x3C}, /* y */
    {0x44, 0x64, 0x54, 0x4C, 0x44}, /* z */
    {0x00, 0x08, 0x36, 0x41, 0x00}, /* { */
    {0x00, 0x00, 0x7F, 0x00, 0x00}, /* | */
    {0x00, 0x41, 0x36, 0x08, 0x00}, /* } */
    {0x08, 0x08, 0x2A, 0x1C, 0x08}, /* ~ */
};

/** 在指定位置画一个字符（含背景色，便于整行刷新） */
static void Lcd_Test_Draw_Char(uint16_t x, uint16_t y, char ch,
                               uint16_t fg, uint16_t bg, uint8_t scale)
{
    static uint16_t cellBuf[5 * 4 * 7 * 4];

    if (ch < 0x20 || ch > 0x7E)
    {
        ch = ' ';
    }
    if (scale < 1 || scale > 4)
    {
        scale = 1;
    }

    const uint8_t *glyph = Lcd_Test_Font5x7[ch - 0x20];
    uint32_t n = 0;
    for (uint8_t row = 0; row < 7; row++)
    {
        for (uint8_t sy = 0; sy < scale; sy++)
        {
            for (uint8_t col = 0; col < 5; col++)
            {
                uint16_t color = (glyph[col] & (1 << row)) ? fg : bg;
                for (uint8_t sx = 0; sx < scale; sx++)
                {
                    cellBuf[n++] = color;
                }
            }
        }
    }

    LCD_Set_Window(x, y, x + 5 * scale - 1, y + 7 * scale - 1);
    LCD_Write_PixelData(cellBuf, n);
}

/** 画一行 ASCII 字符串 */
static void Lcd_Test_Draw_String(uint16_t x, uint16_t y, const char *str,
                                 uint16_t fg, uint16_t bg, uint8_t scale)
{
    while (*str)
    {
        Lcd_Test_Draw_Char(x, y, *str++, fg, bg, scale);
        x += 6 * scale;
    }
}

/** 在触摸点附近画一个 8x8 色块（坐标越界自动收敛到屏内） */
static void Lcd_Test_Draw_Touch_Block(uint16_t x, uint16_t y, uint16_t color)
{
    static uint16_t block[8 * 8];

    if (x > LCD_WIDTH - 8)
    {
        x = (x < 4) ? 0 : (LCD_WIDTH - 8);
    }
    if (y > LCD_HEIGHT - 8)
    {
        y = (y < 4) ? 0 : (LCD_HEIGHT - 8);
    }

    for (uint32_t i = 0; i < sizeof(block) / sizeof(block[0]); i++)
    {
        block[i] = color;
    }
    LCD_Set_Window(x, y, x + 7, y + 7);
    LCD_Write_PixelData(block, sizeof(block) / sizeof(block[0]));
}

/**
 * @brief LCD 显示 + 触摸检测测试（不依赖 LVGL）
 *
 * 流程：I2C 初始化 -> LCD/触摸初始化 -> 清屏画字 -> 轮询 INT 引脚，
 * 按下时显示坐标并画色块，松开时恢复提示文字，串口同步打印状态。
 */
void Lcd_Touch_Test(void)
{
    LCD_TOUCH_LOGI("=== LCD + Touch Test Start ===");

    i2c_master_bus_handle_t i2cBus = NULL;
    esp_err_t err = I2c_Init_Bus(I2C_PORT, (gpio_num_t)I2C_SDA_GPIO,
                                 (gpio_num_t)I2C_SCL_GPIO, I2C_FREQ, &i2cBus);
    if (err != ESP_OK || i2cBus == NULL)
    {
        LCD_TOUCH_LOGE("I2C bus init failed: %s", esp_err_to_name(err));
        return;
    }

    LCD_TOUCH_Init(i2cBus);

    const uint16_t colorBg   = RGB565(0, 0, 0);
    const uint16_t colorText = RGB565(255, 255, 255);
    const uint16_t colorOk   = RGB565(0, 255, 0);
    const uint16_t colorMark = RGB565(255, 0, 0);

    LCD_Fill_Color(colorBg);
    Lcd_Test_Draw_String(20, 40, "LCD TEST OK", colorText, colorBg, 3);
    Lcd_Test_Draw_String(20, 120, "TOUCH: --,--", colorOk, colorBg, 2);

    bool lastPressed = false;
    char line[32];
    while (1)
    {
        /* 触摸任务每 20ms 轮询 FT6336G：按下 touchCount>0，松开自动清零 */
        Touch_Point_t tp = {0};
        bool pressed = LCD_TOUCH_FT6336G_Get_Touch_Points(&tp);

        if (pressed != lastPressed)
        {
            if (pressed)
            {
                LCD_TOUCH_LOGI("Touch pressed: x=%u y=%u",
                         (unsigned)tp.touchX, (unsigned)tp.touchY);
                snprintf(line, sizeof(line), "TOUCH:%3u,%3u",
                         (unsigned)tp.touchX, (unsigned)tp.touchY);
                Lcd_Test_Draw_Touch_Block(tp.touchX, tp.touchY, colorMark);
            }
            else
            {
                LCD_TOUCH_LOGI("Touch released");
                snprintf(line, sizeof(line), "TOUCH: --,--");
            }
            Lcd_Test_Draw_String(20, 120, line, colorOk, colorBg, 2);
            lastPressed = pressed;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

#endif /* LCD_TOUCH_SELF_TEST */
