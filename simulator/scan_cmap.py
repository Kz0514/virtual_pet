#!/usr/bin/env python3
"""scan_cmap.py — 扫描字体文件的 cmap 数据区, 找出所有升序 u16 列表"""
import struct
import sys


def find_ascending_lists(data, start, end, min_len=8):
    """在 [start,end) 内找升序 u16 序列 (允许偶发乱值打断)"""
    lists = []
    i = start
    while i + 2 <= end:
        # 找潜在列表起点: 连续 3 个递增 u16
        a = struct.unpack_from("<H", data, i)[0]
        b = struct.unpack_from("<H", data, i + 2)[0]
        c = struct.unpack_from("<H", data, i + 4)[0]
        if 0x3000 <= a < 0xA000 and a < b < c:
            # 追踪列表长度
            j = i
            prev = a - 1
            run = 0
            while j + 2 <= end:
                v = struct.unpack_from("<H", data, j)[0]
                if v > prev and v < 0xA000:
                    prev = v
                    run += 1
                    j += 2
                else:
                    break
            if run >= min_len:
                lists.append((i, run, a, prev))
                i = j
                continue
        i += 2
    return lists


def main():
    for path in sys.argv[1:]:
        data = open(path, "rb").read()
        # cmap 块位置
        (clen,) = struct.unpack_from("<I", data, 0x30)
        cmap_end = 0x30 + clen
        print(f"=== {path}: cmap 数据区 0x38..0x{cmap_end:X} ===")
        lists = find_ascending_lists(data, 0x38, cmap_end)
        for off, count, first, last in lists:
            print(f"  u16 升序列表 @ 0x{off:X}: {count} 项, "
                  f"U+{first:04X}..U+{last:04X}")
        if not lists:
            print("  未找到列表")


if __name__ == "__main__":
    main()
