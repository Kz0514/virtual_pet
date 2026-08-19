#!/usr/bin/env python3
"""glyph_decode.py — 从 binfont 文件提取字形位图, 分别用紧排/行填充两种方式解码"""
import struct
import sys


def extract(path, gid, box_w, box_h, bpp=4):
    data = open(path, "rb").read()
    # loca: chunk at 0x5F8 (zh_test.bin), data = count + offsets
    # offsets 相对 glyf 块起点 (glyf_start=0x15048, data at 0x15050)
    count = struct.unpack_from("<I", data, 0x600)[0]
    offs = struct.unpack_from(f"<{count}I", data, 0x604)
    glyf_start = 0x15048

    # 收集字形字节 (含头)
    raw = data[glyf_start + offs[gid]: glyf_start + offs[gid + 1]]
    bits = []
    for byte in raw:
        for b in range(8):
            bits.append((byte >> (7 - b)) & 1)
    # 跳过 30 bit 头 (adv10 + xy5+xy5 + wh5+wh5)
    bitmap_bits = bits[30:]

    n_px = box_w * box_h
    px = []
    for i in range(n_px):
        v = 0
        for k in range(bpp):
            v = (v << 1) | bitmap_bits[i * bpp + k]
        px.append(v)
    return px


def show(px, w, h, label):
    print(f"--- {label} ---")
    chars = " .:-=+*#%@"
    for y in range(h):
        row = "".join(chars[min(px[y * w + x], 9)] for x in range(w))
        print(f"  {row}")


if __name__ == "__main__":
    path = sys.argv[1]
    # 喵 U+55B5: cmap[11] range 0x55A6, gid = 2093 + (0x55B5-0x55A6) = 2108
    px = extract(path, 2108, 15, 14)
    show(px, 15, 14, "喵 15x14 紧排解码 (文件原始bit流)")

    # 你 U+4F60: cmap[4] range 0x4EA6, gid = 301 + (0x4F60-0x4EA6) = 487
    px = extract(path, 487, 16, 16)
    show(px, 16, 16, "你 16x16 紧排解码")

    # ! U+0021: cmap[1] range 0x20, type=FORMAT0_TINY, gid = 2 + 1 = 3
    px = extract(path, 3, 2, 13)
    show(px, 2, 13, "! 2x13 紧排解码")
