#!/usr/bin/env python3
"""VPet 素材导入器 — 从 VPet 的 mod/<mod>/pet/<人物>/ 树里挑一组帧, 转成本项目
的 240x240 RGB565 帧 bin (assets/anim_bin/<prefix>_NN.bin), 并给出 anim_meta.json
条目草稿。

架构同 anim_editor: 后端为本文件 (扫描/转换/去重/导出/Win32 对话框), 前端为
tools/vpet_import.html (pywebview 单文件), 打包见根目录 vpet_import.spec。

VPet 源树实况 (萝莉斯 资源包, mod/0000_core/pet/vup):
  25 个顶层动作 / 609 个含 PNG 的叶子目录 / 6181 张 PNG, 全部 1000x1000。
  目录是两层以上的不规则嵌套, 如 Touch_Head/B_Nomal、IDEL/Squat/B_Nomal/1。
  文件名三种形态, 且都可靠:
      名_序号_时长.png   摸头_000_125.png   68.7%
      _序号_时长.png     _000_125.png      30.9%
      名_序号.png / 序号.png               0.4% (缺时长, 落到 125ms)

解析纪律: 序号与时长只认文件名末尾的数字组, 名字里的数字不参与 ——
如 "循环1_000_125.png" 的 1 不是序号。全树 6181 张都解得出序号。

四条已知坑 (本文件都按此处理):
  1. 空帧 — mode='1' 的那批是全透明的, 集中在 front_lay/front (共 24 张)。
     按源图 alpha 极值判空, 默认不勾选。
  2. 序号不连续 — 609 个目录里 40 个。成因四种: 从 1 起、从中间续号
     (A/B/C 分段的续接, 属正常)、同号重复、单数字文件名把时长当序号。
     本文件按 (序号, 原名) 稳定排序, 不做强制归一。
  3. 分层目录 — front_lay/back_lay/front/back 是要叠在宠物前/后的层, 单层
     RGB565 表达不了。标注但不拦。
  4. 透明底泄漏 — 源图导出时透明区带 (20,20,31) 底色 (抽样过半的文件中招),
     直接 convert('RGB') 会把它铺成背景。必须先合成黑底再缩放。

帧的三级处理 (顺序有依赖, 见 build_plan):
  空帧      → 不勾选
  相邻同像素 → 并成一组, 时长相加到组首帧
  连续周期段 → 折叠: 只留一个周期, 在周期末帧挂 loop_back
  以上之外的重复帧一律保留 —— 非相邻重复往往是"回到某姿势"(收尾段),
  裸删会砍掉动作的收势。VPet 把循环次数烘焙在帧序列里, 折叠后次数由
  固件 s_play_loops 决定, 不需要复刻源素材的遍数。

折叠与 loop_back 是绑定的: 固件仅凭"序列里存在 loop_back"就判定整条只播一遍
(pet_avatar.c 的 has_subloop 分支), 只折叠不发 loop_back 会让 head+尾巴 播 3 遍。
同一序列最多折一处 —— 固件的子循环计数是单槽全局量。

开发:   python tools/vpet_import.py
打包:   pyinstaller tools/vpet_import.spec
"""
import base64
import hashlib
import io
import json
import os
import re
import sys

try:
    import webview
except ImportError:
    webview = None

from PIL import Image

FW = FH = 240
FRAME_BYTES = FW * FH * 2
DEFAULT_MS = 125          # 文件名缺时长时的兜底; 实测 92.6% 本来就是 125
MS_MIN_HINT = 100         # 单数字文件名的判别阈值: >= 值视为时长而非序号
                          # (实测序号最大 ~45, 时长最小 124, 间隔干净)

LAYER_RE = re.compile(r'(front_lay|back_lay|front|back)$', re.I)

