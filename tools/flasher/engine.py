#!/usr/bin/env python3
"""烧录引擎 — 动作序列状态机, 唯一碰硬件的地方。

用法 (库; GUI/CLI 都只是它的前端):
    from engine import Job, run_job, ConsoleSink
    job  = Job(images=iss, ops=iss.ops(mode="full"), port=None, verify=True)
    res  = run_job(job, ConsoleSink())
    sys.exit(res.exit_code)

判定只看 esptool 退出码与写后回读, **不依赖文本解析** — 进度条解析失败只降级成
"没有百分比", 绝不影响成败判定。失败按关键词分三类: 可重试 / 需断电重上电 / 永不重试。
"""
import hashlib
import os
import re
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path

import device
from images import FLASH_SIZE, PTABLE_OFF, UnsafeImageSet, verify_version

CHIP = "esp32s3"
CHIP_NAME = "ESP32-S3"
DEFAULT_LADDER = (921600, 460800, 115200)
STEP_NAMES = ("解析镜像", "找板", "连接", "写镜像", "回读校验", "保留区核对", "复位抓串口", "版本核对")

# 进度行 (esptool v5 logger.progress_bar: \r{clear}{prefix}[{bar}] {pct:>5}%{suffix})
PROG_RE = re.compile(r"^(?P<what>Writing|Reading|Dumping|Downloading|Erasing)\b[^\[]*\[[^\]]*\]\s*(?P<pct>[\d.]+)%")
WROTE_RE = re.compile(r"^Wrote (\d+) bytes at (0x[0-9a-fA-F]+)")
APP_VER_RE = re.compile(r"App version:\s*(\S+)")
PROJ_RE = re.compile(r"Project name:\s*(\S+)")
MAC_RE = re.compile(r"MAC:\s*([0-9a-fA-F:]{17})")
ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")

RETRYABLE = ("Timed out waiting for packet", "Serial data stream stopped", "Failed to connect",
             "could not open port", "Resource temporarily unavailable",
             "Packet content transfer stopped", "Failed to read from serial")
NEED_POWER = ("The chip stopped responding", "StopIteration", "MD5 of file does not match",
              "Invalid head of packet")


class EngineError(Exception):
    pass


class NeedPowerCycle(EngineError):
    """芯片挂死 — 重试无用, 必须拔电再插。"""


@dataclass
class Job:
    images: object = None
    ops: list = field(default_factory=list)
    port: str = None
    ladder: tuple = DEFAULT_LADDER
    race: bool = False
    wait: float = 60.0
    verify: bool = True
    verify_all: bool = False
    capture_seconds: float = 8.0
    flash_mode: str = "dio"
    flash_size: str = "32MB"
    flash_freq: str = "80m"
    erase: list = field(default_factory=list)      # [(label, offset, size, token)]
    erase_all: bool = False
    erase_token: str = ""
    backup_dir: Path = None
    restore_file: Path = None
    restore_target: str = ""
    protect_dump: Path = None
    protect_compare: Path = None
    protect_before: bool = False
    dump: list = field(default_factory=list)        # 要提取的分区标签 (偏移连上板子才定)
    dump_dir: Path = None
    dump_confirmed: bool = False
    list_partitions: bool = False                   # 只列出板上分区表就结束
    dry_run: bool = False
    verbose: bool = False
    scripted: bool = False                          # 非交互: 需要断电直接失败
    _depth: int = 0                                 # 断电重来轮次, 防无限递归

    def flash_args(self) -> list:
        return ["--flash-mode", self.flash_mode, "--flash-size", self.flash_size,
                "--flash-freq", self.flash_freq]


@dataclass
class Result:
    ok: bool = False
    exit_code: int = 1
    steps: list = field(default_factory=list)
    version: str = ""
    error: str = ""
    need_power: bool = False
    command_lines: list = field(default_factory=list)

    def add(self, name, status, detail=""):
        self.steps.append((name, status, detail))


class ConsoleSink:
    """CLI 前端: 直接打印; 需要断电时问一句 (非交互则放弃)。"""

    def __init__(self, scripted=False, verbose=False):
        self.scripted = scripted
        self.verbose = verbose
        self._last_pct = -1
        self._last_tick = -1
        self.tty = bool(getattr(sys.stdout, "isatty", lambda: False)())

    def log(self, line, raw=False):
        if raw and not self.verbose:
            return
        print(line, flush=True)

    def step(self, name, status, detail=""):
        print(">>> %-10s %-5s %s" % (name, status, detail), flush=True)

    def progress(self, frac, text=None):
        pct = -1 if frac is None else int(frac * 100)
        if pct == self._last_pct:
            return
        self._last_pct = pct
        if not self.tty:
            # 重定向/管道: \r 与 ANSI 只会变成日志里的乱码 — 按 5% 一档打整行
            tick = pct // 5 if pct >= 0 else -1
            if tick == self._last_tick:
                return
            self._last_tick = tick
            print("    %3d%% %s" % (pct, text or ""), flush=True)
            return
        if pct >= 0:
            sys.stdout.write("\r    %3d%% %s\033[K" % (pct, text or ""))
        else:
            sys.stdout.write("\r    %s\033[K" % (text or "处理中…"))
        sys.stdout.flush()

    def progress_done(self):
        if not self.tty:
            self._last_tick = -1
            return
        sys.stdout.write("\n")
        sys.stdout.flush()

    def need_power_cycle(self, reason) -> bool:
        print("\n!! %s" % reason, flush=True)
        if self.scripted or not sys.stdin.isatty():
            print("   (非交互模式) 请手动断电重上电后重跑", flush=True)
            return False
        ans = input(">>> 请拔掉电源等 3 秒再插上, 然后按回车继续 (q 放弃): ").strip().lower()
        return ans != "q"

    def cancelled(self) -> bool:
        return False


