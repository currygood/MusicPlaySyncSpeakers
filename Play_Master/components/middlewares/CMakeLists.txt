# 1. 自动搜索当前目录下所有的子目录路径
file(GLOB children RELATIVE ${COMPONENT_DIR} *)
set(MID_INCLUDE_DIRS "")

foreach(child ${children})
    if(IS_DIRECTORY ${COMPONENT_DIR}/${child})
        # 将所有子文件夹（DHT22, CoolingFan等）加入包含路径
        list(APPEND MID_INCLUDE_DIRS ${child})
    endif()
endforeach()
    
# 2. 搜集所有的 .c 文件
file(GLOB_RECURSE SOURCES "*.c")

# 3. 注册组件
idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS ${MID_INCLUDE_DIRS}
    # Middlewares 的传感器通常需要调用 BSP 层的底层通信逻辑
    # sync_protocol 依赖 msystem（wifi_manager 组播通道）与 esp_timer（时间戳）
    REQUIRES bsp freertos fatfs sdmmc esp_driver_sdspi esp_driver_spi nvs_flash bt msystem esp_timer
)
