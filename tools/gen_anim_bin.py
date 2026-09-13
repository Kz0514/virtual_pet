#!/usr/bin/env python3
"""生成动画包 assets_fs/anims.bin / simulator/spiffs/anims.bin (单文件包 + 帧表).

背景: SPIFFS 的 open() 线性扫全分区元数据, 文件数 90+ 时 open 实测
0.3-1.5s (冻结整个 LVGL)。open 只与文件数相关, 与文件大小无关 —
把全部动画帧 (含 idle) 合并为 1 个文件, open 降到毫秒级;
固件侧 fd 常开 + lseek/read 纯块读播放。

导入素材: assets/anim_bin/ 下的 RGB565 帧 bin (115200B/帧)。
idle(zhanli) 与全部非 idle 动画都在包里; spiffs/ 里其余文件
(zh.bin 等) 原样保留。

素材演进: 帧 bin 增删/改名 → 改下方 ANIMS → 重跑本脚本 → 重烧 assets。
分区表永远不动, SPIFFS 格式不变。

文件格式 v2 (双解码器时代):
  [头部] {u32 magic="ANI2", u32 version=2, u32 total_frames}
         + total_frames × {u32 anim_id, u32 off, u32 size_flags}
         头部总长 = 12 + 12×total_frames; off 相对数据区起点
  [数据] 帧数据平铺 (同动画帧连续, 固件按 first+idx 定位)
  size_flags: bit31=1 → 原始 RGB565 未压缩 (size=115200)
              bit31=0 → bits30-28 = codec (0=RLE, 1=WebP 无损),
                         低 28 位 = 压缩字节数
  RLE 帧: n × {u16 run_len, u16 color565} (行程 >65535 拆段, 防御)
  WebP 帧: Pillow 无损编码 (RGB565→RGB888→WebP, 往返 RGB888→RGB565
           逐位精确, 解码回比可全帧逐字节校验)

文件格式 v3 (2026-09-13, 帧序表内嵌 — 数据驱动):
  [头部] {u32 magic="ANI2", u32 version=3, u32 total_frames, u32 seq_len}
         + total_frames × {u32 anim_id, u32 off, u32 size_flags} (同 v2)
         + 帧序段 seq_len bytes (播放节奏, 见下)
         + 帧数据平铺 (off 相对数据区起点 = 16 + 12×total + seq_len)
  帧序段 (固件 pet_avatar.c 按此布局解析):
    u8  play_loops            (整组循环轮数, 无子循环动画用)
    u8  pad[3]
    u32 anim_count
    anim_count × {u16 anim_id, u8 count, u8 fps, u8 pad}
    anim_count × count × {u16 duration_ms, u8 loop_back, u8 pad}
  duration_ms=0 → 用默认 fps (1000/fps) — 与固件语义一致。
  JSON 源: assets/anim_meta.json (帧序表唯一数据源, exe 编辑器也读写它);
  JSON 缺失 → 退回 v2 打包 (兼容旧流程)。

固件侧 (pet_avatar.c) 校验 magic 后按 version 分流: v2 老路径 / v3 读帧序段
覆盖内部表。老固件 (version 校验=2) 读 v3 包 → 头部无效整体拒绝 → 宠物
不显示, 系统照常 (既有优雅降级)。
双解码器 (RLE + WebP) 都实现, 由每帧 codec flag 分发; 实机探针计时后
定夺最终 codec (定夺 = 改 --codec 重跑, 容器/固件零改动)。

用法:
  gen_anim_bin.py [--codec rle|webp] [--out PATH] [--verify]
  默认同时输出 assets_fs/anims.bin 与 simulator/spiffs/anims.bin。
  --out 只写指定文件 (如探针用 anims_webp.bin)。
  --verify 读回生成的包按对应 codec 解码全部帧, 与素材逐字节比对。
"""
import argparse
import io
import json
import os
import struct
import sys

SRC_DIR = os.path.join(os.path.dirname(__file__), "..", "assets", "anim_bin")
OUT_DEFAULT = [
    os.path.join(os.path.dirname(__file__), "..", "assets_fs", "anims.bin"),
    os.path.join(os.path.dirname(__file__), "..", "simulator", "spiffs", "anims.bin"),
]
FRAME_SIZE = 240 * 240 * 2

PACK_MAGIC = 0x32494E41  # "ANI2" — v1 是 "ANIM", 必须不同 (老固件读新包走 magic 失败)
PACK_VERSION = 2
PACK_VERSION_SEQ = 3    # v3: 头 16B + 帧序段 (JSON 驱动, 见文件头注释)
FLAG_RAW = 0x80000000
CODEC_RLE = 0
CODEC_WEBP = 1

