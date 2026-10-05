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
    # Middlewares 的传感器通常需要调用 BSP 层的底层通信逻辑；esp-sr 供 CallPhone 唤醒词检测
    # App 层模块（LightControl）依赖 msystem（wifi_manager 组播通道）与 cjson（灯控 JSON 协议）
    REQUIRES middlewares esp-sr msystem cjson
)