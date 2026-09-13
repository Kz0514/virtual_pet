#!/usr/bin/env python3
"""动画包 v3 预览/帧序编辑工具 — pywebview 网页前端版 (2026-09-13).

架构: 后端 (本文件, 数据层 + Win32 文件对话框) + 前端 (anim_editor.html,
单文件 HTML/CSS/JS, pywebview 窗口加载)。帧数据/帧序段解析与重建复用
gen_anim_bin 语义 (格式对齐见该文件头注释 v3 节)。

后端职责:
  - 包解析 parse_pack / 帧序段 parse_seq_blob → JSON meta (与 anim_meta.json
    同结构, 前端直接编辑)
  - 帧解密 → PNG data URI (1280×1280 无缩放, 前端 CSS 缩放播放)
  - rebuild: 按 meta 重打包 (帧数据从包内素材原样搬运, 不重编码)
  - Win32 打开/保存对话框 (线程安全, js_api 方法在后台线程调用)

开发:   python tools/anim_editor.py
打包:   pyinstaller --onefile --windowed --name anim_editor \
        --add-data "tools/anim_editor.html;." tools/anim_editor.py
"""
import base64
import ctypes
import io
import json
import os
import struct
import sys
from ctypes import wintypes

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_anim_bin import (      # noqa: E402
    PACK_MAGIC, PACK_VERSION_SEQ, FRAME_SIZE, build_seq_blob, decode_frame,
)

FW = FH = 240

# anim_id → 名称 (包内无名字; 与 gen_anim_bin.ANIMS 一致)
DEFAULT_NAMES = {
    0: "IDLE", 1: "HAPPY", 2: "SAD", 3: "EXCITED", 4: "SLEEPY", 5: "EATING",
    6: "SURPRISED", 7: "BLUSH", 8: "PATHEAD", 9: "SCRATCH", 10: "POINTSELF",
}


# ══════════ 包解析 (与既有实现同语义) ══════════

def parse_pack(path):
    with open(path, "rb") as f:
        pack = f.read()
    if len(pack) < 16:
        raise ValueError("文件过短 (<16B)")
    magic, version, total, seq_len = struct.unpack_from("<IIII", pack, 0)
    if magic != PACK_MAGIC:
        raise ValueError("不是动画包 (magic 不符)")
    if version != PACK_VERSION_SEQ:
        raise ValueError(f"不是 v3 动画包 (version={version})")
    if not (1 <= total <= 256) or not (1 <= seq_len <= 65535):
        raise ValueError("帧表/帧序段长度异常")
    head = 16
    tab = []
    for i in range(total):
        aid, off, flags = struct.unpack_from("<III", pack, head + i * 12)
        tab.append((aid, off, flags))
    data_off = head + 12 * total + seq_len
    if data_off + 4 > len(pack):
        raise ValueError("帧序段截断")
    return pack, tab, data_off


def parse_seq_blob(pack):
    _, _, total, seq_len = struct.unpack_from("<IIII", pack, 0)
    seq = pack[16 + 12 * total: 16 + 12 * total + seq_len]
    loops = seq[0]
    n = struct.unpack_from("<I", seq, 4)[0]
    pos = 8
    anims = []
    for _ in range(n):
        anim_id, count, fps = struct.unpack_from("<HBB", seq, pos)
        pos += 5
        frames = []
        for _ in range(count):
            dur, lb, le = struct.unpack_from("<HBB", seq, pos)
            frames.append((dur, lb, le))
            pos += 4
        anims.append((anim_id, count, fps, frames))
    return loops, anims


class Pack:
    def __init__(self, path):
        self.path = path
        self.pack, self.tab, self.data_off = parse_pack(path)
        self._first = {}
        for i, (aid, _, _) in enumerate(self.tab):
            self._first.setdefault(aid, i)

    def pack_count(self, anim_id):
        fi = self._first.get(anim_id, -1)
        if fi < 0:
            return 0
        n = 0
        for k in range(fi, len(self.tab)):
            if self.tab[k][0] != anim_id:
                break
            n += 1
        return n

    def frame_bytes(self, anim_id, idx):
        fi = self._first.get(anim_id, -1) + idx
        if fi < 0 or fi >= len(self.tab) or self.tab[fi][0] != anim_id:
            return None
        _, off, flags = self.tab[fi]
        size = flags & 0x0FFFFFFF
        p = self.data_off + off
        return self.pack[p: p + size]

    def frame_pil(self, anim_id, idx):
        blob = self.frame_bytes(anim_id, idx)
        if blob is None:
            return None
        fi = self._first.get(anim_id, -1) + idx
        rgb565, ok = decode_frame(blob, self.tab[fi][2], FRAME_SIZE)
        if not ok:
            return None
        return _rgb565_to_pil(rgb565)

    def frame_datauri(self, anim_id, idx):
        """帧 → PNG data URI (256×256 1px 边框含内 — 前端 CSS 缩放)."""
        img = self.frame_pil(anim_id, idx)
        if img is None:
            return None
        buf = io.BytesIO()
        img.save(buf, "PNG")
        return "data:image/png;base64," + base64.b64encode(
            buf.getvalue()).decode()


