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
    # bsp：底层通信（i2s_driver）；mysystem：wifi_manager 组播通道句柄；
    # esp_timer：同步协议的时间基准（esp_timer_get_time）
    REQUIRES bsp freertos mysystem esp_timer
)