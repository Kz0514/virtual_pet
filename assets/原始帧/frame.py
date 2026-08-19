#!/usr/bin/env python3
"""
将精灵帧图拆分为单帧 -> 缩放至指定尺寸 -> 转换为 RGB565 格式的 .c 数组文件。
输出文件名：frame_00.c, frame_01.c ... （两位数字）
内部数组名：原图名_序号 （如 walk_00, jump_01）避免编译冲突。

用法:
    python split_and_convert.py <输入文件夹> <输出文件夹> [缩放目标尺寸] [原帧尺寸]
    默认缩放目标 240，原帧尺寸 750
"""

import os
import sys
from PIL import Image

IMG_EXTENSIONS = ('.png', '.jpg', '.jpeg', '.gif', '.bmp', '.webp', '.tiff')

def rgb_to_565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)

def save_c_array(pixels, width, height, filepath, array_name):
    with open(filepath, 'w') as f:
        f.write('// Auto-generated RGB565 image data\n')
        f.write(f'// Size: {width}x{height}\n')
        f.write('#include <stdint.h>\n\n')
        f.write(f'const uint16_t {array_name}[{width * height}] = {{\n')

        for y in range(height):
            line = []
            for x in range(width):
                idx = y * width + x
                r, g, b = pixels[idx]
                val = rgb_to_565(r, g, b)
                line.append(f'0x{val:04X}')
            f.write('    ' + ', '.join(line) + ',\n')

        f.write('};\n')
    print(f'  [生成] {filepath}')

def process_image(img_path, output_dir, frame_size=750, target_size=240):
    try:
        img = Image.open(img_path).convert('RGB')
    except Exception as e:
        print(f'[错误] 无法打开 {img_path}: {e}')
        return 0

    width, height = img.size
    if height != frame_size:
        print(f'[警告] {os.path.basename(img_path)} 高度为 {height}，预期 {frame_size}，仍按 {frame_size}x{frame_size} 切割。')
    if width % frame_size != 0:
        print(f'[错误] 宽度 {width} 不能被帧宽 {frame_size} 整除，跳过。')
        return 0

    frame_count = width // frame_size
    base_name = os.path.splitext(os.path.basename(img_path))[0]
    out_subdir = os.path.join(output_dir, base_name)
    os.makedirs(out_subdir, exist_ok=True)

    for i in range(frame_count):
        left = i * frame_size
        right = left + frame_size
        frame = img.crop((left, 0, right, frame_size))

        if target_size != frame_size:
            frame = frame.resize((target_size, target_size), Image.LANCZOS)

        pixels = list(frame.getdata())

        # ★ 文件名改为 frame_00.c, frame_01.c ...（两位数字，无前缀）
        c_filename = f'frame_{i:02d}.c'
        c_path = os.path.join(out_subdir, c_filename)

        # ★ 内部数组名仍使用 "原图名_序号" 避免重名
        array_name = f'{base_name}_{i:02d}'

        save_c_array(pixels, target_size, target_size, c_path, array_name)

    return frame_count

def main():
    if len(sys.argv) < 3:
        print('用法: python split_and_convert.py <输入文件夹> <输出文件夹> [缩放目标尺寸] [原帧尺寸]')
        print('示例: python split_and_convert.py ./sprites ./output 240 750')
        sys.exit(1)

    input_dir = sys.argv[1]
    output_dir = sys.argv[2]
    target_size = int(sys.argv[3]) if len(sys.argv) >= 4 else 240
    frame_size = int(sys.argv[4]) if len(sys.argv) >= 5 else 750

    if not os.path.isdir(input_dir):
        print(f'错误: 输入文件夹 "{input_dir}" 不存在。')
        sys.exit(1)

    image_files = [f for f in os.listdir(input_dir) if f.lower().endswith(IMG_EXTENSIONS)]
    if not image_files:
        print(f'在 "{input_dir}" 中没有找到图片文件。')
        return

    print(f'找到 {len(image_files)} 个图片文件。')
    print(f'原帧尺寸: {frame_size}x{frame_size}, 缩放目标: {target_size}x{target_size}\n')

    total_frames = 0
    for filename in sorted(image_files):
        img_path = os.path.join(input_dir, filename)
        print(f'处理: {filename}')
        count = process_image(img_path, output_dir, frame_size, target_size)
        if count > 0:
            print(f'  完成，共生成 {count} 个 .c 文件。\n')
            total_frames += count

    print(f'全部完成！共处理 {len(image_files)} 张精灵图，生成 {total_frames} 个 .c 文件。')

if __name__ == '__main__':
    main()