SEQ_META_PATH = os.path.join(os.path.dirname(__file__), "..", "assets",
                             "anim_meta.json")

# (anim_id, prefix, frame_count) — anim_id 与 pet_avatar.h 枚举值一致:
# IDLE=0 HAPPY=1 SAD=2(复用zhanli, 无素材) EXCITED=3 SLEEPY=4 EATING=5
# SURPRISED=6 BLUSH=7 PATHEAD=8 SCRATCH=9 POINTSELF=10
# JSON (assets/anim_meta.json) 存在时替代本表并为 v3 提供帧序段; 缺失 → 本表 + v2。
ANIMS = [
    (0, "zhanli", 5),
    (1, "happy", 6),
    (3, "talk", 10),
    (4, "sleep", 13),
    (5, "eating", 19),
    (6, "squat", 2),
    (7, "blush", 14),
    (8, "pathead", 7),
    (9, "scratch", 2),
    (10, "pointself", 5),
]


def load_meta():
    """读 assets/anim_meta.json → (meta | None). meta 含 play_loops + anims[].
    帧序表唯一数据源 (exe 编辑器同步读写); 缺失 = 旧 v2 流程."""
    if not os.path.exists(SEQ_META_PATH):
        return None
    with open(SEQ_META_PATH, encoding="utf-8") as f:
        return json.load(f)


def build_seq_blob(meta):
    """帧序段 (v3): u8 play_loops + pad3 + u32 anim_count +
    anim_count × {u16 anim_id, u8 count, u8 fps, u8 pad} +
    anim_count × count × {u16 duration_ms, u8 loop_back, u8 pad}.
    布局与固件 pet_avatar.c 解析一一对应, 修改必须两侧同步."""
    out = bytearray()
    out += struct.pack("<B", int(meta["play_loops"]))
    out += b"\x00\x00\x00"
    anims = meta.get("anims", [])
    out += struct.pack("<I", len(anims))
    for a in anims:
        frames = a.get("frames", [])
        if len(frames) != a["count"]:
            raise SystemExit(f"meta {a.get('name')}: frames {len(frames)} "
                             f"!= count {a['count']}")
        out += struct.pack("<HBB", a["id"], a["count"], a["fps"])
        out += b"\x00"
        for fr in frames:
            out += struct.pack("<HBB", int(fr.get("dur", 0)),
                                int(fr.get("loop_back", 0)), 0)
    return bytes(out)


def anim_list(meta):
    """返回 [(anim_id, prefix, count)] — JSON 优先, 缺失回退内置表."""
    if meta:
        return [(a["id"], a["prefix"], a["count"]) for a in meta.get("anims", [])]
    return ANIMS


def rle_encode(data):
    """pair-RLE: {u16 run_len, u16 color565} 序列; 行程 >65535 拆段."""
    out = bytearray()
    i = 0
    n = len(data) // 2
    while i < n:
        v = data[i * 2] | (data[i * 2 + 1] << 8)
        j = i + 1
        while j < n and (data[j * 2] | (data[j * 2 + 1] << 8)) == v:
            j += 1
        run = j - i
        while run > 0xFFFF:
            out += struct.pack("<HH", 0xFFFF, v)
            run -= 0xFFFF
        out += struct.pack("<HH", run, v)
        i = j
    return bytes(out)


def rle_decode(data, out_len):
    """返回 (RGB565 bytes, ok); 行程长 0 或输出溢出判损坏."""
    out = bytearray()
    for off in range(0, len(data) - 3, 4):
        run, color = struct.unpack_from("<HH", data, off)
        if run == 0:
            return None, False
        out += struct.pack("<H", color) * run
        if len(out) > out_len:
            return None, False
    return bytes(out), len(out) == out_len


