# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# 将 ESP32-S31-Korvo 官方 BSP（本仓库 common_components）加入组件搜索路径
set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/../common_components
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(gif_led_demo)