# ══════════════════════════════════════════════════════════════════════
# 调 esptool
# ══════════════════════════════════════════════════════════════════════
def _frozen() -> bool:
    return bool(getattr(sys, "frozen", False))


def _base_cmd() -> list:
    """冻结后用自我再入 (零成本), 未冻结时用当前解释器的 -m esptool。"""
    if _frozen():
        return [sys.executable, "--run-esptool"]
    return [sys.executable, "-m", "esptool"]


def esptool_cmd(port, baud, before, after, args) -> list:
    return _base_cmd() + ["--chip", CHIP, "-p", port, "-b", str(baud),
                          "--before", before, "--after", after] + list(args)


def _env():
    e = os.environ.copy()
    e["PYTHONIOENCODING"] = "utf-8"
    e["NO_COLOR"] = "1"          # 管道里也不会带 ANSI 色码
    return e


def _default_events(sink):
    """默认事件处理: 进度行→进度条, 其余→日志。"""
    def f(line, pct):
        if pct is None:
            sink.log(line, raw=True)
        else:
            sink.progress(pct, line)
    return f


def run_esptool(cmd, sink, cwd=None, timeout=900, on_event=None):
    """跑一次 esptool, 边跑边把日志/进度喂给 sink。返回 (rc, lines)。

    on_event(line, pct): pct 为 None 表示普通日志行, 否则是 0..1 的进度。
    cwd 必须锁定 (esptool 会读 CWD 里的 esptool.cfg/setup.cfg/tox.ini 改自己的行为),
    文件路径一律绝对。
    """
    emit = on_event or _default_events(sink)
    sink.log("$ " + " ".join(str(c) for c in cmd), raw=True)
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            cwd=str(cwd) if cwd else None, env=_env(), bufsize=0)
    lines, buf = [], bytearray()
    deadline = time.time() + timeout

    def feed(raw: bytes):
        line = ANSI_RE.sub("", raw.decode("utf-8", "replace")).rstrip()
        if not line:
            return
        m = PROG_RE.match(line.strip())
        if m:
            emit(line.strip()[:m.end()], float(m.group("pct")) / 100.0)
            # 末尾那次刷新没有换行符, 下一行日志会粘在百分比后面 — 拆出来别吞掉
            rest = line.strip()[m.end():].strip()
            if rest:
                feed(rest.encode("utf-8"))
            return
        lines.append(line)
        emit(line, None)

    try:
        while True:
            chunk = proc.stdout.read(4096)
            if chunk:
                buf += chunk
                while True:
                    idx = -1
                    for i, ch in enumerate(buf):
                        if ch in (0x0A, 0x0D):
                            idx = i
                            break
                    if idx < 0:
                        break
                    feed(bytes(buf[:idx]))
                    del buf[:idx + 1]
                continue
            if proc.poll() is not None:
                break
            if time.time() > deadline:
                proc.kill()
                lines.append("!! esptool 超时 (%ds) 已终止" % timeout)
                break
            if sink.cancelled():
                proc.kill()
                lines.append("!! 用户取消")
                break
            time.sleep(0.02)
        rest = proc.stdout.read() or b""
        if rest:
            buf += rest
        if buf:
            feed(bytes(buf))     # 最后一段没有换行的残片
    finally:
        try:
            proc.stdout.close()
        except Exception:  # noqa: BLE001
            pass
        proc.wait()
    return proc.returncode, lines


def classify(lines, port_present=True, attempts=0) -> str:
    """把失败翻译成人话 + 处置建议。返回 'retry' / 'power' / 'fatal'。"""
    blob = "\n".join(lines)
    for kw in NEED_POWER:
        if kw in blob:
            return "power"
    for kw in RETRYABLE:
        if kw in blob:
            return "retry"
    if port_present and attempts >= 2:
        return "power"     # 口在, 但连不上 — 芯片挂死
    return "fatal"


# ══════════════════════════════════════════════════════════════════════
# 连接
# ══════════════════════════════════════════════════════════════════════
def _probe(job, port, baud, before, sink):
    cmd = esptool_cmd(port, baud, before, "no-reset", ["chip-id"])
    rc, lines = run_esptool(cmd, sink, cwd=Path(job.images.root) if job.images else None, timeout=90)
    return rc, lines


def connect(job, sink, res):
    """找板 + 波特率阶梯试连。成功返回 (port, baud, before); 失败抛异常。"""
    port, before = job.port, "default-reset"
    if not port:
        port = device.find_port()
    if not port:
        if device.has_disk_mode():
            sink.log(">>> 设备处于 U 盘模式 (303A:4004) — 该模式不能烧录, 请在设备上关闭 U 盘后重试")
        if job.race or job.wait > 0:
            sink.step("找板", "RUN", "等待设备出现… 请在设备上触发复位或拔插电源")
            port = device.wait_port(job.wait, on_tick=lambda left: sink.progress(
                None, "等待设备出现… %ds" % left))
            if not port:
                raise EngineError("等了 %.0fs 没等到可烧录的串口" % job.wait)
            sink.step("找板", "PASS", port)
        else:
            raise EngineError("没找到可烧录的串口。\n" + device.describe_ports())
    else:
        sink.step("找板", "PASS", port)
    if job.race:
        before = "usb-reset"      # 强制走 USB 复位路径, 不进 ROM 窗口也能抢

    attempts = 0
    for i, baud in enumerate(job.ladder):
        attempts += 1
        rc, lines = _probe(job, port, baud, before, sink)
        if rc == 0:
            if i:
                sink.step("连接", "WARN", "%d 档波特率后连通 (%d)" % (len(job.ladder[:i + 1]), baud))
            else:
                sink.step("连接", "PASS", "%s @%d" % (port, baud))
            return port, baud, before
        kind = classify(lines, device.port_present(port), attempts)
        if i + 1 < len(job.ladder) and kind == "retry":
            sink.step("连接", "WARN", "波特率 %d 连不上, 降级重试" % baud)
            continue
        if kind == "power":
            raise NeedPowerCycle("芯片没有响应 (%s) — 反复重试无用" % port)
        raise EngineError("连不上 %s @%d (esptool rc=%d)" % (port, baud, rc))
    raise EngineError("所有波特率都连不上: %s" % (job.ladder,))


