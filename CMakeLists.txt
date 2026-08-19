# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(Virtualpet)

# SPIFFS 动画资源镜像 (spiffs/ → assets 分区)
spiffs_create_partition_image(assets spiffs FLASH_IN_PROJECT)
