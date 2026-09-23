#!/usr/bin/env python3
"""镜像来源解析与写集合构建 — 决定"烧什么、烧到哪", 是唯一的偏移来源。

用法 (库; 命令行入口见 app.py):
    from images import load
    imgs = load()                 # 自动定位镜像目录
    print(imgs.report())
    ops  = imgs.ops(mode="app", with_assets=False, dual_slot=True)
    imgs.assert_safe(ops)         # 写集合越界/撞保留区 → 抛 UnsafeImageSet

别人拿到分发包后, 把固件丢进 images/ (或直接丢在 exe 旁边) 即可用 ——
角色**先按文件名定** (Virtualpet.bin=主程序 / bootloader.bin / partition-table.bin /
assets.bin / ota_data_initial.bin), 名字认不出的再按内容识别。两者冲突则拒绝使用该文件,
不会默默烧错地方。

偏移以**即将烧写的分区表**为准, 而不是构建时的 flasher_args.json 快照
(对方可能只换了 app, 也可能连分区表一起换)。
"""
import json
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

FLASH_SIZE = 0x2000000  # W25Q256 32MB
APP_MAGIC = 0xABCD5432  # esp_app_desc_t 魔数 (小端读 0x20 处)
PTABLE_MAGIC = 0x50AA
ESP_IMAGE_MAGIC = 0xE9
PTABLE_OFF = 0x8000     # 由 ESP32-S3 boot ROM 固定
BOOTLOADER_OFF = 0x0    # 同上
APP_DESC_OFF = 0x20     # 镜像头 24B + 首段头 8B
APP_VERSION_OFF = 0x30
APP_NAME_OFF = 0x50
IDF_VER_OFF = 0x90

# 允许写的分区标签; 其余一律拒绝 (nvs=WiFi凭据/Token, config=宠物记忆/设置,
# data=日记, phy_init=射频校准, nvskey=NVS加密密钥)
WRITABLE = {"bootloader", "partition-table", "otadata", "ota_0", "ota_1", "assets"}
NEVER_WRITE = {"nvs", "phy_init", "nvskey", "config", "data"}

# 分区表读不出来时的兜底 (与仓库 partitions.csv 一致)
FALLBACK_PARTITIONS = {
    "nvs": (0x9000, 0x6000), "otadata": (0xF000, 0x2000), "phy_init": (0x11000, 0x1000),
    "ota_0": (0x20000, 0x200000), "ota_1": (0x220000, 0x200000),
    "nvskey": (0x420000, 0x4000), "config": (0x424000, 0x80000),
    "data": (0x4A4000, 0x100000), "assets": (0x5A4000, 0x1A5C000),
}

KIND_LABEL = {
    "app": "主程序", "bootloader": "bootloader", "partition-table": "分区表",
    "otadata": "OTA 状态", "assets": "资源包",
}

# 文件名 → 角色。精确名优先 (换固件时通常同名替换), 其次子串匹配。
EXACT_NAMES = {
    "virtualpet.bin": "app",
    "bootloader.bin": "bootloader",
    "partition-table.bin": "partition-table",
    "partition_table.bin": "partition-table",
    "ota_data_initial.bin": "otadata",
    "assets.bin": "assets",
}
NAME_KEYS = (
    ("virtualpet", "app"), ("firmware", "app"), ("固件", "app"),
    ("bootloader", "bootloader"),
    ("partition-table", "partition-table"), ("partition_table", "partition-table"), ("分区表", "partition-table"),
    ("ota_data", "otadata"), ("otadata", "otadata"),
    ("assets", "assets"), ("资源", "assets"),
)


class UnsafeImageSet(Exception):
    """写集合不安全: 撞保留区 / 超片 / 缺关键镜像。"""


@dataclass
class Image:
    kind: str
    path: Path
    size: int
    version: str = ""
    project: str = ""
    idf_ver: str = ""

    @property
    def name(self) -> str:
        return self.path.name


@dataclass
class Partition:
    label: str
    offset: int
    size: int
    type: int = 0
    subtype: int = 0