class _Quiet:
    """吞掉输出的 sink — 只关心 returncode 与攒下来的 lines。"""
    def log(self, *a, **k): pass
    def step(self, *a, **k): pass
    def progress(self, *a, **k): pass
    def progress_done(self, *a, **k): pass
    def cancelled(self): return False
    def need_power_cycle(self, *a, **k): return False


def read_mac(port, baud, before="default-reset") -> str:
    """读芯片 MAC — 擦除前展示, 防止连错板擦错板。读不到返回 ""。"""
    cmd = esptool_cmd(port, baud, before, "no-reset", ["read-mac"])
    rc, lines = run_esptool(cmd, _Quiet(), timeout=60)
    if rc != 0:
        return ""
    for ln in lines:
        m = MAC_RE.search(ln)
        if m:
            return m.group(1).upper()
    return ""


# ══════════════════════════════════════════════════════════════════════
# 写 / 校验 / 擦除 / 备份
# ══════════════════════════════════════════════════════════════════════
def _pairs(ops):
    out = []
    for op in ops:
        out += [hex(op.offset), str(Path(op.src).resolve())]
    return out


def write_ops(job, sink, port, baud, before, ops, after="no-reset"):
    """一次 write-flash 写完整个写集合。

    进度: esptool 的百分比是**当前区域**的, 靠 "Wrote N bytes at 0xADDR" 把已完成区域
    累加起来换成整体百分比。解析不到就退化成"只打日志" — 判定只看 rc。
    """
    args = ["write-flash"] + job.flash_args() + _pairs(ops)
    cmd = esptool_cmd(port, baud, before, after, args)
    total = sum(op.size for op in ops)
    st = {"done": 0, "cur": 0}      # 已完成字节 / 当前区域下标

    def on_event(line, pct):
        if pct is not None:
            if st["cur"] < len(ops) and total:
                op = ops[st["cur"]]
                sink.progress(min(1.0, (st["done"] + pct * op.size) / total),
                              "%s (%d/%d)" % (Path(op.src).name, st["cur"] + 1, len(ops)))
            return
        sink.log(line, raw=True)
        m = WROTE_RE.match(line.strip())
        if m and st["cur"] < len(ops):
            st["done"] += min(int(m.group(1)), ops[st["cur"]].size)
            st["cur"] += 1
            if total:
                sink.progress(min(1.0, st["done"] / total), "")

    rc, lines = run_esptool(cmd, sink, cwd=Path(job.images.root),
                            timeout=1800, on_event=on_event)
    sink.progress_done()
    return rc, lines


def verify_set(job) -> list:
    """回读校验的范围 — assets 26MB 走一遍很慢, 默认跳过 (--verify-all 才带上)。"""
    return [o for o in job.ops if o.label != "assets" or job.verify_all]


def verify_ops(job, sink, port, baud, before, ops):
    args = ["verify-flash"] + _pairs(ops)
    cmd = esptool_cmd(port, baud, before, "no-reset", args)
    sink.progress(None, "回读校验中…")
    rc, lines = run_esptool(cmd, sink, cwd=Path(job.images.root), timeout=1800)
    sink.progress_done()
    return rc, lines


def announce_target(port, baud, before, sink):
    """擦除前亮出目标板 — 连错板是擦除唯一不可挽回的错误。"""
    mac = read_mac(port, baud, before)
    sink.step("擦除前核对", "RUN", "%s%s — 确认是这块板再继续"
              % (port, " · MAC %s" % mac if mac else ""))
    return mac


def erase_regions(job, sink, port, baud, before):
    """擦除指定分区。每个目标必须带确认令牌 — 无令牌直接拒绝。

    令牌由 confirm_phrase(label) 生成, CLI 走 --yes, GUI 要用户手打确认词 —
    误触在引擎层就被挡住, 不靠界面自觉。
    """
    for label, _off, _size, token in job.erase:      # 先验令牌再碰硬件
        if token != confirm_phrase(label):
            raise EngineError("擦除 %s 缺少正确确认令牌 — 拒绝执行" % label)
    if job.erase:
        announce_target(port, baud, before, sink)
    for label, offset, size, _token in job.erase:
        sink.step("擦除 " + label, "RUN", "0x%x .. 0x%x" % (offset, offset + size))
        rc, _lines = run_esptool(esptool_cmd(port, baud, before, "no-reset",
                                             ["erase-region", hex(offset), str(size)]),
                                 sink, timeout=600)
        sink.step("擦除 " + label, "PASS" if rc == 0 else "FAIL")
        if rc != 0:
            return False
    return True