# VPet 的 GraphType 枚举 (值同 C# 的 GraphInfo.GraphType, 顺序即匹配优先级, 首个命中即止)
GRAPH_TYPES = (
    'Common', 'Raised_Dynamic', 'Raised_Static', 'Move', 'Default', 'Touch_Head',
    'Touch_Body', 'Idel', 'Sleep', 'Say', 'StateONE', 'StateTWO', 'StartUP',
    'Shutdown', 'Work', 'Switch_Up', 'Switch_Down', 'Switch_Thirsty',
    'Switch_Hunger', 'SideHide_Left_Main', 'SideHide_Left_Rise',
    'SideHide_Right_Main', 'SideHide_Right_Rise',
)
MODE_ORDER = ('Nomal', 'Happy', 'PoorCondition', 'Ill')
_MODE_KW = (('happy', 'Happy'), ('nomal', 'Nomal'),
            ('poorcondition', 'PoorCondition'), ('ill', 'Ill'))
_ANIMAT_KW = ((('a', 'start'), 'A_Start'), (('b', 'loop'), 'B_Loop'),
              (('c', 'end'), 'C_End'), (('single',), 'Single'))

_CFG = os.path.join(os.path.expanduser('~'), '.vpet_import.json')


def load_cfg():
    try:
        with open(_CFG, encoding='utf-8') as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def save_cfg(**kv):
    c = load_cfg()
    c.update(kv)
    try:
        with open(_CFG, 'w', encoding='utf-8') as f:
            json.dump(c, f)
    except OSError:
        pass


def steam_vpet_roots():
    """从 Steam 注册表 + libraryfolders.vdf 拼出 VPet 的 vup 路径候选。"""
    import winreg
    libs = []
    for hive, sub in ((winreg.HKEY_CURRENT_USER, r'Software\Valve\Steam'),
                      (winreg.HKEY_LOCAL_MACHINE,
                       r'SOFTWARE\WOW6432Node\Valve\Steam')):
        for val in ('SteamPath', 'InstallPath'):
            try:
                with winreg.OpenKey(hive, sub) as k:
                    libs.append(winreg.QueryValueEx(k, val)[0])
            except OSError:
                pass
    out = []
    for lib in libs:
        roots = [lib]
        try:
            with open(os.path.join(lib, 'steamapps', 'libraryfolders.vdf'),
                      encoding='utf-8', errors='replace') as f:
                roots += re.findall(r'"path"\s+"([^"]+)"', f.read())
        except OSError:
            pass
        for r in roots:
            out.append(os.path.join(r.replace('\\\\', '\\'), 'steamapps', 'common',
                                    'VPet', 'mod', '0000_core', 'pet', 'vup'))
    return out


# ══════════ 文件名解析 ══════════

def parse_frame_name(fn):
    """文件名 → (序号|None, 时长ms|None, 备注|None)。只认末尾数字组。"""
    stem = fn[:-4] if fn.lower().endswith('.png') else fn
    m = re.search(r'_(\d+)_(\d+)$', stem)
    if m:
        return int(m.group(1)), int(m.group(2)), None
    m = re.search(r'_(\d+)$', stem)
    if m:
        v = int(m.group(1))
        if v >= MS_MIN_HINT:
            return None, v, '单数字判为时长'
        return v, None, None
    m = re.search(r'(\d+)$', stem)
    if m:
        v = int(m.group(1))
        if v >= MS_MIN_HINT:
            return None, v, '单数字判为时长'
        return v, None, None
    return None, None, '文件名无数字'


# ══════════ 扫描 ══════════

def find_leaves(root):
    """列出所有含 PNG 的叶子目录 (相对路径, 正斜杠)。"""
    out = []
    for dirpath, _, filenames in os.walk(root):
        if any(f.lower().endswith('.png') for f in filenames):
            out.append(os.path.relpath(dirpath, root).replace('\\', '/'))
    return sorted(out)


