idf_component_register(SRCS "app_main.c"
                       PRIV_REQUIRES spi_flash esp_psram
                       INCLUDE_DIRS ""
					   REQUIRES driver bsp middlewares msystem App)