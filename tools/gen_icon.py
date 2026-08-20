#!/usr/bin/env python3
"""生成 U盘磁盘图标 (pet.ico) → main/app/usb_icon.h (C 字节数组, 无 PIL 依赖)

内置默认图标替换: 把想用的图标 (支持 .ico) 存成 tools/pet.ico,
再运行 python tools/gen_icon.py — 直接逐字节嵌入, 不绘制。
(用 .png/.jpg 可先用转换工具转成 .ico; 或改下方 IMAGE_OVERRIDE 路径)
没有 tools/pet.ico 时兜底绘制: (32x32 设计坐标, 多尺寸缩放)
青色圆角方块 + 白猫脸 (耳/眼/腮红/嘴)。
ICO 容器: 32bpp BGRA 位图条目 (BITMAPINFOHEADER + XOR/AND), Windows 原生支持。
用法: python tools/gen_icon.py
"""
import os
import struct

TEAL      = (78, 205, 196)    # 背景
TEAL_DARK = (52, 180, 172)    # 背景描边
WHITE     = (255, 255, 255)
EYE       = (45, 45, 60)
BLUSH     = (255, 175, 185)
MOUTH     = (255, 150, 160)

SIZES = (16, 32, 48)
OUT_H = "main/app/usb_icon.h"
IMAGE_OVERRIDE = "tools/pet.ico"   # 存在时逐字节嵌入, 覆盖绘制


def in_rounded_rect(x, y, r=7.0):
    """圆角矩形距离: 负值=内, 正值=外 (32 空间)"""
    cx = min(max(x, r), 32.0 - r)
    cy = min(max(y, r), 32.0 - r)
    return ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5 - r


def in_triangle(px, py, a, b, c):
    """点在三角形内 (同侧法)"""
    def sign(p1, p2, p3):
        return (p1[0] - p3[0]) * (p2[1] - p3[1]) - (p2[0] - p3[0]) * (p1[1] - p3[1])
    d1 = sign((px, py), a, b)
    d2 = sign((px, py), b, c)
    d3 = sign((px, py), c, a)
    neg = (d1 < 0) or (d2 < 0) or (d3 < 0)
    pos = (d1 > 0) or (d2 > 0) or (d3 > 0)
    return not (neg and pos)


def in_circle(x, y, cx, cy, r):
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def pixel_color(x, y):
    """返回 (r, g, b, a); x,y 为 32 空间连续坐标 (像素中心)"""
    # 背景圆角方块
    d = in_rounded_rect(x, y)
    if d > 0:
        return (0, 0, 0, 0)
    base = TEAL_DARK if d > -1.0 else TEAL

    # 耳朵 (白色三角, 与脸同色合并)
    ear_l = in_triangle(x, y, (6, 3), (12, 3), (8, 13))
    ear_r = in_triangle(x, y, (20, 3), (26, 3), (24, 13))
    face = in_circle(x, y, 16, 20, 10.5)
    if ear_l or ear_r or face:
        base = WHITE

    # 五官
    if in_circle(x, y, 11, 18, 2.2) or in_circle(x, y, 21, 18, 2.2):
        return EYE + (255,)
    if in_circle(x, y, 7, 21, 1.6) or in_circle(x, y, 25, 21, 1.6):
        return BLUSH + (255,)
    # 小嘴 (w 形三像素弧)
    if (in_circle(x, y, 15, 23, 1.0) or in_circle(x, y, 17, 23, 1.0)
            or in_circle(x, y, 16, 24, 1.0)):
        return MOUTH + (255,)
    return base + (255,)


def make_bmp(size):
    """32bpp BGRA 位图 + AND mask (ico 条目)"""
    hdr = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    rows = b""
    for py in range(size - 1, -1, -1):           # bottom-up
        row = b""
        for px in range(size):
            x = (px + 0.5) * 32.0 / size
            y = (py + 0.5) * 32.0 / size
            r, g, b, a = pixel_color(x, y)
            row += struct.pack("<BBBB", b, g, r, a)
        rows += row
    and_row = b"\x00" * (((size + 31) // 32) * 4)  # 全 0 — 32bpp 用 alpha, 掩码空
    return hdr + rows + and_row * size


def build_ico():
    entries, pdata = [], b""
    offset = 6 + 16 * len(SIZES)
    for s in SIZES:
        bmp = make_bmp(s)
        entries.append(struct.pack("<BBBBHHII", s & 0xFF, s & 0xFF, 0, 0, 1, 32,
                                   len(bmp), offset))
        pdata += bmp
        offset += len(bmp)
    return struct.pack("<HHH", 0, 1, len(SIZES)) + b"".join(entries) + pdata


def emit_c(data):
    # 空图标 = 不设置自定义磁盘图标 (Windows 系统默认图标)
    if data:
        body = "".join(
            "    " + ", ".join("0x%02x" % b for b in data[i:i + 16]) + ",\n"
            for i in range(0, len(data), 16))
    else:
        body = "    0x00,\n"
    lines = [
        "/* 自动生成 — tools/gen_icon.py, 勿手改。",
        " * pet.ico: U盘模式磁盘图标 (Windows 经 autorun.inf ICON= 显示);",
        " * 空数组 = 不设置图标 (系统默认)。",
        " * 自定义: 放 tools/pet.ico 后重新构建 (build.bat 自动重跑本脚本) */",
        "#ifndef USB_ICON_H",
        "#define USB_ICON_H",
        "#include <stdint.h>",
        "#include <stddef.h>",
        "static const uint8_t usb_icon_ico[] = {",
        body,
        "};",
        "static const size_t usb_icon_ico_len = %d;" % len(data),
        "#endif",
        "",
    ]
    text = "\n".join(lines)
    # 内容未变不重写 — 否则每次构建都触发 usb_storage.c 重编译
    try:
        with open(OUT_H, "r", encoding="utf-8") as f:
            if f.read() == text:
                return
    except OSError:
        pass
    with open(OUT_H, "w", encoding="utf-8") as f:
        f.write(text)


if __name__ == "__main__":
    if os.path.exists(IMAGE_OVERRIDE):
        with open(IMAGE_OVERRIDE, "rb") as f:
            ico = f.read()
        emit_c(ico)
        print(f"OK: {OUT_H} ({len(ico)} bytes, verbatim from {IMAGE_OVERRIDE})")
    else:
        # 无自定义图标 → 空数组 = 不设置图标 (Windows 系统默认磁盘图标)
        emit_c(b"")
        print(f"OK: {OUT_H} (empty — 无自定义图标, 系统默认)")
