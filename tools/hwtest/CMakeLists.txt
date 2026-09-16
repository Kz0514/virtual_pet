# 独立整板硬件自检工程 (hwtest) — 不引用主工程 main/ 任何源码
#
# 只借用仓库里 vendored 的 components/esp_littlefs, 其余全用 IDF 自带组件。
# 分区表与主工程同表 (partitions.csv) → app 落在 ota_0@0x20000, 与主工程 app-flash 落点一致。
cmake_minimum_required(VERSION 3.16)

# 必须在 project() 之前: 否则首次配置走 IDF 默认目标 esp32
# (sdkconfig.defaults 里的 CONFIG_IDF_TARGET_ESP32S3 对"选哪个目标"不起作用)
set(IDF_TARGET "esp32s3")

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components/esp_littlefs")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(hwtest)
