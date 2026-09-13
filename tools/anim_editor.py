#!/usr/bin/env python3
"""动画包 v3 可视化预览/帧序编辑工具 (tkinter + Pillow).

功能:
  - 打开 anims.bin (v3) → 解析帧表 + 内嵌帧序段
  - 预览: 动画列表 | 大图 (240x240 @2x) | 帧缩略图条 | 按固件帧序播放
    (播放算法与 pet_avatar.c frame_timer_cb 一致: 子循环 loop_back 回跳 +
     play_loops 整组循环; 时长 0 → 1000/fps)
  - 编辑帧序: 每帧 dur / loop_back / loop_extra + 每动画 fps + play_loops
  - 保存 JSON (assets/anim_meta.json 同格式)
  - 重打包 anims.bin (帧数据从包内解码源搬运, 无需素材目录)

运行:   python tools/anim_editor.py
打包:   pyinstaller --onefile --windowed --name anim_editor tools/anim_editor.py
依赖:   Pillow (WebP 解码 + 预览显示)

帧序 JSON 布局与 gen_anim_bin.build_seq_blob / 固件 seq_apply 一一对应;
修改格式必须两侧同步 (见 gen_anim_bin.py 头注释 v3 节).
"""
import json
import os
import struct
import sys
import threading
import tkinter as tk
from tkinter import ttk, filedialog, messagebox

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_anim_bin import (      # noqa: E402
    PACK_MAGIC, PACK_VERSION_SEQ, FRAME_SIZE, build_seq_blob, decode_frame,
)

FW = FH = 240
SCALE = 2                          # 预览放大倍数
THUMB = 60                         # 帧条缩略图边长
GIL_CACHE_DECODING = object()      # 占位: 解码中

# anim_id → 名称 (包内无名字; JSON 缺失时兜底 — 与 gen_anim_bin.ANIMS 一致)
DEFAULT_NAMES = {
    0: "IDLE", 1: "HAPPY", 2: "SAD", 3: "EXCITED", 4: "SLEEPY", 5: "EATING",
    6: "SURPRISED", 7: "BLUSH", 8: "PATHEAD", 9: "SCRATCH", 10: "POINTSELF",
}


# ══════════ 包解析 ══════════

def parse_pack(path):
    """读 v3 包 → (pack_bytes, table, data_off).
    table = [(anim_id, off, flags), ...]; 帧数据在
    pack[data_off+off : data_off+off+(flags&0x0FFFFFFF)]."""
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
    """帧序段 → (play_loops, [(anim_id, count, fps, [(dur,lb,le),...]), ...]).
    布局: u8 loops + pad3 + u32 count + 条目 {u16 id,u8 n,u8 fps,u8 pad}
    + n×{u16 dur, u8 loop_back, u8 loop_extra} (固件 seq_apply 同布局)."""
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


class Anim:
    """一个动画: 帧序元数据 + 包内素材帧 (gil: 按动画内 idx 懒解密)."""
    def __init__(self, anim_id, name, count, fps, frames):
        self.id = anim_id
        self.name = name
        self.count = count            # 包内实际帧数 (素材)
        self.fps = fps                # 默认帧率 (时长 0 → 1000/fps)
        self.frames = frames          # [(dur, loop_back, loop_extra), ...]
        self.gil = {}                 # idx → PIL Image (懒解码缓存)


class Pack:
    """载入的包: 字节 + 表 + 素材通过 Frame 接口取."""
    def __init__(self, path):
        self.path = path
        self.pack, self.tab, self.data_off = parse_pack(path)
        self._first = {}              # anim_id → 表条目起始
        for i, (aid, _, _) in enumerate(self.tab):
            self._first.setdefault(aid, i)
        self._last_seen = None        # 缓存最近一次取帧的表条目号 (优雅线性)

    def pack_count(self, anim_id):
        """包内某动画的素材帧数 (表条目连续数)."""
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
        """解码帧 → PIL RGB Image (None=失败)."""
        blob = self.frame_bytes(anim_id, idx)
        if blob is None:
            return None
        fi = self._first.get(anim_id, -1) + idx
        flags = self.tab[fi][2]
        rgb565, ok = decode_frame(blob, flags, FRAME_SIZE)
        if not ok:
            return None
        return _rgb565_to_pil(rgb565)


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


# ══════════ 播放器 (语义与固件 frame_timer_cb 一致) ══════════