def webp_encode(data):
    """RGB565 → RGB888 → Pillow 无损 WebP (method=6 高压缩)."""
    try:
        from PIL import Image
    except ImportError:
        print("ERROR: 需要 Pillow (pip install pillow) 才能生成 WebP 包")
        sys.exit(1)
    img = Image.new("RGB", (240, 240))
    px = img.load()
    for i in range(0, len(data), 2):
        v = data[i] | (data[i + 1] << 8)
        r = (v >> 11) & 0x1F
        g = (v >> 5) & 0x3F
        b = v & 0x1F
        y, x = divmod(i // 2, 240)
        px[x, y] = (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)
    buf = io.BytesIO()
    img.save(buf, "WEBP", lossless=True, method=6)
    return buf.getvalue()


def webp_decode(data, out_len):
    """解码 WebP → RGB888 → RGB565 (往返逐位精确)."""
    try:
        from PIL import Image
    except ImportError:
        return None, False
    img = Image.open(io.BytesIO(data)).convert("RGB")
    if img.size != (240, 240):
        return None, False
    out = bytearray()
    for y in range(240):
        for x in range(240):
            r, g, b = img.getpixel((x, y))
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            out += struct.pack("<H", v)
    return bytes(out), len(out) == out_len


def encode_frame(data, codec):
    """返回 (blob, size_flags). 压缩后 ≥ 原始 → 存原始置 RAW flag."""
    if codec == CODEC_RLE:
        blob = rle_encode(data)
    else:
        blob = webp_encode(data)
    if len(blob) < len(data):
        return blob, (codec << 28) | len(blob)
    return data, FLAG_RAW | len(data)


def decode_frame(blob, size_flags, out_len):
    """按 size_flags 解码, 返回 (bytes, ok)."""
    if size_flags & FLAG_RAW:
        return blob, len(blob) == out_len
    codec = (size_flags >> 28) & 0x7
    if codec == CODEC_RLE:
        return rle_decode(blob, out_len)
    if codec == CODEC_WEBP:
        return webp_decode(blob, out_len)
    return None, False


def build_pack(codec, meta=None):
    """返回 (pack_bytes, pack_meta). pack_meta = [(anim_id, prefix, idx_in_anim, blob, flags)].
    meta (JSON) 给定 → v3 内嵌帧序段; None → v2 (老流程兼容)."""
    frames = []  # (anim_id, prefix, idx_in_anim, data)
    for anim_id, prefix, count in anim_list(meta):
        for i in range(count):
            p = os.path.join(SRC_DIR, f"{prefix}_{i:02d}.bin")
            with open(p, "rb") as f:
                data = f.read()
            if len(data) != FRAME_SIZE:
                print(f"ERROR: {p} 大小 {len(data)} != {FRAME_SIZE}")
                sys.exit(1)
            frames.append((anim_id, prefix, i, data))

    entries = []  # (anim_id, off, flags)
    blobs = []
    for anim_id, prefix, idx, data in frames:
        blob, flags = encode_frame(data, codec)
        entries.append((anim_id, 0, flags))
        blobs.append(blob)

    off = 0
    for i, (anim_id, _, flags) in enumerate(entries):
        entries[i] = (anim_id, off, flags)
        off += len(blobs[i])

    seq_blob = build_seq_blob(meta) if meta else b""
    version = PACK_VERSION_SEQ if meta else PACK_VERSION
    hdr_fmt = "<IIII" if meta else "<III"

    out = bytearray()
    out += struct.pack(hdr_fmt, PACK_MAGIC, version, len(entries), len(seq_blob))
    for anim_id, eoff, flags in entries:
        out += struct.pack("<III", anim_id, eoff, flags)
    out += seq_blob
    out += b"".join(blobs)

    pack_meta = [(a, p, idx, blobs[gi], entries[gi][2])
                 for gi, (a, p, idx, _) in enumerate(frames)]
    return bytes(out), pack_meta


def unpack_seq_blob(pack, version):
    """从包内解析帧序段 → {play_loops, anims:[{id,count,fps,frames:[(dur,loop_back)]}]}.
    逐字段结构校验 — 固件与脚本对同一布局解析, 不一致即 FAIL."""
    if version == PACK_VERSION:
        return None, 0, None
    magic, ver, total, seq_len = struct.unpack_from("<IIII", pack, 0)
    if magic != PACK_MAGIC or ver != PACK_VERSION_SEQ:
        print(f"ERROR: 头部无效 magic={magic:08x} version={ver}")
        return None, 0, None
    head = 16 + 12 * total
    seq = pack[head:head + seq_len]
    need = 8
    if len(seq) < need:
        print(f"ERROR: 帧序段过短 {len(seq)}B < {need}B")
        return None, seq_len, None
    play_loops = seq[0]
    anim_count = struct.unpack_from("<I", seq, 4)[0]
    pos = 8
    meta = {"play_loops": play_loops, "anims": []}
    for _ in range(anim_count):
        if pos + 5 > len(seq):
            print(f"ERROR: 帧序段截断 (条目标头 @{pos})")
            return None, seq_len, None
        anim_id, count, fps = struct.unpack_from("<HBB", seq, pos)
        pos += 5
        frames = []
        for _ in range(count):
            if pos + 4 > len(seq):
                print(f"ERROR: 帧序段截断 (帧 {anim_id} @{pos})")
                return None, seq_len, None
            dur, lb = struct.unpack_from("<HB", seq, pos)
            frames.append((dur, lb))
            pos += 4
        meta["anims"].append({"id": anim_id, "count": count, "fps": fps,
                              "frames": frames})
    if pos != len(seq):
        print(f"ERROR: 帧序段长度不符 {pos} != {len(seq)}")
        return None, seq_len, None
    return meta, seq_len, seq


def verify_pack(pack, pack_meta, codec_name):
    """读回解码全部帧, 与素材逐字节比对 (两 codec 的往返都必须无损)."""
    magic, version, total = struct.unpack_from("<III", pack, 0)
    if magic != PACK_MAGIC or version not in (PACK_VERSION, PACK_VERSION_SEQ):
        print(f"ERROR: 头部无效 magic={magic:08x} version={version}")
        return False
    seq_meta, seq_len, _ = unpack_seq_blob(pack, version)
    if version == PACK_VERSION_SEQ and seq_meta is None:
        return False
    table_start = 16 if version == PACK_VERSION_SEQ else 12  # v3 头 16B
    data_start = table_start + 12 * total
    if version == PACK_VERSION_SEQ:
        data_start += seq_len
    bad = 0
    for i, (anim_id, prefix, idx, _, flags) in enumerate(pack_meta):
        off = struct.unpack_from("<I", pack, table_start + i * 12 + 4)[0]
        p = os.path.join(SRC_DIR, f"{prefix}_{idx:02d}.bin")
        with open(p, "rb") as f:
            expect = f.read()
        blob = pack[data_start + off:data_start + off + (flags & 0x0FFFFFFF)]
        got, ok = decode_frame(blob, flags, FRAME_SIZE)
        if not ok or got != expect:
            bad += 1
            print(f"FAIL: {prefix}_{idx:02d} (size={len(blob)}, flags={flags:08x})")
    if bad:
        print(f"verify {codec_name}: {bad}/{total} 帧不一致!")
        return False
    print(f"verify {codec_name}: {total}/{total} 帧逐字节一致 [OK]")
    if version == PACK_VERSION_SEQ:
        print(f"verify seq: play_loops={seq_meta['play_loops']} "
              f"{len(seq_meta['anims'])} 动画, "
              f"帧序段 {seq_len}B [OK]")
    return True


def main():
    ap = argparse.ArgumentParser(description="生成动画包 (v2: RLE/WebP 双 codec; v3: JSON 帧序内嵌)")
    ap.add_argument("--codec", choices=["rle", "webp"], default="rle",
                    help="压缩方式 (默认 rle)")
    ap.add_argument("--out", help="只输出到指定文件 (默认双端 spiffs/ + simulator/spiffs/)")
    ap.add_argument("--verify", action="store_true", help="生成后读回解码逐字节比对素材")
    args = ap.parse_args()

    codec = CODEC_RLE if args.codec == "rle" else CODEC_WEBP
    meta = load_meta()
    pack, pack_meta = build_pack(codec, meta)
    version = PACK_VERSION_SEQ if meta else PACK_VERSION
    head = 16 + 12 * len(pack_meta) if meta else 12 + 12 * len(pack_meta)
    total_data = len(pack) - head
    raw_total = FRAME_SIZE * len(pack_meta)
    fmt = f"动画数: {len(ANIMS)}  帧数: {len(pack_meta)}  v{version}"
    if meta:
        fmt += " (JSON 帧序)"
    fmt += f"  codec={args.codec}  数据: {total_data} bytes " \
           f"({total_data/1048576:.2f} MB, {total_data/raw_total*100:.1f}% 原始)"
    print(fmt)
    n_raw = sum(1 for (_, _, _, _, flags) in pack_meta if flags & FLAG_RAW)
    if n_raw:
        print(f"  RAW 回退帧: {n_raw}/{len(pack_meta)}")

    if args.verify:
        if not verify_pack(pack, pack_meta, args.codec):
            sys.exit(1)

    outs = [args.out] if args.out else OUT_DEFAULT
    for out in outs:
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with open(out, "wb") as f:
            f.write(pack)
        print(f"OK -> {out} ({len(pack)} bytes)")


if __name__ == "__main__":
    main()
