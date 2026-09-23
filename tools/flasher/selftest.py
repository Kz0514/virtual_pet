#!/usr/bin/env python3
"""不接板子的自检 — 证明"这个包是好的"。

    python app.py --selftest            # 开发机
    VirtualpetFlasher.exe --selftest    # 冻结包 (外发前必跑这一条)

这是唯一能在没插板的情况下暴露"冻结后找不到 esptool stub / 找不到端口枚举"的检查,
所以打包脚本的最后一步会强制跑它。

判定词与其他工具一致: PASS / FAIL / WARN / SKIP。有 FAIL 返回 1, 只有 WARN 返回 0。
"""
import re
import struct
import sys
from pathlib import Path

import device
import engine
import images

# 进度解析的固定样例 — esptool 输出格式一变这里先红, 而不是等到烧录现场
PROG_SAMPLES = [
    ("Writing [==========          ]  45.0%", "Writing", 0.45),
    ("\033[KWriting [====                ]  12.5%", "Writing", 0.125),
    ("\r\033[KReading [==============      ]  72.0%", "Reading", 0.72),
    ("Erasing [====================] 100.0%", "Erasing", 1.0),
    ("Writing at 0x00010000... (45 %)", None, None),      # v4 语法: 不认, 降级成日志
    ("Wrote 4096 bytes at 0x00010000", None, None),
    ("Hash of data verified.", None, None),
]
WROTE_SAMPLE = "Wrote 4096 bytes at 0x00010000 in 0.3 seconds..."


class Report:
    def __init__(self, verbose=False):
        self.rows = []
        self.verbose = verbose

    def add(self, name, status, detail=""):
        self.rows.append((name, status, detail))
        print("%-5s %-18s %s" % (status, name, detail), flush=True)

    def note(self, text):
        if self.verbose:
            print("      " + text, flush=True)

    @property
    def failed(self):
        return [r for r in self.rows if r[1] == "FAIL"]

    def summary(self) -> int:
        n = {"PASS": 0, "FAIL": 0, "WARN": 0, "SKIP": 0}
        for _name, st, _d in self.rows:
            n[st] = n.get(st, 0) + 1
        print("\n>>> 自检汇总: PASS %d / FAIL %d / WARN %d / SKIP %d"
              % (n["PASS"], n["FAIL"], n["WARN"], n["SKIP"]))
        for name, _st, detail in self.failed:
            print("    FAIL %s — %s" % (name, detail))
        return 1 if self.failed else 0


# ══════════════════════════════════════════════════════════════════════
# 单项检查
# ══════════════════════════════════════════════════════════════════════
def check_runtime(rep: Report):
    frozen = bool(getattr(sys, "frozen", False))
    rep.add("运行环境", "PASS", "Python %s%s" % (sys.version.split()[0],
                                               " · 冻结包" if frozen else " · 源码"))
    if frozen:
        # 外置 images/ 才是资源目录 — 不能用 _MEIPASS (那是解包的内部目录)
        exe_dir = Path(sys.executable).resolve().parent
        rep.note("exe 目录: %s" % exe_dir)
        rep.add("镜像目录 exe 同级", "PASS" if (exe_dir / "images").is_dir() else "WARN",
                str(exe_dir / "images") + ("" if (exe_dir / "images").is_dir()
                                           else " — 没有, 只能靠 --bin-dir 指定"))


def check_serial(rep: Report):
    try:
        import serial  # noqa: F401
        import serial.tools.list_ports  # noqa: F401
    except Exception as e:  # noqa: BLE001
        rep.add("pyserial", "FAIL", "导入失败: %s" % e)
        return
    try:
        rows = device.list_ports()
    except Exception as e:  # noqa: BLE001
        rep.add("端口枚举", "FAIL", "无板时也不该抛异常: %s" % e)
        return
    flash = [d for d, _v, _p, r in rows if r == "flashable"]
    rep.add("端口枚举", "PASS", "共 %d 个串口, 可烧录 %d 个%s"
            % (len(rows), len(flash), (" — " + ", ".join(flash)) if flash else " (没插板属正常)"))


