idf_component_register(SRCS "app_main.c"
                       PRIV_REQUIRES spi_flash esp_psram fatfs sdmmc esp_driver_sdspi esp_driver_spi
                       INCLUDE_DIRS ""
                       REQUIRES App mysystem)