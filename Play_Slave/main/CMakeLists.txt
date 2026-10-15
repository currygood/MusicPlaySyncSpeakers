idf_component_register(SRCS "app_main.c" "debug.c"
                       PRIV_REQUIRES spi_flash esp_psram fatfs sdmmc esp_driver_sdspi esp_driver_spi nvs_flash
                       INCLUDE_DIRS ""
                       REQUIRES App mysystem middlewares)