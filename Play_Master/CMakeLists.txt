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