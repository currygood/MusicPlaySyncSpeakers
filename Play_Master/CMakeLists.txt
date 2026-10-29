# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

# 本机 ccache 与 ninja 组合存在 spawn 问题，默认关闭；
# 如需启用 ccache，删除下面这一行即可。
set(CCACHE_ENABLE OFF CACHE BOOL "Enable ccache for faster rebuilds" FORCE)

set(VERSION 0.1)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
project(Play_Master)

# ---------------------------------------------------------------------------
# 诊断：-fstack-usage（给每个函数输出 .su 帧大小文件）—— **默认关闭**
#
# 什么时候需要它：怀疑某个任务的栈用量异常。它会为每个 .c 产出一个 .su，
# 里面写着每个函数的栈帧大小，能直接定位"哪个函数吃了几 KB"，不用靠猜。
# 教训：UI_Task 那个 23852 B 的渲染峰值，靠 hwm 日志和 SP 探针都只能看到"很深"，
# 是 .su + 排除法才确认"整条 LVGL/FreeType 绘制链加起来不到 1.5KB"，
# 从而断定那 23.4KB 不是单个大帧而是多层调用叠加。
#
# 打开：idf.py -DUI_DIAG_STACK_USAGE=ON build  （会全量重编，≈1800 个 .su）
# 注意键名不能再改回 UI_STACK_USAGE：那个键在上一次构建里已被缓存成 ON，
# CMakeCache 会覆盖这里的默认值，改默认值等于没改（同 UI_LVGL_OPT 那个坑）。
# ---------------------------------------------------------------------------
set(UI_DIAG_STACK_USAGE OFF CACHE BOOL "生成 .su 栈用量文件（诊断用，不改代码生成）")
if(UI_DIAG_STACK_USAGE)
    # 注意：不能用 add_compile_options()——它的作用域是"之后创建的目录/目标"，
    # 而 IDF 的组件目标是在 project() 内部就建好的，写在 project() 之后等于空转
    # （第一次尝试就是这么失败的：一个 .su 都没有）。这里直接遍历组件目标追加。
    idf_build_get_property(_ui_su_comps BUILD_COMPONENTS)
    set(_ui_su_cnt 0)
    foreach(_ui_su_comp ${_ui_su_comps})
        idf_component_get_property(_ui_su_lib ${_ui_su_comp} COMPONENT_LIB)
        if(TARGET ${_ui_su_lib})
            set_property(TARGET ${_ui_su_lib} APPEND PROPERTY COMPILE_OPTIONS -fstack-usage)
            math(EXPR _ui_su_cnt "${_ui_su_cnt} + 1")
        endif()
    endforeach()
    message(STATUS "diag: -fstack-usage ON -> ${_ui_su_cnt} 个组件，产物在各 CMakeFiles/*.su")

    # 例外：espressif/freetype 组件本身只是个 `idf_component_register()` + INTERFACE 包装，
    # 真正的库是它在 freetype/ 子目录里 add_library 出来的 `freetype` 目标，
    # 所以上面的组件遍历打不到它（实测它的 42 个 .obj 编译命令里没有该 flag）。
    if(TARGET freetype)
        set_property(TARGET freetype APPEND PROPERTY COMPILE_OPTIONS -fstack-usage)
        message(STATUS "diag: -fstack-usage 也追加到 freetype 目标")
    endif()
endif()

# ---------------------------------------------------------------------------
# LVGL / FreeType 的优化级别（键名 UI_UI_DRAW_OPT：旧键 UI_LVGL_OPT 已在 CMakeCache 里缓存成 Og，
# 改默认值不会生效，所以换了新键，避免"改了默认值却还是 -Og"这种坑）
#
# 实测（320x240、25 个对象、8 行渲染窗口 = 30 段一次整屏）：
#   切一页 571ms ≈ flush 133ms + draw 439ms，**软件绘制是大头**，所以优化目标是 draw。
#   全局 -Og 下每像素约 440 个周期，对一个纯 RGB565 填充循环来说明显偏慢，
#   主因就是 -Og 不内联（lv_color_mix / 混合内层循环都变成真实函数调用）。
#
# 现状：默认 -Os。-Os 相对 -Og 会做内联与更紧凑的代码生成，通常能拿到 2~4 倍，
#       且 IRA 的输入与 -O2 不同，触发下面那个 ICE 的概率低得多。
#
# ⚠ 已知坑：本机 GCC 15.2 用 -O2 编 lvgl 的 src/core/lv_obj_style.c 时，
#   lv_obj_style_state_compare() 在 RTL pass "ira" 段错误（ICE，编译器 bug）。
#   想再往上试更快：
#     idf.py -DUI_UI_DRAW_OPT=O2 build      <- 会附带 -fjump-tables，试图绕开那个大 switch
#   炸了就 -DUI_UI_DRAW_OPT=Os 或 =Og 回到可用状态，不用改代码。
# ---------------------------------------------------------------------------
set(UI_UI_DRAW_OPT "Os" CACHE STRING "LVGL/FreeType 优化级别：Os(默认) / Og / O2")

if(NOT UI_UI_DRAW_OPT STREQUAL "Og")
    idf_build_get_property(_ui_all_comps BUILD_COMPONENTS)
    # -O2 时顺带打开 jump table：IDF 默认带 -fno-jump-tables -fno-tree-switch-conversion，
    # 会把大 switch 展开成巨型比较链，正是 IRA 崩掉的诱因；后写的 -fjump-tables 覆盖它。
    set(_ui_opt_extra "")
    if(UI_UI_DRAW_OPT STREQUAL "O2")
        set(_ui_opt_extra -fjump-tables)
    endif()
    foreach(_ui_opt_comp lvgl__lvgl espressif__freetype)
        set(_ui_opt_lib "")
        if(TARGET ${_ui_opt_comp})
            set(_ui_opt_lib ${_ui_opt_comp})
        elseif(_ui_opt_comp IN_LIST _ui_all_comps)
            idf_component_get_property(_ui_opt_lib2 ${_ui_opt_comp} COMPONENT_LIB)
            if(TARGET ${_ui_opt_lib2})
                set(_ui_opt_lib ${_ui_opt_lib2})
            endif()
        endif()
        if(_ui_opt_lib)
            set_property(TARGET ${_ui_opt_lib} APPEND PROPERTY COMPILE_OPTIONS
                         -${UI_UI_DRAW_OPT} ${_ui_opt_extra})
            message(STATUS "opt: ${_ui_opt_comp} (${_ui_opt_lib}) -> -${UI_UI_DRAW_OPT} ${_ui_opt_extra}")
        else()
            message(STATUS "opt: ${_ui_opt_comp} 未找到编译目标，跳过（不影响构建）")
        endif()
    endforeach()

    # 同上：freetype 的真库是子目录里的 `freetype` 目标，包装目标加 flag 是空转
    if(TARGET freetype)
        set_property(TARGET freetype APPEND PROPERTY COMPILE_OPTIONS
                     -${UI_UI_DRAW_OPT} ${_ui_opt_extra})
        message(STATUS "opt: freetype (真库目标) -> -${UI_UI_DRAW_OPT} ${_ui_opt_extra}")
    endif()
else()
    message(STATUS "opt: LVGL/FreeType 沿用全局 -Og")
endif()


file(WRITE "${CMAKE_SOURCE_DIR}/components/App/UI/fw_version.h"
     "#ifndef __FW_VERSION_H__\n#define __FW_VERSION_H__\n#define FW_VERSION \"${VERSION}\"\n#endif\n")