def read_dir(root, rel):
    """读一个叶子目录的帧元信息 (不打开图像, 快)。"""
    d = os.path.join(root, rel.replace('/', os.sep))
    pngs = [f for f in os.listdir(d) if f.lower().endswith('.png')]
    items = []
    for f in pngs:
        idx, ms, note = parse_frame_name(f)
        items.append({'file': f, 'idx': idx, 'ms': ms, 'note': note})
    # (序号, 原名) 稳定排序 —— 缺序号的排最后, 按原名兜底
    items.sort(key=lambda t: (t['idx'] is None, t['idx'] if t['idx'] is not None else 0, t['file']))

    idxs = [t['idx'] for t in items if t['idx'] is not None]
    contiguous = idxs == list(range(len(idxs)))
    ms_set = sorted({t['ms'] for t in items if t['ms'] is not None})
    return {
        'rel': rel,
        'count': len(items),
        'frames': items,
        'contiguous': contiguous,
        'ms_set': ms_set,
        'no_ms': sum(1 for t in items if t['ms'] is None),
        'no_idx': sum(1 for t in items if t['idx'] is None),
        'layered': bool(LAYER_RE.search(rel.split('/')[-1]) or
                        LAYER_RE.search(rel)),
    }


# ══════════ VPet 路径解析 (复刻 GraphInfo) ══════════

def _take(tokens, word):
    """等价 List<string>.Remove: 删掉首个匹配项, 返回是否删到。"""
    if word in tokens:
        tokens.remove(word)
        return True
    return False


def parse_graph_path(rel):
    """相对 startuppath 的路径 → (GraphType, Name, Mode, AnimatType)。

    关键词按 `_` 切词 (`/` 等价), 三段顺序任意、大小写不敏感; 名字取剥掉尾部
    数字后剩下的最后一段。四步顺序必须照 VPet: 状态 → 类型 → 动作 → 名字。
    """
    tk = [t for t in rel.lower().replace('\\', '_').replace('/', '_').split('_') if t]

    mode = 'Nomal'
    for word, m in _MODE_KW:
        if _take(tk, word):
            mode = m
            break

    gtype = 'Common'
    for gt in GRAPH_TYPES:
        parts = gt.lower().split('_')
        if parts[0] not in tk:
            continue
        at = tk.index(parts[0])
        hit = True
        for n in range(1, len(parts)):
            if at + n >= len(tk):          # 越界不算失配 —— VPet 原实现如此
                break
            if tk[at + n] != parts[n]:
                hit = False
                break
        if hit:
            gtype = gt
            del tk[at:at + len(parts)]
            break

    animat = 'Single'
    for words, a in _ANIMAT_KW:
        if any(_take(tk, w) for w in words):   # 短路: 删到就不再试同组的下一个
            animat = a
            break

    while tk and (re.fullmatch(r'\d+(\.\d*)?', tk[-1]) or tk[-1].startswith('~')):
        tk.pop()
    return gtype, (tk[-1] if tk else gtype.lower()), mode, animat


def build_groups(root):
    """全树 → 动画组。一组 = 一个 (GraphType, Name); 心情是该组的变体维度。"""
    acc = {}
    for rel in find_leaves(root):
        gt, name, mode, animat = parse_graph_path(rel)
        acc.setdefault((gt, name), {}).setdefault(mode, {}).setdefault(animat, []).append(rel)

    out = []
    for (gt, name), moods in acc.items():
        ms = []
        for mode in sorted(moods, key=lambda m: (MODE_ORDER.index(m)
                                                 if m in MODE_ORDER else 9, m)):
            sec = {k: [{'rel': r, 'n': read_dir(root, r)['count']}
                       for r in sorted(v)] for k, v in moods[mode].items()}
            ms.append({'mode': mode, 'sections': sec,
                       'dirs': sum(len(v) for v in sec.values()),
                       'frames': sum(x['n'] for v in sec.values() for x in v)})
        out.append({'key': f'{gt}/{name}', 'gt': gt, 'name': name, 'moods': ms,
                    'mode': ms[0]['mode'],           # 默认心情: Nomal 优先
                    'dirs': sum(m['dirs'] for m in ms),
                    'frames': sum(m['frames'] for m in ms)})
    out.sort(key=lambda g: (g['gt'], g['name']))
    return out