@dataclass
class Op:
    """一个烧录动作: 把 src 写到 offset。label 用于判定表展示与安全断言。"""
    label: str
    offset: int
    src: Path

    @property
    def size(self) -> int:
        return self.src.stat().st_size


@dataclass
class ImageSet:
    root: Path
    source: str                                   # "build(开发)" / "images(分发)" / 显式目录
    partitions: dict = field(default_factory=dict)
    images: dict = field(default_factory=dict)     # kind -> Image
    warnings: list = field(default_factory=list)
    notes: list = field(default_factory=list)
    rejected: dict = field(default_factory=dict)   # 文件名认出的角色 -> 被拒的文件 (名实不符)

    # ── 展示 ──
    @property
    def app(self):
        return self.images.get("app")

    @property
    def version(self) -> str:
        return self.app.version if self.app else ""

    def part(self, label: str):
        p = self.partitions.get(label)
        if p:
            return p
        off, size = FALLBACK_PARTITIONS.get(label, (None, None))
        return Partition(label, off, size) if off is not None else None

    def report(self) -> str:
        lines = ["镜像来源: %s  (%s)" % (self.root, self.source)]
        if self.version:
            lines.append("固件版本: %s   (%s)" % (self.version, self.app.project or "?"))
        for w in self.warnings:
            lines.append("WARN  " + w)
        for n in self.notes:
            lines.append("      " + n)
        return "\n".join(lines)

    def describe_ops(self, ops) -> str:
        out = []
        for op in ops:
            out.append("0x%-8x %-14s %-28s %9d B" %
                       (op.offset, op.label, op.src.name, op.size))
        return "\n".join(out)

    # ── 写集合 ──
    def ops(self, mode: str = "app", with_assets: bool = False,
            dual_slot: bool = True, extra=None) -> list:
        """mode: app = 只烧主程序 / full = 连 bootloader 分区表一起。"""
        ops = []
        if mode == "full":
            if "bootloader" in self.images:
                ops.append(Op("bootloader", BOOTLOADER_OFF, self.images["bootloader"].path))
            if "partition-table" in self.images:
                ops.append(Op("partition-table", PTABLE_OFF, self.images["partition-table"].path))
            if "otadata" in self.images:
                ops.append(Op("otadata", self.part("otadata").offset, self.images["otadata"].path))

        app = self.app
        if app:
            ota0 = self.part("ota_0")
            ops.append(Op("ota_0", ota0.offset, app.path))
            if dual_slot:
                ota1 = self.part("ota_1")
                if ota1 and ota1.offset:
                    ops.append(Op("ota_1", ota1.offset, app.path))

        if with_assets:
            if "assets" not in self.images:
                # 点名要资源包却没有 — 绝不能静默降级成"只烧主程序",
                # 那会让人以为动画音效已就位, 实际 0x5A4000 一个字节没写
                bad = self.rejected.get("assets")
                if bad:
                    why = "「%s」内容与文件名不符, 已拒绝使用" % bad.name
                else:
                    why = "没有找到 assets.bin"
                raise UnsafeImageSet(
                    "要烧资源包却%s (%s)。\n"
                    "把正确的 assets.bin 放进去, 或去掉 --assets (界面取消勾选)"
                    % (why, self.root))
            ops.append(Op("assets", self.part("assets").offset, self.images["assets"].path))
        for op in (extra or []):
            ops.append(op)
        return ops

    def assert_safe(self, ops):
        """写集合硬门槛 — 撞保留区/超片/尺寸不符即拒绝。"""
        for label, (off, size) in FALLBACK_PARTITIONS.items():
            if label not in NEVER_WRITE:
                continue
            for op in ops:
                if op.offset < off + size and off < op.offset + op.size:
                    raise UnsafeImageSet(
                        "写集合与保留分区 %s (0x%x..0x%x) 相交: %s @0x%x" %
                        (label, off, off + size, op.label, op.offset))
        for op in ops:
            if op.label not in WRITABLE:
                raise UnsafeImageSet("分区 %s 不在可写白名单" % op.label)
            if op.offset + op.size > FLASH_SIZE:
                raise UnsafeImageSet("0x%x + %d 超出 32MB flash" % (op.offset, op.size))
            if op.label.startswith("ota_") and op.size > self.part("ota_0").size:
                raise UnsafeImageSet(
                    "主程序 %d B 超过 ota 槽容量 %d B, 装不进去" %
                    (op.size, self.part("ota_0").size))
        return True

    def app_fits(self) -> bool:
        a = self.app
        return bool(a) and a.size <= self.part("ota_0").size


