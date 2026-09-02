cmake_minimum_required(VERSION 3.16)
project(LVGL_Simulator C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ═══════════════════════════════════════════════════════════════════════════
# 路径
# ═══════════════════════════════════════════════════════════════════════════
# LVGL 已 vendored 到 components/ (本地组件覆盖托管组件, managed_components 不再有 lvgl)
set(LVGL_ROOT  ${CMAKE_CURRENT_SOURCE_DIR}/../components/lvgl)
set(WEBP_ROOT  ${CMAKE_CURRENT_SOURCE_DIR}/../components/libwebp)
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
    ${UI_ROOT}/ui/common/screen_switch.c
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
# libwebp 解码器 — 与固件 components/libwebp 同源 (diary_screen 涂鸦直解)
# 源清单照抄组件 CMakeLists.txt 的 SRC_DEC/DSP/UTILS, 不 glob 全目录
# (SIMD 文件被 #if WEBP_USE_* 守卫成空 TU, 白编 40+ 个)
# ═══════════════════════════════════════════════════════════════════════════
set(WEBP_SRCS
    ${WEBP_ROOT}/src/dec/alpha_dec.c
    ${WEBP_ROOT}/src/dec/buffer_dec.c
    ${WEBP_ROOT}/src/dec/frame_dec.c
    ${WEBP_ROOT}/src/dec/idec_dec.c
    ${WEBP_ROOT}/src/dec/io_dec.c
    ${WEBP_ROOT}/src/dec/quant_dec.c
    ${WEBP_ROOT}/src/dec/tree_dec.c
    ${WEBP_ROOT}/src/dec/vp8_dec.c
    ${WEBP_ROOT}/src/dec/vp8l_dec.c
    ${WEBP_ROOT}/src/dec/webp_dec.c
    ${WEBP_ROOT}/src/dsp/alpha_processing.c
    ${WEBP_ROOT}/src/dsp/cpu.c
    ${WEBP_ROOT}/src/dsp/dec.c
    ${WEBP_ROOT}/src/dsp/dec_clip_tables.c
    ${WEBP_ROOT}/src/dsp/filters.c
    ${WEBP_ROOT}/src/dsp/lossless.c
    ${WEBP_ROOT}/src/dsp/rescaler.c
    ${WEBP_ROOT}/src/dsp/upsampling.c
    ${WEBP_ROOT}/src/dsp/yuv.c
    # PC (MinGW x86_64) 默认定义 __SSE2__ → cpu.c 运行时检测会调用
    # SSE2 初始化函数; 固件 xtensa 无此问题, 模拟器必须补编这些文件
    # (内部 #if WEBP_USE_SSE2 守卫, 非 x86 平台自动空 TU)
    ${WEBP_ROOT}/src/dsp/alpha_processing_sse2.c
    ${WEBP_ROOT}/src/dsp/dec_sse2.c
    ${WEBP_ROOT}/src/dsp/filters_sse2.c
    ${WEBP_ROOT}/src/dsp/lossless_sse2.c
    ${WEBP_ROOT}/src/dsp/rescaler_sse2.c
    ${WEBP_ROOT}/src/dsp/upsampling_sse2.c
    ${WEBP_ROOT}/src/dsp/yuv_sse2.c
    ${WEBP_ROOT}/src/utils/bit_reader_utils.c
    ${WEBP_ROOT}/src/utils/bit_writer_utils.c
    ${WEBP_ROOT}/src/utils/color_cache_utils.c
    ${WEBP_ROOT}/src/utils/filters_utils.c
    ${WEBP_ROOT}/src/utils/huffman_encode_utils.c
    ${WEBP_ROOT}/src/utils/huffman_utils.c
    ${WEBP_ROOT}/src/utils/palette.c
    ${WEBP_ROOT}/src/utils/quant_levels_dec_utils.c
    ${WEBP_ROOT}/src/utils/quant_levels_utils.c
    ${WEBP_ROOT}/src/utils/random_utils.c
    ${WEBP_ROOT}/src/utils/rescaler_utils.c
    ${WEBP_ROOT}/src/utils/thread_utils.c
    ${WEBP_ROOT}/src/utils/utils.c
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
    ${WEBP_SRCS}
    ${LVGL_SRCS}
)

# ═══════════════════════════════════════════════════════════════════════════
# Include 目录 (顺序很重要: mock 优先于真实 ESP-IDF 头文件)
# ═══════════════════════════════════════════════════════════════════════════
target_include_directories(lvgl_simulator PRIVATE
    # 模拟器目录 (lv_conf.h)
    ${CMAKE_CURRENT_SOURCE_DIR}
    # Mock 头文件 (覆盖 esp_log, esp_err, FreeRTOS 等; board.h 遮蔽真 board.h)
    ${MOCK_ROOT}
    ${MOCK_ROOT}/freertos
    # LVGL
    ${LVGL_ROOT}
    ${LVGL_ROOT}/src
    # libwebp (diary_screen 涂鸦解码, 与固件同源)
    ${WEBP_ROOT}
    ${WEBP_ROOT}/src
    # 项目 UI 代码
    ${UI_ROOT}
    ${UI_ROOT}/ui/fonts
    ${UI_ROOT}/ui/screens
    ${UI_ROOT}/ui/common
    ${UI_ROOT}/ui/widgets
    # UI 代码依赖的固件头 (settings_screen 的 config_mgr/time_manager/
    # usb_storage/ota_client; 均 PC 干净, 无需 mock 副本)
    ${UI_ROOT}/app
    ${UI_ROOT}/system
    ${UI_ROOT}/network
    ${UI_ROOT}/drivers/storage
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
# 路径重定向包装 (sim_spiffs.c 实现):
#   fopen  → /spiffs/ 重定向 (历史遗留, 新代码已全走 fd)
#   open   → /assets/ → ../assets_fs/, /data/ → ../simdata/
#   stat64i32 → MinGW 坑: stat() 在头文件被 __MINGW_ASM_CALL 重定向到
#              stat64i32 符号, --wrap=stat 拦截不到, 必须 wrap stat64i32
#   opendir → 同上路径翻译 (readdir/closedir 走真实符号即可)
# ═══════════════════════════════════════════════════════════════════════════
target_link_options(lvgl_simulator PRIVATE
    -Wl,--wrap=fopen
    -Wl,--wrap=open
    -Wl,--wrap=stat64i32
    -Wl,--wrap=opendir
)

# ═══════════════════════════════════════════════════════════════════════════
# 编译定义
# ═══════════════════════════════════════════════════════════════════════════
target_compile_definitions(lvgl_simulator PRIVATE
    LV_CONF_INCLUDE_SIMPLE
    # MinGW CRT 告警抑制 (open/stat 等非 MSVC 命名在 ucrt 下被标记 deprecated)
    _CRT_NONSTDC_NO_DEPRECATE
    _CRT_SECURE_NO_WARNINGS
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