def scan_tree(root, max_frames_warn=45):
    """全树扫描 → 每个叶子目录的摘要 (不读像素)。"""
    rels = find_leaves(root)
    out = []
    by_top = {}
    for rel in rels:
        try:
            info = read_dir(root, rel)
        except OSError:
            continue
        top = rel.split('/')[0]
        out.append({
            'rel': rel,
            'top': top,
            'count': info['count'],
            'ms_set': info['ms_set'],
            'contiguous': info['contiguous'],
            'no_ms': info['no_ms'],
            'no_idx': info['no_idx'],
            'layered': info['layered'],
            'warn': info['count'] > max_frames_warn,
        })
        by_top[top] = by_top.get(top, 0) + 1
    return {'root': root, 'leaves': out,
            'tops': [{'name': k, 'dirs': v} for k, v in sorted(by_top.items())]}


# ══════════ 转换 ══════════

_TAB_R = [((i >> 3) << 11) for i in range(256)]
_TAB_G = [((i >> 2) << 5) for i in range(256)]
_TAB_B = [(i >> 3) for i in range(256)]


def rgb565_bytes(img_rgb):
    """PIL RGB → RGB565 小端字节串。"""
    px = img_rgb.tobytes()
    out = bytearray(FRAME_BYTES)
    tr, tg, tb = _TAB_R, _TAB_G, _TAB_B
    for p in range(FW * FH):
        v = tr[px[p * 3]] | tg[px[p * 3 + 1]] | tb[px[p * 3 + 2]]
        out[p * 2] = v & 0xFF
        out[p * 2 + 1] = v >> 8
    return bytes(out)


def convert_one(path):
    """源 PNG → (RGB565 bytes, 'ok'|'blank'|'bad')。

    合成黑底是必须的: 源图透明区带 (20,20,31) 底色, 直接 convert('RGB')
    会把它铺成可见背景, 角色抗锯齿边缘也会混到错的底色上。
    """
    try:
        im = Image.open(path).convert('RGBA')
    except OSError:
        return b'\x00' * FRAME_BYTES, 'bad'     # 源图截断/损坏, 别连累整个目录
    if im.getchannel('A').getextrema() == (0, 0):
        return b'\x00' * FRAME_BYTES, 'blank'
    bg = Image.new('RGB', im.size, (0, 0, 0))
    bg.paste(im, (0, 0), im)
    if bg.size != (FW, FH):
        bg = bg.resize((FW, FH), Image.LANCZOS)
    return rgb565_bytes(bg), 'ok'


def to_data_uri(rgb565):
    """RGB565 → PNG data URI (前端预览用)。

    故意走 565 已知的量化: 预览显示的就是设备实际会呈现的颜色。
    """
    rgb = bytearray(FW * FH * 3)
    for p in range(FW * FH):
        v = rgb565[p * 2] | (rgb565[p * 2 + 1] << 8)
        rgb[p * 3] = ((v >> 11) & 0x1F) << 3
        rgb[p * 3 + 1] = ((v >> 5) & 0x3F) << 2
        rgb[p * 3 + 2] = (v & 0x1F) << 3
    im = Image.frombytes('RGB', (FW, FH), bytes(rgb))
    buf = io.BytesIO()
    im.save(buf, 'PNG', optimize=True)
    return 'data:image/png;base64,' + base64.b64encode(buf.getvalue()).decode()


def build_plan(frames, keys):
    """→ {'grp': {帧号: 相邻同像素组的组首}, 'dup': {帧号: 首次出现的同像素帧}}。

    相邻同像素并入一组 (时长归组首); 非相邻的同像素帧也不导, 但时长不并到首次那张上
    —— 挪动它会改节奏, 这是为省 flash 主动接受的损失。
    """
    grp, dup, first, cur = {}, {}, {}, None
    for i, f in enumerate(frames):
        if f['blank'] or f['bad']:
            continue
        if cur is not None and keys[i] == keys[cur]:
            grp[i] = grp[cur]
            continue
        grp[i] = i
        cur = i
        if keys[i] in first:
            dup[i] = first[keys[i]]
        else:
            first[keys[i]] = i
    return {'grp': grp, 'dup': dup}