# ══════════════════════════════════════════════════════════════════════
# 内容识别
# ══════════════════════════════════════════════════════════════════════
def _read_head(path: Path, n: int = 0x100) -> bytes:
    with open(path, "rb") as f:
        return f.read(n)


def identify(path: Path) -> str:
    """按内容判断这是哪种镜像; 认不出返回 'unknown'。

    判据 (实测 build/ 产物):
      app             0xE9 开头 + 0x20 处 0xABCD5432 (esp_app_desc_t)
      bootloader      0xE9 开头, 无 app 描述符
      partition-table 0x50AA 开头 (32B/条)
      assets          LittleFS 镜像, 偏移 8 处 "littlefs"
      otadata         全 0xFF (写进去等价于清空 = 引导 ota_0)
    """
    try:
        head = _read_head(path)
    except OSError:
        return "unknown"
    if len(head) < 16:
        return "unknown"
    if len(head) >= APP_DESC_OFF + 4 and struct.unpack_from("<I", head, APP_DESC_OFF)[0] == APP_MAGIC:
        return "app"
    if struct.unpack_from("<H", head, 0)[0] == PTABLE_MAGIC:
        return "partition-table"
    if b"littlefs" in head[:16]:
        return "assets"
    if head[0] == ESP_IMAGE_MAGIC:
        return "bootloader"
    if all(b == 0xFF for b in head):
        return "otadata"
    return "unknown"


def role_from_name(path: Path) -> str:
    """按文件名判角色; 认不出返回 ''。"""
    low = path.name.lower()
    if low in EXACT_NAMES:
        return EXACT_NAMES[low]
    stem = low.replace(" ", "").replace("-", "_")
    for key, kind in NAME_KEYS:
        if key in stem:
            return kind
    return ""


def role_of(path: Path) -> tuple:
    """(角色, 依据)。文件名与内容冲突 → 抛 UnsafeImageSet (宁可不烧, 也不烧错地方)。"""
    by_name, by_content = role_from_name(path), identify(path)
    if by_name and by_content not in ("unknown", "") and by_name != by_content:
        raise UnsafeImageSet(
            "%s 的文件名说是「%s」, 内容却是「%s」— 拒绝使用, 请核对这个文件" %
            (path.name, KIND_LABEL.get(by_name, by_name), KIND_LABEL.get(by_content, by_content)))
    if by_name:
        return by_name, "文件名"
    if by_content != "unknown":
        return by_content, "内容识别"
    return "", ""


def parse_app_desc(path: Path) -> dict:
    """从 app 镜像里读版本/项目名 — 版本号的权威来源, 不依赖别的产物。"""
    head = _read_head(path, IDF_VER_OFF + 32)
    if len(head) < APP_DESC_OFF + 4 or struct.unpack_from("<I", head, APP_DESC_OFF)[0] != APP_MAGIC:
        return {}

    def s(off):
        return head[off:off + 32].split(b"\x00")[0].decode("utf-8", "replace")

    return {"version": s(APP_VERSION_OFF), "project": s(APP_NAME_OFF), "idf_ver": s(IDF_VER_OFF)}