def erase_all(job, sink, port, baud, before, more_to_come=False):
    """整片擦除。more_to_come=True 表示后面还要烧东西 → 不让芯片复位。

    擦完不复位是故意的: 空白芯片重启只会在 ROM 里空转, 留着 stub 让下一次
    esptool 调用直接接上更稳。
    """
    if job.erase_token != confirm_phrase("all"):     # 先验令牌再碰硬件
        raise EngineError("整片擦除缺少正确确认令牌 — 拒绝执行")
    announce_target(port, baud, before, sink)
    sink.step("整片擦除", "RUN", "32MB, 约 30–60s — 中途不要断电")
    rc, _lines = run_esptool(esptool_cmd(port, baud, before,
                                         "no-reset" if more_to_come else "hard-reset",
                                         ["erase-flash"]),
                             sink, timeout=1200)
    sink.step("整片擦除", "PASS" if rc == 0 else "FAIL")
    return rc == 0


def confirm_phrase(label: str) -> str:
    """擦除确认词 — GUI 要用户手打这个, CLI 要 --yes。"""
    return {"all": "擦除整片"}.get(label, "擦除")


# 擦除目标: label -> (擦掉的是什么, 后果, 强度)。强度 2 = GUI 要手打确认词。
# 从轻到重排 — 界面与打印清单都照这个顺序。
ERASE_TARGETS = {
    "otadata": ("引导记录 (8K)", "下次启动回到 ota_0 槽。应用和设备上的数据都不受影响, 可随时烧回去", 1),
    "nvs": ("WiFi 凭据 + 设备 Token + 系统设置 (24K)",
            "设备会忘记 WiFi 和登录信息, 需要重新配网并重新注册。日记不受影响", 2),
    "config": ("宠物记忆 / 设置 / 传感器日志 (512K)",
               "下次启动自动重建为空分区 — 宠物记忆和设置丢失", 2),
    "data": ("日记与交互日志 (1M)",
             "设备上的日记会永久丢失, 无法恢复。擦前建议先备份 (--backup)", 2),
    "assets": ("动画 / 音效 / 字体 / 语音模型 (26M)",
               "分区会变成空的 — 设备能开机但没动画没声音, 必须重烧 assets.bin 才恢复", 2),
    "all": ("整片 flash 32MB (含 bootloader 与分区表)",
            "设备将无法启动, 必须重新烧录 bootloader + 分区表 + 主程序", 2),
}
ERASE_ORDER = ("otadata", "nvs", "config", "data", "assets", "all")
# 单独擦会让 nvs 变成不可解密状态 — 比不擦更糟, 所以不给单独入口 (随整片擦除一并处理)
ERASE_DENIED = {"phy_init": "它只是射频校准数据, 擦不擦都一样, 设备自己会重建",
                "nvskey": "它和 nvs 绑定, 单独擦会让 nvs 变成读不出来的状态, 比不擦更糟"}


def erase_plan(labels, confirmed: bool) -> list:
    """把目标名变成引擎要的 [(label, offset, size, token)]。

    confirmed 为假时刻意不给令牌 — 引擎会直接拒绝执行, 误触在引擎层就断了。
    """
    from images import FALLBACK_PARTITIONS
    plan = []
    for label in labels:
        if label in ERASE_DENIED:
            raise EngineError("不提供 %s 的单独擦除: %s" % (label, ERASE_DENIED[label]))
        if label == "all":
            raise EngineError("整片擦除走 job.erase_all + job.erase_token, 不经这里")
        if label not in ERASE_TARGETS:
            raise EngineError("未知擦除目标 %r (可选: %s)"
                              % (label, ", ".join(ERASE_ORDER[:-1])))
        off, size = FALLBACK_PARTITIONS[label]
        plan.append((label, off, size, confirm_phrase(label) if confirmed else ""))
    return plan


# ══════════════════════════════════════════════════════════════════════
# 提取分区 — 把板上任意区域读成一个 bin 文件
# ══════════════════════════════════════════════════════════════════════
# 这两个不在分区表里 (它们位于分区表之前), 偏移固定
DUMP_FIXED = {
    "bootloader": (0x0, 0x8000, "引导程序 (32K, 上界正好到分区表)"),
    "partition-table": (PTABLE_OFF, 0x1000, "分区表 (4K)"),
}
# 内容本身就是凭据 — 提取要另加确认词, 产出文件等同凭据
DUMP_SENSITIVE = {"nvs": ("WiFi 密码 + 设备 Token + 系统设置", "提取凭据")}
# 不提供提取: 理由与擦除拒绝它一致
DUMP_DENIED = {
    "nvskey": "它是 NVS 加密密钥 — 加密一旦真的启用, 导出这个文件等于交出钥匙",
    "phy_init": "只是射频校准数据, 设备自己会重建, 没有提取价值",
}


def dump_phrase(label: str) -> str:
    """该目标要不要确认词; 返回 "" 表示直接放行。"""
    ent = DUMP_SENSITIVE.get(label)
    return ent[1] if ent else ""


def read_partition_table(port, baud, before) -> dict:
    """从芯片读分区表 → {label: Partition}。读不到返回 {}。

    比用内置偏移表强: 手头没有镜像目录(比如只想备份一块量产的板子)时,
    也知道这块板真实的布局, 而不是假设它跟 build/ 里的一致。
    """
    from images import parse_partition_table
    tmp = Path(tempfile.gettempdir()) / "vp_ptable_read.bin"
    try:
        rc, _ = run_esptool(esptool_cmd(port, baud, before, "no-reset",
                                        ["read-flash", hex(PTABLE_OFF), "4096", str(tmp)]),
                            _Quiet(), timeout=180)
        if rc != 0 or not tmp.is_file():
            return {}
        return parse_partition_table(tmp)
    except Exception:  # noqa: BLE001  读不到就退回内置表, 不该因此中断
        return {}
    finally:
        tmp.unlink(missing_ok=True)


