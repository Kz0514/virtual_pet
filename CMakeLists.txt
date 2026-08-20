# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# 固件头版本 (esp_app_desc.version) 必须等于 version.txt — OTA 检查用它对比
# 服务端最新版本。之前 project() 不带 VERSION, IDF 默认 PROJECT_VER=1,
# 设备永远报旧版本 → 每次重启都触发 OTA → 无限循环 (1.0.221 实测)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/version.txt" PROJECT_VER_STR)
string(STRIP "${PROJECT_VER_STR}" PROJECT_VER_STR)
project(Virtualpet VERSION ${PROJECT_VER_STR})

# SPIFFS 动画资源镜像 (spiffs/ → assets 分区)
spiffs_create_partition_image(assets spiffs FLASH_IN_PROJECT)
