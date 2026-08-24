# libwebp 解码器裁剪组件 (ESP32-S3)
#
# 只编译解码器所需源文件 (dec + dsp 通用 C + utils), 裁掉 enc/mux/demux
# 及所有平台优化 (neon/sse/mips/msa/avx2 — xtensa 上不可用, 自动落通用 C)。
#
# 不定义 HAVE_CONFIG_H → libwebp 用编译器检测平台, 无需 config.h
# (见 src/dsp/cpu.h: #if !defined(HAVE_CONFIG_H) 分支)。
#
# 来源: gitee mirror of webmproject/libwebp (master, 2026-08)
# 许可: BSD-3-Clause (见 COPYING / PATENTS)

set(SRC_DEC
    src/dec/alpha_dec.c
    src/dec/buffer_dec.c
    src/dec/frame_dec.c
    src/dec/idec_dec.c
    src/dec/io_dec.c
    src/dec/quant_dec.c
    src/dec/tree_dec.c
    src/dec/vp8_dec.c
    src/dec/vp8l_dec.c
    src/dec/webp_dec.c)

set(SRC_DSP
    src/dsp/alpha_processing.c
    src/dsp/cpu.c
    src/dsp/dec.c
    src/dsp/dec_clip_tables.c
    src/dsp/filters.c
    src/dsp/lossless.c
    src/dsp/rescaler.c
    src/dsp/upsampling.c
    src/dsp/yuv.c)

# utils 目录全量编译 — 解码器各 .c 间有隐蔽依赖 (如 frame_dec 需要
# random_utils 的 VP8InitRandom), 裁剪会踩 undefined reference;
# 未引用的函数/对象被 --gc-sections 丢弃, 不占 flash
set(SRC_UTILS
    src/utils/bit_reader_utils.c
    src/utils/bit_writer_utils.c
    src/utils/color_cache_utils.c
    src/utils/filters_utils.c
    src/utils/huffman_encode_utils.c
    src/utils/huffman_utils.c
    src/utils/palette.c
    src/utils/quant_levels_dec_utils.c
    src/utils/quant_levels_utils.c
    src/utils/random_utils.c
    src/utils/rescaler_utils.c
    src/utils/thread_utils.c
    src/utils/utils.c)

idf_component_register(
    SRCS ${SRC_DEC} ${SRC_DSP} ${SRC_UTILS}
    INCLUDE_DIRS . src)

# libwebp 源码里的 x86/ARM 平台宏在 xtensa 上未定义, 通用 C 路径编译会有
# 少量未使用告警; 静默以免刷屏
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-parameter
    -Wno-unused-function)