def check_esptool(rep: Report):
    """版本 + stub 数据 + 真跑一次子进程 (端到端)。"""
    try:
        import esptool
    except Exception as e:  # noqa: BLE001
        rep.add("esptool 导入", "FAIL", str(e))
        return
    ver = getattr(esptool, "__version__", "") or "?"
    major = ver.split(".")[0]
    rep.add("esptool 版本", "PASS" if major == "5" else "FAIL",
            "%s (本工具按 v5 短横线语法调用)" % ver)

    # stub 是包内数据文件 — PyInstaller 少了 --collect-data 就会在真烧时崩, 这里提前红
    root = Path(esptool.__file__).resolve().parent
    stubs = sorted(root.glob("targets/stub_flasher/*/*.json"))
    rep.add("stub 数据文件", "PASS" if stubs else "FAIL",
            "%d 个 @ %s%s" % (len(stubs), root,
                              "" if stubs else " — 打包少了 --collect-data esptool"))

    # 真跑一次: 证明子进程调用路径在冻结后也通 (version 不需要端口)
    try:
        rc, lines = engine.run_esptool(engine._base_cmd() + ["version"], engine._Quiet(),
                                       timeout=60)
    except Exception as e:  # noqa: BLE001
        rep.add("esptool 子进程", "FAIL", "起不来: %s" % e)
        return
    blob = "\n".join(lines)
    m = re.search(r"esptool(?:\.py)? v?(\S+)", blob)
    rep.add("esptool 子进程", "PASS" if rc == 0 else "FAIL",
            "rc=%d %s" % (rc, ("v" + m.group(1)) if m else blob[:80]))


def check_progress_regex(rep: Report):
    bad = []
    for line, what, frac in PROG_SAMPLES:
        m = engine.PROG_RE.match(engine.ANSI_RE.sub("", line).strip())
        if what is None:
            if m:
                bad.append("%r 不该被认成进度" % line)
            continue
        if not m or m.group("what") != what or abs(float(m.group("pct")) / 100 - frac) > 1e-9:
            bad.append("%r 解析错" % line)
    m = engine.WROTE_RE.match(WROTE_SAMPLE)
    if not (m and m.group(1) == "4096" and m.group(2) == "0x00010000"):
        bad.append("WROTE_RE 认不出已写字节数")
    rep.add("进度正则自测", "FAIL" if bad else "PASS",
            "; ".join(bad) if bad else "%d 条样例" % len(PROG_SAMPLES))


def check_classify(rep: Report):
    """失败分类的关键词判定 — 判错会让卡死的板子被无限重试。"""
    cases = [
        (["A fatal error occurred: The chip stopped responding."], True, 0, "power"),
        (["Timed out waiting for packet header"], True, 0, "retry"),
        (["could not open port COM9"], False, 0, "retry"),
        (["(nothing useful)"], True, 2, "power"),        # 口在却连不上两次 → 芯片挂死
        (["A fatal error occurred: Invalid head of packet"], True, 0, "power"),
        (["some other error"], True, 1, "fatal"),
    ]
    bad = []
    for lines, present, attempts, want in cases:
        got = engine.classify(lines, present, attempts)
        if got != want:
            bad.append("%r → %s (期望 %s)" % (lines[0][:32], got, want))
    rep.add("失败分类", "FAIL" if bad else "PASS", "; ".join(bad) if bad else "%d 条样例" % len(cases))


