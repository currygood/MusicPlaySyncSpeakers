#ifndef __LCD_TOUCH_H__
#define __LCD_TOUCH_H__

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "spi_driver.h"
#include "driver/gpio.h"
#include "i2c_driver.h"


// 模块的引脚定义
#define LCD_TOUCH_SPI_CS 					1
#define LCD_TOUCH_SPI_RST 					40
#define LCD_TOUCH_SPI_RS					42
#define LCD_TOUCH_SPI_LED					41
#define LCD_TOUCH_FT6336_INT				48
#define LCD_TOUCH_FT6336_RST				47
#define LCD_TOUCH_SD_CS						45


#define LCD_WIDTH				240
#define LCD_HEIGHT				320


#define LCD_TOUCH_FT6336G_DEV_ADDR                0x38		// FT6636的I2C地址
// ==================== LCD_TOUCH_FT6336G 寄存器地址定义 ====================
#define LCD_TOUCH_FT6336G_REG_DEV_MODE             0x00
#define LCD_TOUCH_FT6336G_REG_GEST_ID              0x01
#define LCD_TOUCH_FT6336G_REG_TD_STATUS            0x02
#define LCD_TOUCH_FT6336G_REG_P1_XH                0x03
#define LCD_TOUCH_FT6336G_REG_P1_XL                0x04
#define LCD_TOUCH_FT6336G_REG_P1_YH                0x05
#define LCD_TOUCH_FT6336G_REG_P1_YL                0x06
#define LCD_TOUCH_FT6336G_REG_P1_WEIGHT            0x07
#define LCD_TOUCH_FT6336G_REG_P1_MISC              0x08
#define LCD_TOUCH_FT6336G_REG_P2_XH                0x09
#define LCD_TOUCH_FT6336G_REG_P2_XL                0x0A
#define LCD_TOUCH_FT6336G_REG_P2_YH                0x0B
#define LCD_TOUCH_FT6336G_REG_P2_YL                0x0C
#define LCD_TOUCH_FT6336G_REG_P2_WEIGHT            0x0D
#define LCD_TOUCH_FT6336G_REG_P2_MISC              0x0E
#define LCD_TOUCH_FT6336G_REG_G_MODE               0xA4
#define LCD_TOUCH_FT6336G_REG_PWR_MODE             0xA5
#define LCD_TOUCH_FT6336G_REG_FIRMID               0xA6
#define LCD_TOUCH_FT6336G_REG_FOCALTECH_ID         0xA8
#define LCD_TOUCH_FT6336G_REG_STATE                0xBC

#define ILI9341_NOP					0x00
#define ILI9341_SWRESET				0x01
#define ILI9341_RDDID				0x04
#define ILI9341_RDDST				0x09
#define ILI9341_SLPIN				0x10
#define ILI9341_SLPOUT				0x11
#define ILI9341_PTLON				0x12
#define ILI9341_NORON				0x13
#define ILI9341_RDMODE				0x0A
#define ILI9341_RDMADCTL			0x0B
#define ILI9341_RDPIXFMT			0x0C
#define ILI9341_RDIMGFMT			0x0D
#define ILI9341_RDSELFDIAG			0x0F
#define ILI9341_INVOFF				0x20
#define ILI9341_INVON				0x21
#define ILI9341_GAMSET				0x26
#define ILI9341_DISPOFF				0x28
#define ILI9341_DISPON				0x29
#define ILI9341_CASET				0x2A
#define ILI9341_PASET				0x2B
#define ILI9341_RAMWR				0x2C
#define ILI9341_RAMRD				0x2E
#define ILI9341_PTLAR				0x30
#define ILI9341_VSCRDEF				0x33
#define ILI9341_MADCTL				0x36
#define ILI9341_VSCRSADD			0x37
#define ILI9341_PIXFMT				0x3A
#define ILI9341_FRMCTR1				0xB1
#define ILI9341_FRMCTR2				0xB2
#define ILI9341_FRMCTR3				0xB3
#define ILI9341_INVCTR				0xB4
#define ILI9341_DFUNCTR			0xB6
#define ILI9341_PWCTRL1			0xC0
#define ILI9341_PWCTRL2			0xC1
#define ILI9341_VMCTRL1			0xC5
#define ILI9341_VMCTRL2			0xC7
#define ILI9341_PGAMCTRL			0xE0
#define ILI9341_NGAMCTRL			0xE1
#define ILI9341_IFCTL				0xF6

#define MADCTL_MY					0x80
#define MADCTL_MX					0x40
#define MADCTL_MV					0x20
#define MADCTL_ML					0x10
#define MADCTL_BGR					0x08  // Bit 3: BGR颜色顺序（文档要求）
#define MADCTL_MH					0x04
#define MADCTL_SS					0x02
#define MADCTL_RGB					0x00  // RGB模式（默认）

#define PIXEL_FORMAT_16BIT			0x55

#define RGB565(r, g, b)			(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))

typedef struct {
    uint16_t touchX;
    uint16_t touchY;
    uint8_t touchCount;
} Touch_Point_t;


// ==================== LCD_TOUCH 显示函数定义 ====================

// 初始化LCD显示屏（需要传入已初始化的I2C总线句柄）
void LCD_TOUCH_Init(i2c_master_bus_handle_t i2cBusHandle);

// 发送命令字节
void LCD_Write_Cmd(uint8_t cmd);

// 发送8位参数数据
void LCD_Write_Data(uint8_t data);

// 发送16位像素颜色数据
void LCD_Write_Data_16Bit(uint16_t data);

// 设置显示窗口区域
void LCD_Set_Window(uint16_t xStart, uint16_t yStart, uint16_t xEnd, uint16_t yEnd);

// 全屏填充指定颜色
void LCD_Fill_Color(uint16_t color);

// 批量写入像素数据（LVGL刷新用）
void LCD_Write_PixelData(const uint16_t *data, uint32_t pixelCount);

// 控制背光开关
void LCD_Set_Backlight(bool state);


// ==================== LCD_TOUCH_FT6336G 触摸函数定义 ====================

// 获取触摸点
bool LCD_TOUCH_FT6336G_Get_Touch_Points(Touch_Point_t *touchPoint);

// 获取触摸任务句柄
TaskHandle_t LCD_TOUCH_FT6336G_Get_Task_Handle(void);

#endif