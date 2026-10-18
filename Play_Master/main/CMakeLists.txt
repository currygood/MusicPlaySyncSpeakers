idf_component_register(SRCS "app_main.c"
                       PRIV_REQUIRES spi_flash esp_psram esp_hw_support
                       INCLUDE_DIRS ""
					   REQUIRES driver bsp middlewares msystem App espcoredump)


spiffs_create_partition_image(storage ../storage FLASH_IN_PROJECT)