def _rgb565_to_pil(rgb565):
    from PIL import Image
    img = Image.new("RGB", (FW, FH))
    px = img.load()
    for i in range(0, len(rgb565), 2):
        v = rgb565[i] | (rgb565[i + 1] << 8)
        r = (v >> 11) & 0x1F
        g = (v >> 5) & 0x3F
        b = v & 0x1F
        y, x = divmod(i // 2, FW)
        px[x, y] = (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)
    return img


# ══════════ 重打包 (meta dict → v3 bytes; 帧数据原样搬运) ══════════

def rebuild_pack_from_meta(pack, meta):
    """meta 结构 = anim_meta.json: {play_loops, anims:[{id,name,count,fps,
    frames:[{dur,loop_back,loop_extra}]}]}. 帧素材从包内按帧号搬运."""
    packed = []
    off = 0
    for a in meta["anims"]:
        for idx in range(a["count"]):
            blob = pack.frame_bytes(a["id"], idx)
            if blob is None:
                raise ValueError(f"{a.get('name', a['id'])}"
                                 f" 缺素材帧 {idx} — 帧数超包")
            fi = pack._first.get(a["id"], -1) + idx
            flags = pack.tab[fi][2]
            packed.append((a["id"], off, flags, blob))
            off += len(blob)
    seq_blob = build_seq_blob(meta)
    out = bytearray()
    out += struct.pack("<IIII", PACK_MAGIC, PACK_VERSION_SEQ,
                       len(packed), len(seq_blob))
    for aid, eoff, flags, _blob in packed:
        out += struct.pack("<III", aid, eoff, flags)
    out += seq_blob
    for _aid, _eoff, _fl, blob in packed:
        out += blob
    return bytes(out)


# ══════════ Win32 文件对话框 (线程安全 — js_api 在后台线程) ══════════

class _OFN(ctypes.Structure):
    _fields_ = [
        ("lStructSize", wintypes.DWORD),
        ("hwndOwner", wintypes.HWND),
        ("hInstance", wintypes.HINSTANCE),
        ("lpstrFilter", wintypes.LPCWSTR),
        ("lpstrCustomFilter", wintypes.LPWSTR),
        ("nMaxCustFilter", wintypes.DWORD),
        ("nFilterIndex", wintypes.DWORD),
        ("lpstrFile", wintypes.LPWSTR),
        ("nMaxFile", wintypes.DWORD),
        ("lpstrFileTitle", wintypes.LPWSTR),
        ("nMaxFileTitle", wintypes.DWORD),
        ("lpstrInitialDir", wintypes.LPCWSTR),
        ("lpstrTitle", wintypes.LPCWSTR),
        ("flags", wintypes.DWORD),
        ("nFileOffset", wintypes.WORD),
        ("nFileExtension", wintypes.WORD),
        ("lpstrDefExt", wintypes.LPCWSTR),
        ("lCustData", wintypes.LPARAM),
        ("lpfnHook", wintypes.LPVOID),
        ("lpTemplateName", wintypes.LPCWSTR),
    ]


def win32_open_dialog(title, filter, initial_dir):
    buf = ctypes.create_unicode_buffer(4096)
    ofn = _OFN()
    ofn.lStructSize = ctypes.sizeof(_OFN)
    ofn.lpstrFilter = filter + "\0"
    ofn.lpstrFile = buf
    ofn.nMaxFile = 4096
    ofn.lpstrTitle = title
    ofn.lpstrInitialDir = initial_dir or None
    ofn.flags = 0x00001000 | 0x00000800      # OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST
    if ctypes.windll.comdlg32.GetOpenFileNameW(ctypes.byref(ofn)):
        return buf.value
    return None


def win32_save_dialog(title, filter, initial_dir, default_name):
    buf = ctypes.create_unicode_buffer(4096)
    buf.value = default_name or ""
    ofn = _OFN()
    ofn.lStructSize = ctypes.sizeof(_OFN)
    ofn.lpstrFilter = filter + "\0"
    ofn.lpstrFile = buf
    ofn.nMaxFile = 4096
    ofn.lpstrTitle = title
    ofn.lpstrInitialDir = initial_dir or None
    ofn.flags = 0x00000002                        # OFN_OVERWRITEPROMPT
    if ctypes.windll.comdlg32.GetSaveFileNameW(ctypes.byref(ofn)):
        return buf.value
    return None


try:
    import webview
except ImportError:
    webview = None


class Api:
    """前端 JS 可通过 window.pywebview.api.xxx() 调用."""

    def __init__(self):
        self.pack = None
        self.project_home = os.path.normpath(os.path.dirname(
            os.path.dirname(os.path.abspath(__file__))))

    # ── 打开 ──
    def open_pack_dialog(self):
        path = win32_open_dialog("打开动画包 v3",
                                 "anims.bin (*.bin)\0*.bin\0All\0*.*\0",
                                 self.project_home)
        if not path:
            return None
        return self.load_pack(path)

    def load_default(self):
        """启动自载: 项目内 assets_fs/anims.bin (exe 则找 exe 同目录)."""
        if getattr(sys, "frozen", False):
            base = os.path.dirname(sys.executable)
        else:
            base = self.project_home
        for p in (os.path.join(self.project_home, "assets_fs", "anims.bin"),
                  os.path.join(base, "anims.bin")):
            if os.path.exists(p):
                try:
                    return self.load_pack(p)
                except ValueError:
                    continue
        return None

    def load_pack(self, path):
        pack = Pack(path)
        loops, seq = parse_seq_blob(pack.pack)
        anims = []
        for anim_id, _c, fps, frames in seq:
            n = pack.pack_count(anim_id)
            if n == 0:
                n = _c
            anims.append({
                "id": anim_id,
                "name": DEFAULT_NAMES.get(anim_id, f"anim{anim_id}"),
                "prefix": DEFAULT_NAMES.get(anim_id, f"anim{anim_id}").lower(),
                "count": n, "fps": fps,
                "frames": [{"dur": d, "loop_back": lb, "loop_extra": le}
                           for d, lb, le in frames],
            })
        # 帧图: 全部一次性 PNG data URI。解码结果缓存到 sidecar
        # (<path>.uris.json, mtime+size 判决效) — 首次 ~1-2s, 之后秒开
        frames = self._frame_uris_cached(pack, path, anims)
        self.pack = pack
        meta = {"play_loops": loops, "anims": anims}
        return {"ok": True, "meta": meta, "frames": frames,
                "info": {"path": path, "total": len(pack.tab)}}

    def _frame_uris_cached(self, pack, path, anims):
        cache_p = path + ".uris.json"
        try:
            st = os.stat(path)
            key = (st.st_mtime_ns, st.st_size)
            with open(cache_p, "r", encoding="utf-8") as f:
                c = json.load(f)
            if c.get("size") == key[1] and c.get("mtime") == key[0]:
                return c["uris"]
        except (OSError, ValueError, KeyError):
            pass
        uris = {}
        for a in anims:
            for idx in range(a["count"]):
                uri = pack.frame_datauri(a["id"], idx)
                if uri:
                    uris[f"{a['id']}:{idx}"] = uri
        try:
            with open(cache_p, "w", encoding="utf-8") as f:
                json.dump({"size": key[1], "mtime": key[0], "uris": uris}, f)
        except OSError:
            pass
        return uris

    # ── 保存 ──
    def save_json(self, meta):
        path = win32_save_dialog(
            "保存帧序 JSON", "JSON (*.json)\0*.json\0All\0*.*\0",
            os.path.join(self.project_home, "assets"), "anim_meta.json")
        if not path:
            return {"ok": False, "error": "已取消"}
        try:
            with open(path, "w", encoding="utf-8") as f:
                json.dump(meta, f, ensure_ascii=False, indent=2)
        except OSError as e:
            return {"ok": False, "error": str(e)}
        return {"ok": True, "path": path}

    def rebuild_pack(self, meta):
        if self.pack is None:
            return {"ok": False, "error": "尚未打开包"}
        try:
            out = rebuild_pack_from_meta(self.pack, meta)
        except ValueError as e:
            return {"ok": False, "error": str(e)}
        path = win32_save_dialog(
            "打包输出 anims.bin", "anims.bin (*.bin)\0*.bin\0All\0*.*\0",
            os.path.join(self.project_home, "assets_fs"), "anims.bin")
        if not path:
            return {"ok": False, "error": "已取消"}
        try:
            with open(path, "wb") as f:
                f.write(out)
        except OSError as e:
            return {"ok": False, "error": str(e)}
        return {"ok": True, "path": path, "size": len(out)}


def main():
    html = os.path.join(
        sys._MEIPASS if getattr(sys, "frozen", False) else
        os.path.dirname(os.path.abspath(__file__)), "anim_editor.html")
    if webview is None:
        print("缺少 pywebview (pip install pywebview)")
        return
    api = Api()
    webview.create_window("动画包 v3 编辑器", html, js_api=api,
                          width=1440, height=920, background_color="#14171b")
    webview.start()


if __name__ == "__main__":
    main()