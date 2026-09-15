#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_psram.h"

void app_main(void)
{
	if (esp_psram_is_initialized()) {
        printf("PSRAM 初始化成功！可用大小: %d 字节\n", esp_psram_get_size());
    } else {
        printf("PSRAM 未找到或未初始化！\n");
    }

	
	while(1)
	{
		ESP_LOGI("Hello", "Hello world!");
		vTaskDelay(pdMS_TO_TICKS(1000));
	}
}