def parse_partition_table(path: Path) -> dict:
    """解析分区表二进制 → {label: Partition}; 遇到非 0x50AA 即停 (尾部填充)。"""
    data = path.read_bytes()
    out = {}
    for i in range(len(data) // 32):
        o = i * 32
        magic, typ, sub, off, size = struct.unpack_from("<HBBII", data, o)
        if magic != PTABLE_MAGIC:
            break
        raw = data[o + 12:o + 28]
        if b"\x00" not in raw:
            break
        label = raw.split(b"\x00")[0].decode("utf-8", "replace")
        if not label or not label.isprintable():
            break
        out[label] = Partition(label, off, size, typ, sub)
    return out


# ══════════════════════════════════════════════════════════════════════
# 定位
# ══════════════════════════════════════════════════════════════════════
def _frozen() -> bool:
    return bool(getattr(sys, "frozen", False))


def _exe_dir() -> Path:
    """冻结后是 exe 所在目录 (images/ 是外置资源, 不进 exe), 未冻结是本工具目录。"""
    if _frozen():
        return Path(sys.executable).resolve().parent
    return Path(__file__).resolve().parent


def _repo_build() -> Path:
    return Path(__file__).resolve().parents[2] / "build"


def _scan(root: Path) -> list:
    """扫 root 与其一级子目录里的 *.bin (支持 images/bootloader/x.bin 布局与随手丢的文件)。"""
    found = []
    if not root.is_dir():
        return found
    for entry in sorted(root.iterdir()):
        if entry.is_file() and entry.suffix.lower() == ".bin":
            found.append(entry)
        elif entry.is_dir() and entry.name not in ("_internal", "__pycache__"):
            for sub in sorted(entry.iterdir()):
                if sub.is_file() and sub.suffix.lower() == ".bin":
                    found.append(sub)
    return found


def _candidate_dirs(bin_dir=None) -> list:
    if bin_dir:
        return [(Path(bin_dir).resolve(), "指定目录")]
    out = []
    ed = _exe_dir()
    if _frozen():
        out.append((ed / "images", "images(分发)"))
        out.append((ed, "exe 同级"))
    out.append((_repo_build(), "build(开发)"))
    if not _frozen():
        out.append((ed / "images", "images(本地)"))
    return out


def load(bin_dir=None, verbose: bool = False) -> ImageSet:
    """定位镜像目录 → 识别 → 构建 ImageSet。全部候选目录都没有可用镜像 → UnsafeImageSet。"""
    tried = []
    all_conflicts = []
    for root, tag in _candidate_dirs(bin_dir):
        files = _scan(root)
        if not files:
            tried.append(str(root))
            continue

        by_kind = {}
        conflicts = []
        rejected = {}
        for f in files:
            try:
                kind, how = role_of(f)
            except UnsafeImageSet as e:
                conflicts.append(str(e))
                # 记下文件名自称的角色 — 后面要资源包却找不到时, 报错得说清
                # "你放的 assets.bin 内容不对" 而不是 "没有 assets.bin"
                rejected.setdefault(role_from_name(f), f)
                continue
            if not kind:
                continue
            by_kind.setdefault(kind, []).append(f)
        if "app" not in by_kind and "partition-table" not in by_kind:
            tried.append("%s (无 app/分区表)" % root)
            all_conflicts += conflicts
            continue

        imgs = {}
        warns = list(conflicts)
        notes = []
        for kind, paths in by_kind.items():
            # 多份候选时: 精确同名 > 在规范子目录 > 最新修改
            def rank(p, kind=kind):
                exact = 0 if EXACT_NAMES.get(p.name.lower()) == kind else 1
                return (exact, p.parent != root, -p.stat().st_mtime)

            paths.sort(key=rank)
            chosen = paths[0]
            if len(paths) > 1:
                why = "精确同名" if EXACT_NAMES.get(chosen.name.lower()) == kind else "最新修改"
                warns.append("%s 有 %d 个候选, 按「%s」选了 %s (其余: %s)" % (
                    KIND_LABEL.get(kind, kind), len(paths), why,
                    chosen.relative_to(root), ", ".join(p.name for p in paths[1:])))
            info = parse_app_desc(chosen) if kind == "app" else {}
            imgs[kind] = Image(kind, chosen, chosen.stat().st_size, **info)

        imgs_by_kind = imgs
        # 偏移来源: 即将烧写的分区表 > flasher_args.json > 兜底表
        parts = {}
        ptable_img = imgs_by_kind.get("partition-table")
        if ptable_img:
            parts = parse_partition_table(ptable_img.path)
            if parts:
                notes.append("偏移取自待烧写的分区表 (%d 个分区)" % len(parts))
        if not parts:
            recipe = root / "flasher_args.json"
            if recipe.exists():
                try:
                    parts = _parts_from_recipe(json.loads(recipe.read_text("utf-8")))
                    notes.append("偏移取自 flasher_args.json")
                except Exception as e:  # noqa: BLE001  坏 json 不该拦住烧录
                    warns.append("flasher_args.json 解析失败 (%s), 改用兜底偏移表" % e)
        if not parts:
            parts = {k: Partition(k, o, s) for k, (o, s) in FALLBACK_PARTITIONS.items()}
            warns.append("未找到分区表与 flasher_args.json, 使用内置偏移表 — 若固件改过分区请核对")

        iss = ImageSet(root=root, source=tag, partitions=parts, images=imgs_by_kind,
                       warnings=warns, notes=notes, rejected=rejected)
        _sanity(iss)
        return iss
    msg = ""
    if all_conflicts:
        msg = "文件名与文件内容对不上, 已跳过后仍然没有可烧的镜像:\n  " + \
              "\n  ".join(all_conflicts) + "\n\n"
    msg += ("没找到可用镜像。找过:\n  " + "\n  ".join(tried) +
            "\n把 Virtualpet.bin (要和 bootloader/分区表/assets 一起烧就都放进来) 放进 images/ 即可")
    raise UnsafeImageSet(msg)


def _parts_from_recipe(recipe: dict) -> dict:
    """从 flasher_args.json 的 flash_files 反推分区 (仅用于兜底, 信息比分区表少)。"""
    parts = {}
    for off_s, name in recipe.get("flash_files", {}).items():
        off = int(off_s, 16)
        for label, (fo, fs) in FALLBACK_PARTITIONS.items():
            if fo == off:
                parts[label] = Partition(label, fo, fs)
    for label in ("bootloader", "partition-table"):
        parts.pop(label, None)
    if "otadata" in parts and "ota_0" not in parts:
        parts["ota_0"] = Partition("ota_0", FALLBACK_PARTITIONS["ota_0"][0],
                                   FALLBACK_PARTITIONS["ota_0"][1])
    return parts


def _sanity(iss: ImageSet):
    """交叉核对与提醒 — 只 WARN, 不拦 (拦的活归 assert_safe)。"""
    app = iss.app
    if app:
        if app.project and app.project != "Virtualpet":
            iss.warnings.append("镜像项目名是 %r, 不是 Virtualpet — 确认没拿错文件" % app.project)
        ota0 = iss.part("ota_0")
        if ota0 and app.size > ota0.size:
            iss.warnings.append("主程序 %d B 超过 ota 槽 %d B, 装不进去" % (app.size, ota0.size))
    if "assets" not in iss.images:
        iss.notes.append("未发现 assets.bin (资源包) — 只烧主程序")
    if "bootloader" not in iss.images:
        iss.notes.append("未发现 bootloader.bin — 只能烧 app (全烧模式会跳过它)")


def verify_version(iss: ImageSet, repo_version_file=None) -> list:
    """版本交叉校验: 镜像头 (权威) vs version.txt。返回 WARN 列表。"""
    warns = []
    app = iss.app
    if not app or not app.version:
        return warns
    cands = [Path(repo_version_file)] if repo_version_file else [
        Path(__file__).resolve().parents[2] / "version.txt",   # 仓库根
        _exe_dir() / "version.txt",                            # 分发包根 (与 images/ 同级)
    ]
    repo_v = ""
    for vf in cands:
        try:
            repo_v = vf.read_text("utf-8").strip()
            break
        except OSError:
            continue
    if repo_v and repo_v != app.version:
        warns.append("镜像里的版本 %s ≠ version.txt 的 %s — "
                     "要么改了代码忘了递增版本, 要么改了 version.txt 没重新构建" %
                     (app.version, repo_v))
    return warns