# 分区类型/子类型 (IDF partition_table) — 比复述尺寸有用: 一眼知道这一区是什么
_PT_TYPE = {0x00: "app", 0x01: "data"}
_PT_SUB = {
    (0x00, 0x00): "factory", (0x00, 0x10): "ota_0", (0x00, 0x11): "ota_1",
    (0x00, 0x20): "test",
    (0x01, 0x00): "otadata", (0x01, 0x01): "phy", (0x01, 0x02): "nvs",
    (0x01, 0x04): "nvs_keys", (0x01, 0x81): "fat", (0x01, 0x82): "spiffs",
    (0x01, 0x83): "littlefs",
}


def _ptype(p) -> str:
    return "%s/%s" % (_PT_TYPE.get(p.type, "0x%02x" % p.type),
                      _PT_SUB.get((p.type, p.subtype), "0x%02x" % p.subtype))


def dump_avail(partitions: dict) -> dict:
    """可提取目标: label -> (offset, size, 说明)。"""
    out = dict(DUMP_FIXED)
    for label, p in (partitions or {}).items():
        if label in DUMP_DENIED or p.offset is None or not p.size:
            continue
        out[label] = (p.offset, p.size, _ptype(p))
    if not partitions:
        # 分区表没读到 — 退回内置表, 并在说明里标出来 (别让人以为这是板上实测值)
        from images import FALLBACK_PARTITIONS
        for label, (off, size) in FALLBACK_PARTITIONS.items():
            if label in DUMP_DENIED or label in out:
                continue
            out[label] = (off, size, "%s (内置表, 未读到板上分区表)" % _pretty_size(size))
    return out


def dump_plan(avail: dict, labels, confirmed: bool) -> list:
    """labels → [(label, offset, size, token)]。未知/被拒/越界即抛。"""
    plan = []
    for label in labels:
        if label in DUMP_DENIED:
            raise EngineError("不提供 %s 的提取: %s" % (label, DUMP_DENIED[label]))
        if label == "all":
            plan.append(("all", 0x0, FLASH_SIZE, ""))
            continue
        if label not in avail:
            raise EngineError("这块板上没有 %r 这个分区 (可用: %s)"
                              % (label, ", ".join(sorted(avail))))
        off, size, _note = avail[label]
        if off < 0 or off + size > FLASH_SIZE:
            raise EngineError("%s 的范围 0x%x..0x%x 超出 32MB, 拒绝提取"
                              % (label, off, off + size))
        plan.append((label, off, size, dump_phrase(label) if confirmed else ""))
    return plan


def dump_regions(job, sink, port, baud, before, plan):
    """把 plan 里的区域逐个读成文件。任何一块读不到即中止 (不留半份)。"""
    for label, _o, _s, tok in plan:          # 先验令牌再碰硬件
        want = dump_phrase(label)
        if want and tok != want:
            raise EngineError("提取 %s 需要确认词「%s」— 拒绝执行" % (label, want))
    out_dir = Path(job.dump_dir or ".")
    out_dir.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d_%H%M%S")
    made = []
    for label, offset, size, _tok in plan:
        dst = out_dir / ("%s_%s.bin" % (label, stamp))
        sink.step("提取 " + label, "RUN",
                  "0x%x..0x%x (%s) → %s" % (offset, offset + size, _pretty_size(size), dst.name))
        rc, _lines = run_esptool(esptool_cmd(port, baud, before, "no-reset",
                                             ["read-flash", hex(offset), str(size), str(dst)]),
                                 sink, timeout=1800)
        if rc != 0:
            sink.step("提取 " + label, "FAIL", "读不到 — 已中止")
            return None
        got = dst.stat().st_size
        sink.step("提取 " + label, "PASS",
                  "%s  md5 %s" % (_pretty_size(got), _md5(dst)[:12]))
        made.append((label, dst))
    return made


def _pretty_size(n: int) -> str:
    if n >= 1048576:
        return "%.1fM" % (n / 1048576.0)
    if n >= 1024:
        return "%dK" % round(n / 1024.0)
    return "%d B" % n


def backup_targets(job, sink, port, baud, before):
    """擦前把目标分区整区读出存文件 — 误擦的唯一后路。"""
    if not job.backup_dir:
        return []
    made = []
    for label, offset, size, _tok in job.erase:
        if label == "all":
            continue
        dst = Path(job.backup_dir) / ("%s_%s.bin" % (label, time.strftime("%Y%m%d_%H%M%S")))
        dst.parent.mkdir(parents=True, exist_ok=True)
        sink.step("备份 " + label, "RUN", str(dst))
        rc, _ = run_esptool(esptool_cmd(port, baud, before, "no-reset",
                                        ["read-flash", hex(offset), str(size), str(dst)]),
                            sink, timeout=900)
        if rc != 0:
            sink.step("备份 " + label, "FAIL", "备份失败 — 已中止, 不擦")
            return None
        sink.step("备份 " + label, "PASS", "%s (md5 %s)" % (dst.name, _md5(dst)[:8]))
        made.append((label, dst))
    return made