class Player:
    """帧序播放状态机. tick() → (pos, dur_ms, done)."""
    def __init__(self, anim, play_loops):
        self.anim = anim
        self.play_loops = play_loops
        self.pos = 0
        self.loop_count = 0
        self.loop_active = False
        self.loop_remaining = 0

    def reset(self, anim=None, play_loops=None):
        if anim is not None:
            self.anim = anim
        if play_loops is not None:
            self.play_loops = play_loops
        self.pos = 0
        self.loop_count = 0
        self.loop_active = False
        self.loop_remaining = 0

    def _dur(self, f):
        dur = f[0]
        if dur == 0:
            dur = 1000 // (self.anim.fps or 15)
        return dur if dur > 0 else 150

    def tick(self):
        """固件语义 (frame_timer_cb): 帧 loop_back>0 → 首达时置激活并剩余
        loops-1 次, 剩余>0 回跳 (返回跳回后帧); 耗尽继续前进。前进到末尾:
        无子循环整组播 play_loops 次, 有子循环播 1 遍。预览循环不停."""
        n = len(self.anim.frames)
        if n == 0:
            return (0, 150, False)
        f = self.anim.frames[self.pos]
        if f[1] > 0:                                # 子循环帧
            if not self.loop_active:
                self.loop_active = True
                self.loop_remaining = (self.play_loops - 1
                                       if self.play_loops > 0 else 0)
            if self.loop_remaining > 0:
                self.loop_remaining -= 1
                self.pos -= f[1]
                f = self.anim.frames[self.pos]
                return (self.pos, self._dur(f), False)
            self.loop_active = False
        self.pos += 1
        done = False
        if self.pos >= n:
            self.pos = 0
            self.loop_count += 1
            has_subloop = any(fr[1] > 0 for fr in self.anim.frames)
            target = 1 if has_subloop else self.play_loops
            if self.loop_count >= target:
                done = True
                self.loop_count = 0      # 预览: 循环继续
        f = self.anim.frames[self.pos]
        return (self.pos, self._dur(f), done)


# ══════════ Application ══════════

def rebuild_pack(pack, anims, play_loops):
    """重打包 → v3 bytes. 帧数据从包内素材原样搬运 (帧号 = 帧序内序号),
    帧表/帧序段按新元数据重建 → 只改帧序不必重编码素材."""
    packed = []
    off = 0
    for a in anims:
        for idx in range(a.count):
            blob = pack.frame_bytes(a.id, idx)
            if blob is None:
                raise ValueError(f"{a.name} 缺素材帧 {idx} — 帧数超包")
            fi = pack._first.get(a.id, -1) + idx
            flags = pack.tab[fi][2]        # 素材原始 flags (codec/raw 原样)
            packed.append((a.id, off, flags, blob))
            off += len(blob)
    meta = {
        "play_loops": play_loops,
        "anims": [{
            "id": a.id, "count": a.count, "fps": a.fps,
            "frames": [{"dur": d, "loop_back": lb, "loop_extra": le}
                       for d, lb, le in a.frames],
        } for a in anims],
    }
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