def is_loop_rel(rel):
    """B 段是循环体 —— 导出时整段挂 loop_back, 圈数交给固件。"""
    return parse_graph_path(rel)[3] == 'B_Loop'


def analyze_frames(root, rel, want_uris=True):
    """读一个叶子目录并转换全部帧, 附空帧/合并/折叠标记。"""
    info = read_dir(root, rel)
    d = os.path.join(root, rel.replace('/', os.sep))
    seen = {}          # 像素摘要 -> 首次出现的帧序号
    raw = []
    for i, t in enumerate(info['frames']):
        blob, st = convert_one(os.path.join(d, t['file']))
        key = hashlib.blake2b(blob, digest_size=16).digest()
        if st == 'ok' and key not in seen:
            seen[key] = i
        raw.append({
            'i': i, 'file': t['file'], 'ms': t['ms'] or DEFAULT_MS,
            'ms_from_name': t['ms'] is not None,
            'idx': t['idx'], 'note': t['note'],
            'blank': st == 'blank', 'bad': st == 'bad',
            '_blob': blob, '_key': key,
        })

    plan = build_plan(raw, [r['_key'] for r in raw])
    grp, dup = plan['grp'], plan['dup']

    frames = []
    for r in raw:
        blob, key = r.pop('_blob'), r.pop('_key')
        if r['blank'] or r['bad']:
            r['plan'], r['ref'] = ('bad' if r['bad'] else 'blank'), None
        elif grp.get(r['i']) != r['i']:
            r['plan'], r['ref'] = 'merge', grp[r['i']]
        elif r['i'] in dup:
            r['plan'], r['ref'] = 'dup', dup[r['i']]
        else:
            r['plan'], r['ref'] = 'keep', None
        # 合并组首帧要替整组承担时长
        r['keep_ms'] = sum(x['ms'] for x in raw
                           if grp.get(x['i']) == r['i']) if r['plan'] == 'keep' else r['ms']
        r['grp'] = grp.get(r['i'])
        r['keep'] = r['plan'] == 'keep'
        r['uri'] = to_data_uri(blob) if want_uris else None
        r['bytes'] = len(blob)
        frames.append(r)
    return {'rel': rel, 'count': len(frames), 'frames': frames,
            'contiguous': info['contiguous'], 'layered': info['layered'],
            'no_ms': info['no_ms'], 'no_idx': info['no_idx'],
            'unique': len(seen), 'animat': parse_graph_path(rel)[3],
            'bad': sum(1 for f in frames if f['plan'] == 'bad'),
            'kept': sum(1 for f in frames if f['keep'])}


# ══════════ 导出 ══════════

def build_seq(plan, keep_idx, loop=False):
    """勾选集 → 播放序列 [{i, grp, ms, lb}]。

    合并组的时长合计压在组首帧上。loop 时在末帧挂 loop_back = 序列长度-1, 让固件
    把整段当循环体重复播放 (圈数看 s_play_loops); 只剩 1 帧就挂不上, 当线性播。
    """
    by_i = {f['i']: f for f in plan['frames']}
    kept = [i for i in keep_idx
            if i in by_i and by_i[i]['plan'] not in ('blank', 'bad')]
    ks = set(kept)
    # 组首帧 (= grp 指向自己) 恒留下; 组成员只在组首没被保留时才自己出面
    use = [i for i in kept
           if by_i[i]['grp'] == i or by_i[i]['grp'] not in ks]

    seq = []
    for i in use:
        f = by_i[i]
        ms = f['keep_ms'] if f['plan'] == 'keep' else f['ms']
        if seq and seq[-1]['grp'] == f['grp']:
            seq[-1]['ms'] += ms
        else:
            seq.append({'i': i, 'grp': f['grp'], 'ms': ms, 'lb': 0})

    if loop and len(seq) > 1:
        seq[-1]['lb'] = len(seq) - 1
    return seq


