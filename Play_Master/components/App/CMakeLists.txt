# 1. 自动搜索当前目录下所有的子目录路径
#
# 关于 CONFIGURE_DEPENDS：
#   没有它时 GLOB 结果在 configure 阶段就冻结了，新增 .c/.h 目录不会被感知，
#   表现为 "undefined reference to xxx"，只有手动 idf.py reconfigure 才消失。
#   但 IDF 在"收集组件需求"阶段是用 cmake -P（脚本模式）include 本文件的，
#   脚本模式不支持 CONFIGURE_DEPENDS（会直接报
#   "CONFIGURE_DEPENDS is invalid for script and find package modes"）。
#   所以两种模式要分开写。
if(CMAKE_SCRIPT_MODE_FILE)
    file(GLOB children RELATIVE ${COMPONENT_DIR} *)
else()
    file(GLOB children CONFIGURE_DEPENDS RELATIVE ${COMPONENT_DIR} *)
endif()
set(MID_INCLUDE_DIRS "")

foreach(child ${children})
    if(IS_DIRECTORY ${COMPONENT_DIR}/${child})
        # 将所有子文件夹（DHT22, CoolingFan等）加入包含路径
        list(APPEND MID_INCLUDE_DIRS ${child})
    endif()
endforeach()

# 2. 搜集所有的 .c 文件（同上：脚本模式不支持 CONFIGURE_DEPENDS）
if(CMAKE_SCRIPT_MODE_FILE)
    file(GLOB_RECURSE SOURCES "*.c")
else()
    file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "*.c")
endif()

# 2.1 显式兜底：GLOB 在"已配置过"的 build 目录里可能仍是旧列表（尤其是老工程加新文件），
#     这里把关键新模块显式补进 SRCS，避免再出现 "undefined reference to xxx"。
#     已在 GLOB 结果里时跳过（CMake 对同一 target 内的重复源文件也会去重，双保险）。
list(FIND SOURCES "${COMPONENT_DIR}/UI/ui_font.c" _ui_font_idx)
if(_ui_font_idx EQUAL -1)
    list(APPEND SOURCES "${COMPONENT_DIR}/UI/ui_font.c")
endif()
 
# 3. 注册组件
idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS ${MID_INCLUDE_DIRS}
    # Middlewares 的传感器通常需要调用 BSP 层的底层通信逻辑
    # App 层模块（LightControl）依赖 msystem（wifi_manager 组播通道）与 cjson（灯控 JSON 协议）
    # App/UI 依赖：lvgl（LVGL 9 自持 UI_Task 方案，UI 代码 include "lvgl.h"）
    #             + spiffs（UI 图片烧录 storage 分区，esp_vfs_spiffs_register 挂载）
    # 注意：esp_lvgl_port 本工程未使用，其组件名是 esp_lvgl_port（不是 lvgl_port/esp_lvgl），
    #       不要加进 REQUIRES，否则 CMake 解析报 unknown name
    # 注意：esp-sr 已从构建中移除（语音唤醒/麦克风整条链路下线，见
    #       legacy_disabled/README.md），不要再加回来，否则会重新引入 esp-dl 依赖与
    #       AFE 任务占核（IDLE1 饿死 → A2DP starved → INT WDT）。
    REQUIRES middlewares msystem cjson spiffs lvgl vfs
)