class App:
    def __init__(self, root):
        self.root = root
        root.title("动画包 v3 编辑器")
        root.geometry("1280x840")
        self.pack = None              # Pack
        self.anims = []               # [Anim]
        self.play_loops = 3
        self.selected = None          # Anim
        self.player = None
        self.after_id = None
        self._build_ui()

    # ── UI ──
    def _build_ui(self):
        bar = ttk.Frame(self.root, padding=4)
        bar.pack(side=tk.TOP, fill=tk.X)
        ttk.Button(bar, text="打开 anims.bin", command=self.open_pack).pack(side=tk.LEFT)
        ttk.Button(bar, text="保存 JSON", command=self.save_json).pack(side=tk.LEFT, padx=(6, 0))
        ttk.Button(bar, text="打包 anims.bin", command=self.rebuild_pack).pack(side=tk.LEFT, padx=(6, 0))
        ttk.Separator(bar, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=8)
        ttk.Button(bar, text="▶ 播放", command=self.play).pack(side=tk.LEFT)
        ttk.Button(bar, text="■ 停止", command=self.stop).pack(side=tk.LEFT, padx=(4, 0))
        ttk.Label(bar, text="循环轮数:").pack(side=tk.LEFT, padx=(16, 2))
        self.var_loops = tk.IntVar(value=3)
        ttk.Spinbox(bar, from_=1, to=9, width=3, textvariable=self.var_loops,
                    command=self._loops_changed).pack(side=tk.LEFT)

        paned = ttk.Panedwindow(self.root, orient=tk.HORIZONTAL)
        paned.pack(side=tk.TOP, fill=tk.BOTH, expand=True)

        # 左: 动画列表 + fps
        left = ttk.Frame(paned, padding=(6, 4))
        paned.add(left, weight=1)
        self.listbox = tk.Listbox(left, width=24, exportselection=False)
        sb = ttk.Scrollbar(left, orient=tk.VERTICAL, command=self.listbox.yview)
        self.listbox.configure(yscrollcommand=sb.set)
        self.listbox.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        sb.pack(side=tk.RIGHT, fill=tk.Y)
        self.listbox.bind("<<ListboxSelect>>", self._anim_selected)
        info = ttk.Frame(left, padding=(0, 6))
        info.pack(side=tk.BOTTOM, fill=tk.X)
        ttk.Label(info, text="默认fps:").pack(side=tk.LEFT)
        self.var_fps = tk.IntVar(value=8)
        ttk.Spinbox(info, from_=1, to=60, width=4, textvariable=self.var_fps,
                    command=self._fps_changed).pack(side=tk.LEFT)

        # 中: 大图 + 帧条
        mid = ttk.Frame(paned, padding=(6, 4))
        paned.add(mid, weight=3)
        self.canvas = tk.Canvas(mid, width=FW * SCALE, height=FH * SCALE,
                                bg="#222", highlightthickness=0)
        self.canvas.pack(side=tk.TOP)
        self.var_state = tk.StringVar(value="未打开包")
        ttk.Label(mid, textvariable=self.var_state).pack(side=tk.TOP, pady=(2, 0))
        self.thumb_canvas = tk.Canvas(mid, height=THUMB + 26, bg="#333",
                                      highlightthickness=0)
        self.thumb_canvas.pack(side=tk.BOTTOM, fill=tk.X, pady=(6, 0))

        # 右: 帧序表
        right = ttk.Frame(paned, padding=(6, 4))
        paned.add(right, weight=2)
        cols = ("dur", "lb", "le")
        heads = {"dur": "时长ms", "lb": "回跳", "le": "额外"}
        self.tree = ttk.Treeview(right, columns=cols, show="headings",
                                 height=18, selectmode="browse")
        for c in cols:
            self.tree.heading(c, text=heads[c])
            self.tree.column(c, width=80, anchor="center")
        tsb = ttk.Scrollbar(right, orient=tk.VERTICAL, command=self.tree.yview)
        self.tree.configure(yscrollcommand=tsb.set)
        self.tree.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        tsb.pack(side=tk.RIGHT, fill=tk.Y)
        self.tree.bind("<Double-1>", self._tree_edit)
        ttk.Label(right, wraplength=320, justify="left",
                  text="双击帧行编辑: 时长ms (0=默认fps) / 回跳 (该帧播完跳回 N 步) / 额外 (保留位)").pack(
                  side=tk.BOTTOM, pady=(4, 0), anchor="w")

        status = ttk.Frame(self.root, padding=(6, 2))
        status.pack(side=tk.BOTTOM, fill=tk.X)
        self.var_status = tk.StringVar(value="就绪 — 打开 anims.bin 开始")
        ttk.Label(status, textvariable=self.var_status).pack(side=tk.LEFT)

    # ── 打开包 ──
    def open_pack(self):
        path = filedialog.askopenfilename(
            title="打开动画包", filetypes=[("anims.bin", "*.bin"), ("所有文件", "*")],
            initialdir=self._home())
        if not path:
            return
        try:
            pack = Pack(path)
        except ValueError as e:
            messagebox.showerror("打开失败", str(e))
            return
        self.pack = pack
        try:
            loops, seq_anims = parse_seq_blob(pack.pack)
        except (IndexError, struct.error) as e:
            messagebox.showerror("打开失败", f"帧序段解析失败: {e}")
            return
        self.play_loops = loops
        self.var_loops.set(loops)
        self.anims = []
        for anim_id, _count, fps, frames in seq_anims:
            n = pack.pack_count(anim_id)
            # 帧序段 count 与素材数相等时用素材数; 缺素材动画照列
            if n == 0:
                n = _count
            self.anims.append(Anim(anim_id,
                                   DEFAULT_NAMES.get(anim_id, f"anim{anim_id}"),
                                   n, fps, list(frames)))
        self.listbox.delete(0, tk.END)
        for a in self.anims:
            self.listbox.insert(tk.END, f"{a.name:9s} id{a.id:2d} {a.count}帧")
        if self.anims:
            self.listbox.selection_set(0)
            self._anim_selected()
        self.var_status.set(f"{path} — v3 包 {len(pack.tab)} 帧, 帧序段数据驱动")
        threading.Thread(target=self._predecode, daemon=True).start()

    def _home(self):
        return os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

    def _predecode(self):
        """后台预解码全部素材帧 → Anims[].gil; 每动画完成即刷新缩略条."""
        pack = self.pack
        if not pack:
            return
        for a in self.anims:
            for idx in range(a.count):
                if idx in a.gil:
                    continue
                img = pack.frame_pil(a.id, idx)
                if img is None:
                    break
                a.gil[idx] = img
            self.root.after(0, self._thumbs_refresh, a)
        self.root.after(0, self.var_state.set, "预解码完成 — 全部帧就绪")

    def _thumbs_refresh(self, a):
        if self.selected is a:
            self._redraw_thumbs()

    # ── 显示 ──
    def _photo(self, img, size):
        from PIL import Image, ImageTk
        if img.size != (size, size):
            img = img.resize((size, size), Image.LANCZOS)
        return ImageTk.PhotoImage(img)

    def _anim_img(self, anim, idx, decode=True):
        """取帧图. decode=False 时只查缓存 (缩略条用 — 缺帧画灰块,
        由 _predecode 线程补齐后刷新, 避免同步解码卡 UI / 递归重绘)."""
        img = anim.gil.get(idx)
        if img is None and decode and idx < anim.count:
            img = self.pack.frame_pil(anim.id, idx)
            if img is not None:
                anim.gil[idx] = img
        return img

    def _show_frame(self, anim, idx):
        img = self._anim_img(anim, idx)
        self.canvas.delete("all")
        if img is None:
            self.canvas.create_text(FW * SCALE // 2, FH * SCALE // 2,
                                    text="无帧", fill="#888")
            return
        # 引用挂 canvas 属性: 覆盖旧图 → 旧 PhotoImage 释放 (防播放内存泄漏)
        self.canvas._photo = self._photo(img, FW * SCALE)
        self.canvas.create_image(0, 0, image=self.canvas._photo, anchor=tk.NW)

    def _redraw_thumbs(self):
        c = self.thumb_canvas
        c.delete("all")
        a = self.selected
        if not a:
            return
        photos = []          # 本次重绘的引用集中持有, 覆盖旧列表即释放
        x = 4
        for idx in range(a.count):
            tag = f"t{idx}"
            img = self._anim_img(a, idx, decode=False)
            if img is None:
                c.create_rectangle(x, 2, x + THUMB, 2 + THUMB, fill="#555")
            else:
                ph = self._photo(img, THUMB)
                photos.append(ph)
                c.create_image(x, 2, image=ph, anchor=tk.NW, tags=(tag,))
            c.create_text(x + THUMB // 2, THUMB + 8, text=str(idx),
                          fill="#ccc", font=("Helvetica", 8))
            c.tag_bind(tag, "<Button-1>", lambda e, k=idx: self._thumb_click(k))
            x += THUMB + 8
        c._photos = photos
        c.configure(scrollregion=(0, 0, x, THUMB + 26))

    def _thumb_click(self, idx):
        self.stop()
        self._show_frame(self.selected, idx)
        self.tree.selection_set(str(idx + 1))
        self.tree.see(str(idx + 1))

    # ── 动画选择 / 参数 ──
    def _anim_selected(self, _e=None):
        sel = self.listbox.curselection()
        if not sel:
            return
        a = self.anims[sel[0]]
        self.selected = a
        self.var_fps.set(a.fps)
        self.tree.delete(*self.tree.get_children())
        for i, (dur, lb, le) in enumerate(a.frames, 1):
            self.tree.insert("", tk.END, iid=str(i),
                             values=(self._dur_text(dur, a), lb, le))
        self._redraw_thumbs()
        self.stop()
        self._show_frame(a, 0)
        self.var_state.set(f"{a.name}: {a.count} 帧")

    def _dur_text(self, dur, a):
        return f"{dur}ms" if dur else f"{1000 // (a.fps or 15)}ms(默认)"

    def _fps_changed(self):
        a = self.selected
        if not a:
            return
        v = self.var_fps.get()
        if v < 1:
            v = 8
            self.var_fps.set(v)
        a.fps = v
        for i in range(len(a.frames)):
            if self.tree.exists(str(i + 1)):
                dur, lb, le = a.frames[i]
                self.tree.set(str(i + 1), "dur", self._dur_text(dur, a))
        self.var_status.set(f"{a.name}: fps → {v}")

    def _loops_changed(self):
        v = self.var_loops.get()
        if v < 1:
            v = 3
            self.var_loops.set(v)
        self.play_loops = v

    # ── 帧序编辑 (双击) ──
    def _tree_edit(self, evt):
        item = self.tree.identify_row(evt.y)
        col = self.tree.identify_column(evt.x)
        if not item or col not in ("#2", "#3", "#4"):
            return
        a = self.selected
        if not a:
            return
        i = int(item) - 1
        key = {"#2": 0, "#3": 1, "#4": 2}[col]
        labels = ["时长ms (0=默认fps)", "loop_back 回跳步数", "loop_extra 额外"]
        win = tk.Toplevel(self.root)
        win.title(f"编辑 {a.name} 第{i}帧 — {labels[key]}")
        win.resizable(False, False)
        ttk.Label(win, text=labels[key] + ":").pack(padx=10, pady=(8, 0))
        var = tk.IntVar(value=a.frames[i][key])
        rng = (0, 65535) if key == 0 else (0, 255)
        ttk.Spinbox(win, from_=rng[0], to=rng[1], width=8,
                    textvariable=var).pack(padx=10, pady=4)
        btns = ttk.Frame(win)
        btns.pack(pady=6)

        def ok():
            try:
                v = int(var.get())
            except ValueError:
                return
            v = max(min(v, rng[1]), rng[0])
            tup = list(a.frames[i])
            tup[key] = v
            a.frames[i] = tuple(tup)
            self.tree.set(item, col, self._dur_text(v, a) if key == 0 else v)
            if self.after_id:
                self.play()          # 播放中 → 按新时长继续
            win.destroy()

        ttk.Button(btns, text="确定", command=ok).pack(side=tk.LEFT, padx=6)
        ttk.Button(btns, text="取消", command=win.destroy).pack(side=tk.LEFT)

    # ── 播放 ──
    def play(self):
        a = self.selected
        if not a:
            return
        if self.after_id:
            self.stop()
        if self.player and self.player.anim is a:
            self.player.reset(a, self.play_loops)
        else:
            self.player = Player(a, self.play_loops)
        self._tick_loop()

    def stop(self):
        if self.after_id:
            self.root.after_cancel(self.after_id)
            self.after_id = None

    def _tick_loop(self):
        p = self.player
        if not p:
            return
        idx, dur, done = p.tick()
        self._show_frame(p.anim, idx)
        self.tree.selection_set(str(idx + 1))
        self.tree.see(str(idx + 1))
        self.var_state.set(f"{p.anim.name} 帧序 #{idx}  dur={dur}ms"
                           f"{'  完成一轮' if done else ''}  轮 {p.loop_count}")
        self.after_id = self.root.after(dur, self._tick_loop)

    # ── 保存 / 打包 ──
    def save_json(self):
        if not self.anims:
            messagebox.showinfo("提示", "先打开 anims.bin")
            return
        meta = {
            "_comment": "帧序表唯一数据源 (v3); tools/anim_editor.py 读写",
            "play_loops": self.play_loops,
            "anims": [{
                "id": a.id, "name": a.name,
                "prefix": a.name.lower(),
                "count": a.count, "fps": a.fps,
                "frames": [{"dur": d, "loop_back": lb, "loop_extra": le}
                           for d, lb, le in a.frames],
            } for a in self.anims],
        }
        default = os.path.normpath(os.path.join(self._home(), "assets", "anim_meta.json"))
        path = filedialog.asksaveasfilename(
            title="保存帧序 JSON", defaultextension=".json",
            initialfile=os.path.basename(default),
            initialdir=os.path.dirname(default),
            filetypes=[("JSON", "*.json")])
        if not path:
            return
        with open(path, "w", encoding="utf-8") as f:
            json.dump(meta, f, ensure_ascii=False, indent=2)
        self.var_status.set(f"JSON 已保存: {path}")

    def rebuild_pack(self):
        if not self.pack or not self.anims:
            messagebox.showinfo("提示", "先打开 anims.bin")
            return
        try:
            out = rebuild_pack(self.pack, self.anims, self.play_loops)
        except ValueError as e:
            messagebox.showerror("打包失败", str(e))
            return
        default = os.path.normpath(os.path.join(self._home(), "assets_fs", "anims.bin"))
        path = filedialog.asksaveasfilename(
            title="打包输出", defaultextension=".bin", initialfile="anims.bin",
            initialdir=os.path.dirname(default),
            filetypes=[("anims.bin", "*.bin")])
        if not path:
            return
        with open(path, "wb") as f:
            f.write(bytes(out))
        self.var_status.set(f"打包完成: {path} ({len(out)} bytes) — 烧录 assets 后设备读取")


def main():
    root = tk.Tk()
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()