def seq_entry(prefix, seq):
    """播放序列 → anim_meta.json 条目草稿。"""
    ms = [e['ms'] for e in seq]
    med = sorted(ms)[len(ms) // 2] if ms else DEFAULT_MS
    return {
        'prefix': prefix, 'count': len(seq),
        'fps': max(1, round(1000 / med)) if med else 8,
        'frames': [dict({'dur': e['ms']},
                        **({'loop_back': e['lb']} if e['lb'] else {}))
                   for e in seq],
    }


def export_sections(root, sections, prefix, out_dir):
    """多目录合成一条序列 → (写入清单, anim_meta 条目草稿)。

    sections: [{'rel':..., 'keep':[...], 'plan':分析结果|None}, ...], 调用方按
    A→B→C 排好; 单段就是普通的独立动画。每段各自去重 (loop_back 只落在自己段内,
    见 is_loop_rel), 段间不合并同像素帧 —— 否则边界时长会被吃掉。
    """
    os.makedirs(out_dir, exist_ok=True)
    written, live = [], []
    for s in sections:
        plan = s.get('plan') or analyze_frames(root, s['rel'], want_uris=False)
        by_i = {f['i']: f for f in plan['frames']}
        d = os.path.join(root, s['rel'].replace('/', os.sep))
        for e in build_seq(plan, s.get('keep') or [], is_loop_rel(s['rel'])):
            blob, st = convert_one(os.path.join(d, by_i[e['i']]['file']))
            if st != 'ok':
                continue
            name = f'{prefix}_{len(written):02d}.bin'
            with open(os.path.join(out_dir, name), 'wb') as fh:
                fh.write(blob)
            written.append(name)
            live.append(e)
    return written, seq_entry(prefix, live)


# ══════════ Win32 对话框 ══════════

def win32_pick_dir(title):
    """选择目录 (SHBrowseForFolderW)。"""
    import ctypes
    from ctypes import wintypes

    class BROWSEINFOW(ctypes.Structure):
        _fields_ = [('hwndOwner', wintypes.HWND), ('pidlRoot', ctypes.c_void_p),
                    ('pszDisplayName', wintypes.LPWSTR), ('lpszTitle', wintypes.LPCWSTR),
                    ('ulFlags', wintypes.UINT), ('lpfn', ctypes.c_void_p),
                    ('lParam', wintypes.LPARAM), ('iImage', ctypes.c_int)]

    buf = ctypes.create_unicode_buffer(260)
    bi = BROWSEINFOW()
    bi.pszDisplayName = buf
    bi.lpszTitle = title
    bi.ulFlags = 0x00000001 | 0x00000040  # RETURNONLYFSDIRS | EDITBOX
    pidl = ctypes.windll.shell32.SHBrowseForFolderW(ctypes.byref(bi))
    if not pidl:
        return None
    path = ctypes.create_unicode_buffer(260)
    ctypes.windll.shell32.SHGetPathFromIDListW(pidl, path)
    ctypes.windll.ole32.CoTaskMemFree(pidl)
    return path.value or None


# ══════════ 前端 API ══════════

class Api:
    def __init__(self):
        self.project_home = os.path.abspath(
            os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
        self.root = None
        self.plans = {}        # rel -> analyze_frames 结果 (含帧 URI), 换段不重转

    def default_root(self):
        saved = load_cfg().get('root')
        for p in [saved] + steam_vpet_roots():
            if p and os.path.isdir(p) and find_leaves(p):
                return p
        return ''

    def pick_root(self):
        p = win32_pick_dir('选择 VPet 的 pet 目录 (含 vup/ 的那一层)')
        if not p:
            return {'ok': False, 'error': '已取消'}
        # 允许用户选到 pet/ 或 pet/vup/ 或 mod/0000_core/
        for cand in (p, os.path.join(p, 'vup')):
            if os.path.isdir(cand) and find_leaves(cand):
                self.root = cand
                save_cfg(root=cand)
                try:
                    return {'ok': True, 'root': cand, 'info': scan_tree(cand)}
                except OSError as e:
                    return {'ok': False, 'error': str(e)}
        return {'ok': False, 'error': f'{p} 下没找到含 PNG 的动作目录'}

    def scan(self, root=None):
        root = root or self.root
        if not root or not os.path.isdir(root):
            return {'ok': False, 'error': 'VPet 目录无效'}
        self.root = root
        save_cfg(root=root)
        try:
            return {'ok': True, 'scan': scan_tree(root)}
        except OSError as e:
            return {'ok': False, 'error': str(e)}

    def groups(self):
        """全库 → 动画组 (组 = GraphType+Name; 心情/段/变体是组内层次)。"""
        if not self.root:
            return {'ok': False, 'error': '尚未选择 VPet 目录'}
        try:
            return {'ok': True, 'groups': build_groups(self.root)}
        except OSError as e:
            return {'ok': False, 'error': str(e)}

    def load_dir(self, rel):
        if not self.root:
            return {'ok': False, 'error': '尚未选择 VPet 目录'}
        try:
            self.plans[rel] = analyze_frames(self.root, rel)
        except OSError as e:
            return {'ok': False, 'error': str(e)}
        return {'ok': True, 'dir': self.plans[rel]}

    def _plan(self, rel):
        """已装载就用缓存 —— 一个目录转换要几百毫秒起, 预览每次改勾选都得复用。"""
        if rel not in self.plans:
            self.plans[rel] = analyze_frames(self.root, rel, want_uris=False)
        return self.plans[rel]

    def seq_group(self, sections, prefix):
        """合成预览。sections: [{'rel', 'keep'}...], 按 A→B→C 排好。"""
        if not self.root:
            return {'ok': False, 'error': '尚未选择 VPet 目录'}
        out = []
        try:
            for n, s in enumerate(sections or []):
                if not s.get('rel'):
                    continue
                seq = build_seq(self._plan(s['rel']), s.get('keep') or [],
                                is_loop_rel(s['rel']))
                out += [dict(e, s=n) for e in seq]
        except OSError as e:
            return {'ok': False, 'error': str(e)}
        return {'ok': True, 'seq': out, 'n': len(out),
                'entry': seq_entry(prefix or 'x', out)}

    def export_group(self, sections, prefix):
        """多段合成一条动画。sections: [{'rel', 'keep'}...], 按 A→B→C 排好。"""
        if not self.root:
            return {'ok': False, 'error': '尚未选择 VPet 目录'}
        prefix = re.sub(r'[^A-Za-z0-9_]', '', prefix or '')
        if not prefix:
            return {'ok': False, 'error': '前缀不能为空 (仅字母数字下划线)'}
        secs = [dict(s, plan=self.plans.get(s['rel']))
                for s in (sections or []) if s.get('rel')]
        if not secs:
            return {'ok': False, 'error': '没有选中的段'}
        out_dir = os.path.join(self.project_home, 'assets', 'anim_bin')
        try:
            written, entry = export_sections(self.root, secs, prefix, out_dir)
        except OSError as e:
            return {'ok': False, 'error': str(e)}
        if not written:
            return {'ok': False, 'error': '没有可导出的帧 (全被剔除或被判空帧)'}
        return {'ok': True, 'dir': out_dir, 'files': written,
                'entry': entry, 'n': len(written)}

    def open_out_dir(self):
        d = os.path.join(self.project_home, 'assets', 'anim_bin')
        if os.path.isdir(d):
            os.startfile(d)
            return {'ok': True}
        return {'ok': False, 'error': '目录还不存在'}


def main():
    html = os.path.join(
        sys._MEIPASS if getattr(sys, 'frozen', False) else
        os.path.dirname(os.path.abspath(__file__)), 'vpet_import.html')
    if webview is None:
        print('缺少 pywebview (pip install pywebview)')
        return
    api = Api()
    webview.create_window('VPet 素材导入器', html, js_api=api,
                          width=1500, height=950, background_color='#14171b')
    webview.start()


if __name__ == '__main__':
    main()
