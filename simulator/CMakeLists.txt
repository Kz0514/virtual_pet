cmake_minimum_required(VERSION 3.16)
project(LVGL_Simulator C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ═══════════════════════════════════════════════════════════════════════════
# 路径
# ═══════════════════════════════════════════════════════════════════════════
set(LVGL_ROOT  ${CMAKE_CURRENT_SOURCE_DIR}/../managed_components/lvgl__lvgl)
set(UI_ROOT    ${CMAKE_CURRENT_SOURCE_DIR}/../main)
set(MOCK_ROOT  ${CMAKE_CURRENT_SOURCE_DIR}/mock)

# ═══════════════════════════════════════════════════════════════════════════
# 查找 SDL2
# ═══════════════════════════════════════════════════════════════════════════
find_package(SDL2 REQUIRED)

# ═══════════════════════════════════════════════════════════════════════════
# LVGL v9.5 源文件 — 递归收集全部 .c (未启用的组件由 lv_conf 宏守卫)
# ═══════════════════════════════════════════════════════════════════════════
file(GLOB_RECURSE LVGL_SRCS
    ${LVGL_ROOT}/src/*.c
)

# ═══════════════════════════════════════════════════════════════════════════
# 项目 UI 源文件 (引用原文件, 不拷贝)
# ═══════════════════════════════════════════════════════════════════════════
set(UI_SRCS
    # Screens
    ${UI_ROOT}/ui/screens/loading_screen.c
    ${UI_ROOT}/ui/screens/home_screen.c
    ${UI_ROOT}/ui/screens/boot_screen.c
    ${UI_ROOT}/ui/screens/settings_screen.c
    ${UI_ROOT}/ui/screens/diary_screen.c
    ${UI_ROOT}/ui/screens/ota_screen.c
    # Widgets
    ${UI_ROOT}/ui/widgets/pet_avatar.c
    ${UI_ROOT}/ui/widgets/status_bar.c
    ${UI_ROOT}/ui/widgets/chat_bubble.c
    ${UI_ROOT}/ui/widgets/notify_overlay.c
    ${UI_ROOT}/ui/widgets/brightness_bar.c
    ${UI_ROOT}/ui/widgets/weather_overlay.c
    # Fonts
    ${UI_ROOT}/ui/fonts/font_loader.c
)

# ═══════════════════════════════════════════════════════════════════════════
# 模拟器专属源文件
# ═══════════════════════════════════════════════════════════════════════════
set(SIM_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/sim_spiffs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mock_impl.c
)

# ═══════════════════════════════════════════════════════════════════════════
# 可执行文件
# ═══════════════════════════════════════════════════════════════════════════
add_executable(lvgl_simulator
    ${SIM_SRCS}
    ${UI_SRCS}
    ${LVGL_SRCS}
)

# ═══════════════════════════════════════════════════════════════════════════
# Include 目录 (顺序很重要: mock 优先于真实 ESP-IDF 头文件)
# ═══════════════════════════════════════════════════════════════════════════
target_include_directories(lvgl_simulator PRIVATE
    # 模拟器目录 (lv_conf.h)
    ${CMAKE_CURRENT_SOURCE_DIR}
    # Mock 头文件 (覆盖 esp_log, esp_err, FreeRTOS 等)
    ${MOCK_ROOT}
    ${MOCK_ROOT}/freertos
    # LVGL
    ${LVGL_ROOT}
    ${LVGL_ROOT}/src
    # 项目 UI 代码
    ${UI_ROOT}
    ${UI_ROOT}/ui/fonts
    ${UI_ROOT}/ui/screens
    ${UI_ROOT}/ui/widgets
    # SDL2
    ${SDL2_INCLUDE_DIRS}
)

# ═══════════════════════════════════════════════════════════════════════════
# 链接
# ═══════════════════════════════════════════════════════════════════════════
target_link_libraries(lvgl_simulator PRIVATE
    ${SDL2_LIBRARIES}
)

# Unix/MinGW: link math library; MSVC has it built-in
if(NOT MSVC)
    target_link_libraries(lvgl_simulator PRIVATE m)
endif()

# ═══════════════════════════════════════════════════════════════════════════
# fopen 包装: 将 /spiffs/ 路径重定向到 spiffs/ 目录
# ═══════════════════════════════════════════════════════════════════════════
target_link_options(lvgl_simulator PRIVATE
    -Wl,--wrap=fopen
)

# ═══════════════════════════════════════════════════════════════════════════
# 编译定义
# ═══════════════════════════════════════════════════════════════════════════
target_compile_definitions(lvgl_simulator PRIVATE
    LV_CONF_INCLUDE_SIMPLE
)

# ═══════════════════════════════════════════════════════════════════════════
# 编译选项 (降低警告噪音)
# ═══════════════════════════════════════════════════════════════════════════
target_compile_options(lvgl_simulator PRIVATE
    -Wall -Wextra
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-missing-field-initializers
    -Wno-sign-compare
    -Wno-format
    -Wno-int-to-pointer-cast
    -Wno-pointer-to-int-cast
)

if(MSVC)
    target_compile_options(lvgl_simulator PRIVATE /W3 /wd4100 /wd4189 /wd4244 /wd4018)
endif()

# ═══════════════════════════════════════════════════════════════════════════
# 状态输出
# ═══════════════════════════════════════════════════════════════════════════
message(STATUS "========================================")
message(STATUS "LVGL Simulator Configuration")
message(STATUS "----------------------------------------")
message(STATUS "SDL2 include : ${SDL2_INCLUDE_DIRS}")
message(STATUS "SDL2 libs    : ${SDL2_LIBRARIES}")
message(STATUS "LVGL root    : ${LVGL_ROOT}")
message(STATUS "UI root      : ${UI_ROOT}")
message(STATUS "LVGL sources : ${LVGL_SRCS}")
list(LENGTH LVGL_SRCS LVGL_SRC_COUNT)
message(STATUS "LVGL count   : ${LVGL_SRC_COUNT} files")
message(STATUS "========================================")
