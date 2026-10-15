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
    # bsp：底层通信；其余为 wifi_manager（STA 连接 + UDP 组播通道）与
    # node_role（NVS 持久化）所需
    # esp_ringbuf：wifi_manager 的组播接收 FIFO 用的是 xRingbuffer（IDF 6 里
    #              它已从 freertos 拆成独立组件）
    REQUIRES bsp nvs_flash esp_wifi esp_netif esp_event lwip esp_timer esp_ringbuf
)