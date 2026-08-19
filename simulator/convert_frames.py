#!/usr/bin/env python3
"""
convert_frames.py — 将 assets/animation/<name>/frame_XX.c 转换为 spiffs/<prefix>_XX.bin

动画帧.c 文件格式:
    const uint16_t <name>_XX[57600] = { 0x0000, 0x0000, ... };

输出: 原始 RGB565 字节 (240x240 x 2 = 115200 bytes)

用法:
    python convert_frames.py <源目录> <输出前缀> [--max N]

示例:
    python convert_frames.py ../assets/animation/zhanli zhanli
    → simulator/spiffs/zhanli_00.bin ... zhanli_04.bin

    python convert_frames.py ../assets/animation/shuijiao sleep --max 13
    → simulator/spiffs/sleep_00.bin ... sleep_12.bin

宠物动画名称与源目录对应关系 (参考 pet_avatar.c):
    idle/sad    → zhanli/         → zhanli (5帧)
    happy       → gaoxingjiangjie/ → happy  (6帧)
    excited     → baoxiongshuohua/ → talk   (10帧)
    sleepy      → shuijiao/        → sleep  (13帧)
    eating      → e/               → eating (19帧)
    surprised   → dunzhe/          → squat  (2帧)
"""

import os
import re
import sys


def parse_frame_c(filepath):
    """解析 frame_XX.c 文件, 提取 uint16_t 数组为 bytes"""
    with open(filepath, "r", encoding="utf-8") as f:
        content = f.read()

    # 匹配所有 0xXXXX 十六进制值
    hex_vals = re.findall(r"0x[0-9a-fA-F]{4}", content)
    if not hex_vals:
        print(f"  WARNING: {filepath}: 未找到十六进制数据")
        return None

    data = bytearray()
    for h in hex_vals:
        val = int(h, 16)
        data.append(val & 0xFF)        # 低字节
        data.append((val >> 8) & 0xFF) # 高字节

    expected = 240 * 240 * 2  # 115200
    if len(data) != expected:
        print(f"  WARNING: {filepath}: 数据长度={len(data)}, 期望={expected}")

    return bytes(data)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)

    src_dir = sys.argv[1]
    out_prefix = sys.argv[2]
    max_frames = None

    # 解析可选参数
    i = 3
    while i < len(sys.argv):
        if sys.argv[i] == "--max" and i + 1 < len(sys.argv):
            max_frames = int(sys.argv[i + 1])
            i += 2
        else:
            i += 1

    # 输出到 simulator/spiffs/
    script_dir = os.path.dirname(os.path.abspath(__file__))
    out_dir = os.path.join(script_dir, "spiffs")
    os.makedirs(out_dir, exist_ok=True)

    count = 0
    for fname in sorted(os.listdir(src_dir)):
        m = re.match(r"frame_(\d+)\.c", fname)
        if not m:
            continue
        idx = int(m.group(1))
        if max_frames is not None and idx >= max_frames:
            continue

        src_path = os.path.join(src_dir, fname)
        data = parse_frame_c(src_path)
        if data is None:
            continue

        out_name = f"{out_prefix}_{idx:02d}.bin"
        out_path = os.path.join(out_dir, out_name)
        with open(out_path, "wb") as f:
            f.write(data)

        print(f"  OK {out_name}  ({len(data)} bytes)")
        count += 1

    print(f"\nDone: {count} frames -> {out_dir}/")


if __name__ == "__main__":
    main()