def check_images(rep: Report, bin_dir=None):
    try:
        iss = images.load(bin_dir=bin_dir)
    except Exception as e:  # noqa: BLE001
        rep.add("镜像解析", "FAIL", str(e))
        return None
    rep.add("镜像解析", "PASS", "来源 %s (%s)" % (iss.root, iss.source))
    for w in iss.warnings:
        rep.add("镜像解析", "WARN", w)

    ops = iss.ops(mode="full", with_assets=("assets" in iss.images and _has_assets(iss)))
    print("\n将写入的区域:")
    print("  " + iss.describe_ops(ops).replace("\n", "\n  "))
    print()

    # 硬线复算 — 不信任 assert_safe 内部实现, 这里独立再算一遍
    bad = []
    for op in ops:
        if op.label not in images.WRITABLE:
            bad.append("%s 不在可写白名单" % op.label)
        for label in images.NEVER_WRITE:
            off, size = images.FALLBACK_PARTITIONS[label]
            if op.offset < off + size and off < op.offset + op.size:
                bad.append("%s @0x%x 撞上保留分区 %s" % (op.label, op.offset, label))
        if op.offset + op.size > images.FLASH_SIZE:
            bad.append("%s 超出 32MB" % op.label)
    try:
        iss.assert_safe(ops)
    except Exception as e:  # noqa: BLE001
        bad.append("assert_safe 拒绝: %s" % e)
    rep.add("写集合硬线", "FAIL" if bad else "PASS",
            "; ".join(bad) if bad else "%d 个区域 ∩ 保留区 = ∅, 全在 32MB 内" % len(ops))

    # app 描述符 — 自己读字节, 不复用 images 的解析 (否则它的 bug 会一起自证清白)
    app = iss.app
    if not app:
        rep.add("app 描述符", "FAIL", "没找到主程序镜像 (Virtualpet.bin)")
    else:
        bad = []
        head = app.path.read_bytes()[:0xB0]
        magic = struct.unpack_from("<I", head, 0x20)[0]
        if magic != 0xABCD5432:
            bad.append("0x20 处的 magic 是 0x%08x ≠ 0xABCD5432 — 这不是 ESP 应用镜像" % magic)
        if head[0] != 0xE9:
            bad.append("首字节 0x%02x ≠ 0xE9" % head[0])
        if app.project and app.project != "Virtualpet":
            bad.append("project_name = %r" % app.project)
        if not app.version:
            bad.append("版本号读不出来")
        rep.add("app 描述符", "FAIL" if bad else "PASS",
                "; ".join(bad) if bad else "%s · v%s · %.2f MB"
                % (app.project, app.version, app.size / 1048576.0))

    # 版本交叉校验
    warns = images.verify_version(iss)
    rep.add("版本交叉校验", "WARN" if warns else "PASS",
            warns[0] if warns else "镜像 v%s = version.txt" % iss.version)

    # flasher_args.json 与分区表推导对账
    _crosscheck_partitions(rep, iss)
    return iss


def _has_assets(iss) -> bool:
    try:
        return any(p.name.lower().startswith("assets") for p in iss.root.rglob("assets*.bin"))
    except OSError:
        return False


def _crosscheck_partitions(rep: Report, iss):
    """磁盘上的分区表是真凭据 — 与它不一致说明 flasher_args.json 过期了。"""
    pt = iss.images.get("partition-table")
    if not pt:
        rep.add("分区表对账", "SKIP", "没有 partition-table.bin")
        return
    derived = images.parse_partition_table(pt.path)
    if not derived:
        rep.add("分区表对账", "WARN", "partition-table.bin 解不出条目")
        return
    bad = []
    for label in ("otadata", "ota_0", "ota_1", "assets", "nvs", "config", "data"):
        a, b = iss.part(label), derived.get(label)
        if a and b and (a.offset, a.size) != (b.offset, b.size):
            bad.append("%s: 用的是 0x%x/%d, 分区表里是 0x%x/%d"
                       % (label, a.offset, a.size, b.offset, b.size))
    rep.add("分区表对账", "FAIL" if bad else "PASS",
            "; ".join(bad) if bad else "%d 个分区与 partition-table.bin 逐项一致" % len(derived))


# ══════════════════════════════════════════════════════════════════════
def run(bin_dir=None, verbose=False) -> int:
    print("=== Virtualpet 烧录器自检 (不接板子) ===\n")
    rep = Report(verbose)
    check_runtime(rep)
    check_serial(rep)
    check_esptool(rep)
    check_progress_regex(rep)
    check_classify(rep)
    check_images(rep, bin_dir=bin_dir)
    return rep.summary()


if __name__ == "__main__":
    sys.exit(run())