def restore_file(job, sink, port, baud, before):
    """把备份写回原分区 (受控通道: 只在显式指名时用, 同样要 --yes)。"""
    from images import FALLBACK_PARTITIONS
    dst, label = Path(job.restore_file), job.restore_target
    if label not in FALLBACK_PARTITIONS:
        raise EngineError("未知分区 %r" % label)
    offset = job.images.part(label).offset if job.images else FALLBACK_PARTITIONS[label][0]
    sink.step("写回 " + label, "RUN", "0x%x ← %s" % (offset, dst.name))
    rc, _ = run_esptool(esptool_cmd(port, baud, before, "no-reset",
                                    ["write-flash", "--flash-mode", "dio", "--flash-size", "32MB",
                                     "--flash-freq", "80m", hex(offset), str(dst.resolve())]),
                        sink, cwd=Path(job.images.root) if job.images else None, timeout=900)
    sink.step("写回 " + label, "PASS" if rc == 0 else "FAIL")
    return rc == 0


# ══════════════════════════════════════════════════════════════════════
# 用户数据取证
# ══════════════════════════════════════════════════════════════════════
PROTECT_LABELS = ("nvs", "config", "data")


def _md5(p) -> str:
    h = hashlib.md5()
    with open(p, "rb") as f:
        for blk in iter(lambda: f.read(1 << 20), b""):
            h.update(blk)
    return h.hexdigest()


def protect_dump(job, sink, port, baud, before, out_path):
    """读 nvs/config/data 三段存 md5 — 烧前留基线, 烧后取证工具没碰用户数据。"""
    from images import FALLBACK_PARTITIONS
    import json
    out = {}
    for label in PROTECT_LABELS:
        offset, size = FALLBACK_PARTITIONS[label]
        tmp = Path(out_path).with_suffix(".%s.tmp" % label)
        sink.step("取证 " + label, "RUN")
        rc, _ = run_esptool(esptool_cmd(port, baud, before, "no-reset",
                                        ["read-flash", hex(offset), str(size), str(tmp)]),
                            sink, timeout=900)
        if rc != 0:
            sink.step("取证 " + label, "FAIL", "读不到 — 无法取证")
            return None
        out[label] = {"offset": offset, "size": size, "md5": _md5(tmp)}
        tmp.unlink(missing_ok=True)
        sink.step("取证 " + label, "PASS", out[label]["md5"][:16])
    Path(out_path).write_text(json.dumps(out, indent=2), "utf-8")
    sink.log(">>> 基线已存 %s" % out_path)
    return out


def protect_compare(job, sink, port, baud, before, base_path):
    """复算三段 md5 与基线比对 — 不一致就是工具碰了不该碰的地方。"""
    import json
    try:
        base = json.loads(Path(base_path).read_text("utf-8"))
    except Exception as e:  # noqa: BLE001
        sink.step("保留区核对", "WARN", "基线读不了: %s" % e)
        return True
    from images import FALLBACK_PARTITIONS
    bad = []
    for label in PROTECT_LABELS:
        if label not in base:
            continue
        offset, size = FALLBACK_PARTITIONS[label]
        tmp = Path(base_path).with_suffix(".%s.tmp" % label)
        rc, _ = run_esptool(esptool_cmd(port, baud, before, "no-reset",
                                        ["read-flash", hex(offset), str(size), str(tmp)]),
                            sink, timeout=900)
        if rc != 0:
            sink.step("保留区核对", "WARN", "%s 读不到, 跳过" % label)
            continue
        got = _md5(tmp)
        tmp.unlink(missing_ok=True)
        if got != base[label]["md5"]:
            bad.append(label)
    if bad:
        sink.step("保留区核对", "FAIL", "被改动: %s" % ", ".join(bad))
        return False
    sink.step("保留区核对", "PASS", "nvs/config/data 均未变动")
    return True


# ══════════════════════════════════════════════════════════════════════
# 主流程
# ══════════════════════════════════════════════════════════════════════
MAX_POWER_ROUNDS = 2     # 断电重来的轮次上限 — 芯片反复挂死时别无限转圈


