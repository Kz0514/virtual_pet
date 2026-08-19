#!/usr/bin/env python3
"""
analyze_font.py — 对比分析 LVGL 二进制字体 (zh.bin vs zh_test.bin)

Chunk 格式: [len:u32][label:4s][data...]  len 含自身 8 字节头
head 结构见 lv_binfont_loader.c font_header_bin_t (40 bytes)
"""
import struct
import sys

HEAD_FMT = "<IHHHHHHHHHHHBBBBBBBBBBhH"  # 40 bytes


def parse_chunks(data):
    """返回 {label: (data_offset, chunk_total_len)}"""
    chunks = {}
    off = 0
    while off + 8 <= len(data):
        (length,) = struct.unpack_from("<I", data, off)
        label = data[off + 4:off + 8].decode("latin1")
        chunks[label] = (off + 8, length)
        off += length
    return chunks


def analyze(path):
    with open(path, "rb") as f:
        data = f.read()

    chunks = parse_chunks(data)
    print(f"=== {path} ({len(data)} bytes) ===")
    for label, (off, length) in chunks.items():
        print(f"  chunk '{label}': data_off={off:#x} total_len={length}")

    # head
    hoff, _ = chunks["head"]
    h = struct.unpack_from(HEAD_FMT, data, hoff)
    (version, tables_count, font_size, ascent, descent, typo_ascent,
     typo_descent, typo_line_gap, min_y, max_y, default_adv_w, kern_scale,
     loc_format, glyph_id_fmt, adv_w_fmt, bpp, xy_bits, wh_bits,
     adv_w_bits, compression, subpx, padding, underline_pos,
     underline_thickness) = h
    print(f"  version={version} tables={tables_count} size={font_size} "
          f"ascent={ascent} descent={descent}")
    print(f"  bpp={bpp} adv_w_fmt={adv_w_fmt} adv_w_bits={adv_w_bits} "
          f"xy={xy_bits} wh={wh_bits} compression={compression} "
          f"glyph_id_fmt={glyph_id_fmt} loc_fmt={loc_format} "
          f"kern_scale={kern_scale}")

    # cmap — cmap_table_bin_t = u32+u32+u16+u16+u16+u8+u8 = 16 bytes
    coff, _ = chunks["cmap"]
    (n,) = struct.unpack_from("<I", data, coff)
    print(f"  cmap subtables: {n}")
    total_entries = 0
    for i in range(n):
        t = struct.unpack_from("<IIHHHBB", data, coff + 4 + i * 16)
        data_offset, range_start, range_length, gid_start, entries, fmt, pad = t
        total_entries += entries
        if i < 12:
            print(f"    [{i}] range=U+{range_start:04X} len={range_length} "
                  f"gid_start={gid_start} entries={entries} fmt={fmt} "
                  f"data_off={data_offset}")
    print(f"  cmap total entries: {total_entries}")

    # loca — index_to_loc_format=1 → u32 offsets
    loff, llen = chunks["loca"]
    n_glyphs = (llen - 8) // 4
    first5 = [struct.unpack_from("<I", data, loff + 4 * i)[0] for i in range(5)]
    print(f"  loca: {n_glyphs} glyph offsets, first 5: {first5}")

    # glyf
    goff, glen = chunks["glyf"]
    print(f"  glyf: {glen} bytes total")
    print(f"  glyf first 32 bytes: {data[goff:goff+32].hex()}")

    # 解析每个子表的真实映射 (fmt=2: unicode_list+gid_list; fmt=3: unicode_list)
    def build_map(data, cmaps_start):
        unicode_to_gid = {}
        for i in range(n):
            t = struct.unpack_from("<IIHHHBB", data, coff + 4 + i * 16)
            data_offset, range_start, range_length, gid_start, entries, fmt, pad = t
            if fmt == 2:  # SPARSE_FULL
                base = cmaps_start + data_offset
                ulist = struct.unpack_from(f"<{entries}H", data, base)
                glist = struct.unpack_from(f"<{entries}H", data, base + 2 * entries)
                for u, g in zip(ulist, glist):
                    unicode_to_gid[u] = g
            elif fmt == 3:  # SPARSE_TINY
                base = cmaps_start + data_offset
                ulist = struct.unpack_from(f"<{entries}H", data, base)
                for idx, u in enumerate(ulist):
                    unicode_to_gid[u] = gid_start + idx
            elif fmt == 0:  # FULL (dense u8)
                base = cmaps_start + data_offset
                olist = struct.unpack_from(f"<{entries}B", data, base)
                for k, off in enumerate(olist):
                    unicode_to_gid[range_start + k] = gid_start + off
        return unicode_to_gid

    unicode_to_gid = build_map(data, 48)
    print(f"  total mapped codepoints: {len(unicode_to_gid)}")
    return chunks, data, unicode_to_gid


def check_chars(name, u2g, chars):
    print(f"--- {name} ---")
    for ch in chars:
        code = ord(ch)
        gid = u2g.get(code)
        if gid is not None:
            print(f"  '{ch}' U+{code:04X}: gid={gid}  OK")
        else:
            print(f"  '{ch}' U+{code:04X}: MISSING")


if __name__ == "__main__":
    results = []
    for p in sys.argv[1:]:
        results.append((p, analyze(p)))
        print()

    # 检查关键字符
    test_chars = ["你", "喵", "哦", "!", "A", "啊", "吗", "好"]
    if len(results) >= 1:
        p, (chunks, data, u2g) = results[0]
        check_chars(p, u2g, test_chars)
        print()
    if len(results) >= 2:
        p, (chunks, data, u2g) = results[1]
        check_chars(p, u2g, test_chars)


if __name__ == "__main__":
    for p in sys.argv[1:]:
        analyze(p)
        print()