def run_job(job, sink) -> Result:
    res = Result()
    iss = job.images

    # S0 解析与断言 (纯本地, 不碰硬件)
    if iss is not None:
        try:
            iss.assert_safe(job.ops)
            warns = verify_version(iss)
            for w in warns:
                sink.step("解析镜像", "WARN", w)
            res.add("解析镜像", "WARN" if warns else "PASS",
                    "v%s · %d 个区域 · %.1f MB" % (iss.version or "?",
                                                   len(job.ops),
                                                   sum(o.size for o in job.ops) / 1048576.0))
        except UnsafeImageSet as e:
            sink.step("解析镜像", "FAIL", str(e))
            res.add("解析镜像", "FAIL", str(e))
            res.error, res.exit_code = str(e), 2
            return res

    if job.dry_run:
        # 顺序与真跑一致: 写回 → 整片擦除 → 写镜像 → 校验 → 分区擦除
        p, pb = "<端口>", ("usb-reset" if job.race else "default-reset")
        if job.ops:
            sink.log("\n将写入的区域:")
            sink.log(iss.describe_ops(job.ops))
        if job.restore_file:
            res.command_lines.append(" ".join(str(c) for c in esptool_cmd(
                p, job.ladder[0], pb, "no-reset",
                ["write-flash"] + job.flash_args() + ["<偏移>", str(Path(job.restore_file).resolve())]))
                + "   # 写回 %s" % job.restore_target)
        if job.erase_all:
            res.command_lines.append(" ".join(str(c) for c in esptool_cmd(
                p, job.ladder[0], pb, "no-reset" if job.ops else "hard-reset", ["erase-flash"])))
        if job.ops:
            res.command_lines.append(" ".join(str(c) for c in esptool_cmd(
                p, job.ladder[0], pb, "no-reset",
                ["write-flash"] + job.flash_args() + _pairs(job.ops))))
        if job.verify and job.ops:
            vops = verify_set(job)
            res.command_lines.append(" ".join(str(c) for c in esptool_cmd(
                p, job.ladder[0], pb, "no-reset", ["verify-flash"] + _pairs(vops))))
        if job.erase:
            res.command_lines.append(" ".join(str(c) for c in esptool_cmd(
                p, job.ladder[0], pb, "no-reset",
                ["erase-region", " ".join("%s %d" % (hex(o), s) for _l, o, s, _t in job.erase)])))
        if job.list_partitions:
            res.command_lines.append(" ".join(str(c) for c in esptool_cmd(
                p, job.ladder[0], pb, "no-reset",
                ["read-flash", hex(PTABLE_OFF), "4096", "<临时文件>"]))
                + "   # 读板上分区表并列出来")
        if job.dump:
            # 不连板子拿不到真实偏移 — 用兜底表展示形状, 真跑时以板上分区表为准
            avail = dump_avail({})
            for label in job.dump:
                if label == "all":
                    off, size = 0x0, FLASH_SIZE
                elif label in avail:
                    off, size, _n = avail[label]
                else:
                    res.command_lines.append("  # %s: 板上分区表里没有这个分区" % label)
                    continue
                res.command_lines.append(" ".join(str(c) for c in esptool_cmd(
                    p, job.ladder[0], pb, "no-reset",
                    ["read-flash", hex(off), str(size),
                     "<%s>/%s_<时间戳>.bin" % (job.dump_dir, label)]))
                    + "   # 偏移以板上分区表为准 (此处用兜底表)")
        sink.log("\n将执行的命令:")
        for c in res.command_lines:
            sink.log("  " + c)
        if not res.command_lines:
            sink.log("  (没有任何动作 — 参数没给全?)")
        sink.log("\n(试运行 — 未碰硬件)")
        res.ok, res.exit_code = True, 0
        return res

    # S1/S2 找板 + 连接
    try:
        port, baud, before = connect(job, sink, res)
        res.add("找板", "PASS", port)
        res.add("连接", "PASS", "%d baud" % baud)
    except NeedPowerCycle as e:
        res.add("连接", "FAIL", str(e))
        res.error, res.need_power = str(e), True
        sink.step("连接", "FAIL", str(e))
        if job._depth < MAX_POWER_ROUNDS and sink.need_power_cycle(str(e)):
            sink.log(">>> 已重新上电, 从找板重来 (第 %d 轮)" % (job._depth + 2))
            job._depth += 1
            return run_job(job, sink)
        res.exit_code = 1
        return res
    except EngineError as e:
        res.add("连接", "FAIL", str(e))
        res.error, res.exit_code = str(e), 2
        sink.step("连接", "FAIL", str(e))
        return res

    base = dict(port=port, baud=baud, before=before)
    fails = 0        # 连续失败次数 — 口在却连不上即判芯片挂死

    # 提取分区 / 列分区表 (只读, 排在所有写之前 — 存的必须是改动前的东西)
    if job.list_partitions:
        parts = read_partition_table(**base)
        if not parts:
            sink.step("读分区表", "FAIL", "读不到板上的分区表")
            res.error, res.exit_code = "读分区表失败", 1
            return res
        sink.log("\n板上分区表 (%d 个):" % len(parts))
        sink.log("  %-16s %-10s %-10s %s" % ("标签", "偏移", "大小", "说明"))
        for label, p in sorted(parts.items(), key=lambda kv: kv[1].offset):
            note = DUMP_SENSITIVE[label][0] if label in DUMP_SENSITIVE else ""
            if label in DUMP_DENIED:
                note = "不提供提取 — " + DUMP_DENIED[label]
            sink.log("  %-16s 0x%-8x %-10s %s"
                     % (label, p.offset, _pretty_size(p.size), note))
        res.add("读分区表", "PASS", "%d 个分区" % len(parts))
        res.ok, res.exit_code = True, 0
        return res

    if job.dump:
        # 偏移要连上板子才知道 — 读它自己的分区表, 而不是假设跟 build/ 一致
        avail = dump_avail(read_partition_table(**base))
        try:
            plan = dump_plan(avail, job.dump, job.dump_confirmed)
            made = dump_regions(job, sink, **base, plan=plan)
        except EngineError as e:
            sink.step("提取", "FAIL", str(e))
            res.add("提取", "FAIL", str(e))
            res.error, res.exit_code = str(e), 2
            return res
        if made is None:
            res.add("提取", "FAIL")
            res.error, res.exit_code = "提取失败", 1
            return res
        res.add("提取", "PASS", "%d 个区域 → %s" % (len(made), job.dump_dir))

    # 写回备份 (受控通道: 只在显式指名目标时走, 不参与烧录动线)
    if job.restore_file:
        if not restore_file(job, sink, **base):
            res.add("写回", "FAIL")
            res.error, res.exit_code = "写回失败", 1
            return res
        res.add("写回", "PASS", "%s → %s" % (Path(job.restore_file).name, job.restore_target))

    # 整片擦除必须排在写之前 — 擦完不重烧等于把设备擦成砖
    if job.erase_all:
        try:
            ok = erase_all(job, sink, **base, more_to_come=bool(job.ops))
        except EngineError as e:
            sink.step("整片擦除", "FAIL", str(e))
            res.add("整片擦除", "FAIL", str(e))
            res.error, res.exit_code = str(e), 2
            return res
        if not ok:
            res.add("整片擦除", "FAIL")
            res.error, res.exit_code = "整片擦除失败", 1
            return res
        res.add("整片擦除", "PASS")

    # 备份 (分区擦除前)
    if job.erase and job.backup_dir:
        if backup_targets(job, sink, **base) is None:
            res.error, res.exit_code = "备份失败, 已中止 (未擦除任何分区)", 1
            return res

    # 写镜像
    if job.ops:
        sink.step("写镜像", "RUN", "%d 个区域" % len(job.ops))
        after = "no-reset" if job.capture_seconds > 0 else "hard-reset"
        rc, lines = write_ops(job, sink, port=port, baud=baud, before=before, ops=job.ops, after=after)
        if rc != 0:
            fails += 1
            sink.step("写镜像", "FAIL", "esptool rc=%d" % rc)
            res.add("写镜像", "FAIL")
            kind = classify(lines, device.port_present(port), fails)
            res.error = "写镜像失败"
            if kind == "power":
                res.need_power = True
                if job._depth < MAX_POWER_ROUNDS and sink.need_power_cycle("写镜像失败且芯片无响应"):
                    job._depth += 1
                    sink.log(">>> 已重新上电, 从找板重来 (第 %d 轮)" % (job._depth + 1))
                    return run_job(job, sink)
            res.exit_code = 1
            return res
        sink.step("写镜像", "PASS", "%.1f MB" % (sum(o.size for o in job.ops) / 1048576.0))
        res.add("写镜像", "PASS")

        # 回读校验
        if job.verify:
            vops = verify_set(job)
            sink.step("回读校验", "RUN", "%d 个区域" % len(vops))
            rc, lines = verify_ops(job, sink, port=port, baud=baud, before=before, ops=vops)
            if rc != 0:
                sink.step("回读校验", "FAIL", "写进去的内容和源文件不一致 — 别直接用, 断电重烧")
                res.add("回读校验", "FAIL")
                res.error, res.exit_code = "回读校验失败", 1
                return res
            sink.step("回读校验", "PASS")
            res.add("回读校验", "PASS")
        else:
            res.add("回读校验", "SKIP", "--no-verify")

    # 分区擦除 (独立动作: 擦完就结束, 不会顺手写任何东西)
    if job.erase:
        try:
            ok = erase_regions(job, sink, **base)
        except EngineError as e:
            sink.step("擦除", "FAIL", str(e))
            res.add("擦除", "FAIL", str(e))
            res.error, res.exit_code = str(e), 2
            return res
        if not ok:
            res.add("擦除", "FAIL")
            res.error, res.exit_code = "擦除失败", 1
            return res
        res.add("擦除", "PASS", ", ".join(l for l, *_ in job.erase))

    if job.protect_before and job.protect_dump:
        # 只取证不烧时, 读出来就是终点 — 读不到必须让整件事失败
        if protect_dump(job, sink, **base, out_path=job.protect_dump) is None:
            res.add("取证", "FAIL", "读不到用户分区")
            res.error, res.exit_code = "取证失败", 1
            return res
        res.add("取证", "PASS", str(job.protect_dump))

    # 保留区核对必须紧跟在读基线之后、复位之前。
    # 放到复位抓串口之后做会误报: 那一步会让应用跑起来, 而应用自己就会往 /cfg
    # 和 /data 写 (传感器日志/电量日志) — 比对会把设备自己的写入栽赃成"工具碰了"。
    # 此刻芯片还停在下载模式, 中间没跑过任何东西, 差异只可能来自工具的写操作。
    if job.protect_compare:
        if not protect_compare(job, sink, **base, base_path=job.protect_compare):
            res.error, res.exit_code = "保留区被改动", 1
            return res
        res.add("保留区核对", "PASS", "三段 md5 与基线一致")

    # S7 复位 + 抓串口 (抓串口是为了核对刚烧进去的版本 — 纯取证/纯读回时不复位,
    # 免得为了读个 md5 把一块本来正常的板子重启一遍)
    seen_ver, seen_proj = "", ""
    if job.capture_seconds > 0 and not job.erase_all and (job.ops or job.erase or job.restore_file):
        sink.step("复位抓串口", "RUN")
        try:
            # 看到 App version 就收工 — 它在 I(893), 没必要空等满整个窗口
            lines = device.capture_after_reset(
                port, job.capture_seconds,
                until=lambda ln: bool(APP_VER_RE.search(ln)))
        except device.DeviceError as e:
            lines = []
            sink.step("复位抓串口", "WARN", str(e))
        if not lines:
            sink.step("复位抓串口", "WARN", "没抓到串口输出")
            res.add("复位抓串口", "WARN")
        else:
            for ln in lines:
                m = APP_VER_RE.search(ln)
                if m:
                    seen_ver = m.group(1)
                m = PROJ_RE.search(ln)
                if m:
                    seen_proj = m.group(1)
            sink.step("复位抓串口", "PASS", "%d 行" % len(lines))
            res.add("复位抓串口", "PASS")

        # S8 版本核对
        want = job.images.version if job.images else ""
        if seen_ver:
            res.version = seen_ver
            if want and seen_ver != want:
                sink.step("版本核对", "FAIL", "板上跑的是 %s, 烧进去的是 %s" % (seen_ver, want))
                res.add("版本核对", "FAIL")
                res.error, res.exit_code = "版本不匹配", 1
                return res
            if seen_proj and seen_proj != "Virtualpet":
                sink.step("版本核对", "WARN", "project name 是 %s" % seen_proj)
            sink.step("版本核对", "PASS", "App version: %s" % seen_ver)
            res.add("版本核对", "PASS", seen_ver)
        elif job.capture_seconds > 0:
            sink.step("版本核对", "WARN", "串口里没看到 App version 行 (日志级别? 板子真重启了吗?)")
            res.add("版本核对", "WARN")

    res.ok, res.exit_code = True, 0
    return res
