#!/usr/bin/env python3
"""图形界面 — pywebview + WebView2, 界面在 gui.html。

分工: 引擎跑在工作线程里, 事件进 queue, JS 每 100ms 调一次 poll() 取走渲染。
不用 Python 主动推 JS —— 跨线程推 pywebview 不稳, 轮询虽然土但不会出怪事。

擦除确认在**这一层**再验一遍确认词: 界面的按钮状态是给用户看的, 不是安全边界。
"""
import json
import queue
import sys
import threading
import traceback
from pathlib import Path

import device
import engine
import images

WEBVIEW2_GUID = "{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}"
WEBVIEW2_URL = "https://developer.microsoft.com/microsoft-edge/webview2/"
RESTORE_PHRASE = "写回"      # 写回备份的确认词, 与 gui.html 里的提示词一致


def _default_backup_dir() -> Path:
    """勾了"擦前备份"却没填目录时的落点 — 绝不能因为目录空着就不备份。"""
    if getattr(sys, "frozen", False):
        return Path(sys.executable).resolve().parent / "backup"
    return Path(__file__).resolve().parent / "backup"


def _default_dump_dir() -> Path:
    """提取文件的落点 — 与备份同级。双击 exe 时 CWD 未必是 exe 所在目录。"""
    if getattr(sys, "frozen", False):
        return Path(sys.executable).resolve().parent / "dump"
    return Path(__file__).resolve().parent / "dump"


# ══════════════════════════════════════════════════════════════════════
# WebView2 检测 (必须走 winreg: reg query 在 Git Bash 下会花括号假阴性, 实测过)
# ══════════════════════════════════════════════════════════════════════
def webview2_version() -> str:
    if sys.platform != "win32":
        return "?"
    import winreg
    roots = [
        (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\%s" % WEBVIEW2_GUID),
        (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\EdgeUpdate\Clients\%s" % WEBVIEW2_GUID),
        (winreg.HKEY_CURRENT_USER, r"SOFTWARE\Microsoft\EdgeUpdate\Clients\%s" % WEBVIEW2_GUID),
    ]
    for hive, path in roots:
        try:
            with winreg.OpenKey(hive, path) as k:
                v = winreg.QueryValueEx(k, "pv")[0]
                if v:
                    return str(v)
        except OSError:
            continue
    return ""


def _message_box(title, text):
    try:
        import ctypes
        ctypes.windll.user32.MessageBoxW(None, text, title, 0x10)
    except Exception:  # noqa: BLE001
        pass


def precheck() -> bool:
    """开窗前的把关 — 控制台这时还看得见, 报错能打出来。"""
    v = webview2_version()
    if v:
        return True
    msg = ("缺少 Microsoft Edge WebView2 运行时, 图形界面无法启动。\n\n"
           "请安装后重试 (免费, 约 2MB):\n%s\n\n"
           "也可以直接用命令行模式, 不受影响:\n"
           "  VirtualpetFlasher.exe --full\n"
           "  VirtualpetFlasher.exe --selftest" % WEBVIEW2_URL)
    print("!! " + msg.replace("\n\n", "\n"), file=sys.stderr)
    if sys.platform == "win32":
        _message_box("Virtualpet 烧录器 — 缺少 WebView2", msg)
    return False


# ══════════════════════════════════════════════════════════════════════
class GuiSink:
    """引擎 → GUI 的事件通道。引擎在工作线程里调它, 全是非阻塞的。"""

    def __init__(self, q: queue.Queue):
        self.q = q
        self._cancel = threading.Event()
        self._power = threading.Event()
        self._power_ans = False

    def _put(self, ev):
        self.q.put(ev)

    def log(self, line, raw=False):
        self._put({"t": "log", "line": line})

    def step(self, name, status, detail=""):
        self._put({"t": "step", "name": name, "status": status, "detail": detail})

    def progress(self, frac, text=None):
        self._put({"t": "progress", "frac": frac, "text": text or ""})

    def progress_done(self):
        self._put({"t": "progress", "frac": None, "text": ""})

    def cancelled(self) -> bool:
        return self._cancel.is_set()

    def reset_cancel(self):
        self._cancel.clear()

    def cancel(self):
        self._cancel.set()

    def need_power_cycle(self, reason) -> bool:
        """挂起工作线程, 等界面回话 (超时就当放弃)。"""
        self._power.clear()
        self._put({"t": "power", "reason": reason})
        if not self._power.wait(timeout=600):
            return False
        return self._power_ans

    def power_answer(self, ok: bool):
        self._power_ans = bool(ok)
        self._power.set()


def _ns(**over):
    """从 argparse 的默认值起手再覆盖 — 加新参数时不会在这里静默漏掉。

    GUI 走的是与命令行**同一个** make_job, 所以两边的闸门与安全断言完全一致:
    界面上没有"绕过校验"的近路。
    """
    import argparse
    import cli
    base = vars(cli.build_parser().parse_args([]))
    base.update({"yes": True, "prompt": True})      # GUI 已确认过, 且要弹断电框
    base.update(over)
    return argparse.Namespace(**base)


# ══════════════════════════════════════════════════════════════════════
class Api:
    """暴露给 JS 的方法。名字与 gui.html 里的 pywebview.api.* 一一对应。"""

    def __init__(self, window_holder):
        self.q = queue.Queue()
        self.sink = GuiSink(self.q)
        self.win = window_holder            # list, 窗口建好后填 [window]
        self.busy = False
        self.result = None
        self.images = None
        self._lock = threading.Lock()

    # ── 轮询 ──
    def poll(self):
        out = []
        for _ in range(400):
            try:
                out.append(self.q.get_nowait())
            except queue.Empty:
                break
        return json.dumps({"events": out, "busy": self.busy}, ensure_ascii=False)

    def bootstrap(self):
        """开窗时拉一次静态信息: 镜像横幅 / 版本 / 端口 / 擦除目标表。"""
        info = {"version": "", "project": "", "source": "", "warnings": [],
                "ports": [], "targets": [], "webview2": webview2_version(),
                "tool": _tool_stamp()}
        try:
            self.images = images.load()
            iss = self.images
            info["version"] = iss.version
            info["project"] = iss.app.project if iss.app else ""
            info["source"] = "%s (%s)" % (iss.root, iss.source)
            info["warnings"] = list(iss.warnings)
            info["assets"] = "assets" in iss.images
            info["has_bootloader"] = "bootloader" in iss.images
        except Exception as e:  # noqa: BLE001
            info["warnings"] = ["镜像解析失败: %s" % e]
        info["ports"] = self.refresh_ports()
        # "all" 不在分区表里 (它不是分区), 但界面上要有这个按钮 — 别用分区表过滤
        info["targets"] = [
            {"label": l, "what": engine.ERASE_TARGETS[l][0],
             "conseq": engine.ERASE_TARGETS[l][1], "level": engine.ERASE_TARGETS[l][2],
             "phrase": engine.confirm_phrase(l),
             "offset": images.FALLBACK_PARTITIONS.get(l, (None, None))[0]}
            for l in engine.ERASE_ORDER
            if l == "all" or l in images.FALLBACK_PARTITIONS]
        return json.dumps(info, ensure_ascii=False)

    def refresh_ports(self):
        try:
            return [{"dev": d, "role": r,
                     "text": "%s  %s" % (d, {"flashable": "可烧录", "disk": "U盘模式 (不能烧录)"}.get(r, "其它"))}
                    for d, _v, _p, r in device.list_ports() if r != "other"]
        except Exception:  # noqa: BLE001
            return []

    def selftest(self):
        if self.busy:
            return "busy"
        self._start(lambda: self._do_selftest(), "自检")
        return "ok"

    def _do_selftest(self):
        import selftest
        rc = selftest.run(verbose=False)
        return rc == 0, 0 if rc == 0 else 1, "自检 FAIL" if rc else ""

    # ── 烧录 ──
    def start(self, kind, opts_json):
        if self.busy:
            return "busy"
        opts = json.loads(opts_json or "{}")
        self._start(lambda: self._do_flash(kind, opts), "烧录")
        return "ok"

    def _do_flash(self, kind, opts):
        return self._run_job(_ns(
            full=(kind == "full"), app_only=(kind == "app"),
            erase_otadata=(kind == "otadata"),
            assets=bool(opts.get("assets")), ota1=bool(opts.get("ota1", True)),
            verify=bool(opts.get("verify", True)), capture=bool(opts.get("capture", True)),
            race=bool(opts.get("race")), wait=float(opts.get("wait", 60)),
            baud=int(opts.get("baud", 921600)), port=opts.get("port"),
            protect_check=bool(opts.get("protect"))))

    def _run_job(self, ns):
        import cli
        try:
            job = cli.make_job(ns, self.sink)
        except (cli.UsageError, images.UnsafeImageSet, engine.EngineError) as e:
            return False, 2, str(e)
        job.scripted = False           # GUI 要弹"请断电"的框, 不能当非交互
        res = engine.run_job(job, self.sink)
        return res.ok, res.exit_code, res.error

    # ── 擦除 ──
    def erase_info(self, targets_json):
        """后台读 MAC — 读芯片 MAC 要开串口, 别让界面卡住。"""
        if self.busy:
            return "busy"
        targets = json.loads(targets_json or "[]")
        self._start(lambda: self._do_erase_info(targets), "读取设备信息")
        return "ok"

    def _do_erase_info(self, targets):
        port = self.refresh_ports()
        dev = port[0]["dev"] if port else None
        mac = ""
        if dev:
            self.sink.log(">>> 读取设备 MAC 用于核对 (端口 %s)…" % dev)
            try:
                mac = engine.read_mac(dev, 115200)
            except Exception as e:  # noqa: BLE001
                self.sink.log(">>> 读 MAC 失败: %s" % e)
        self.q.put({"t": "erase_info", "targets": targets, "port": dev, "mac": mac})
        return True, 0, ""

    def erase(self, targets_json, phrase, backup, backup_dir=""):
        if self.busy:
            return "busy"
        targets = json.loads(targets_json or "[]")
        # 界面层已经要求手打确认词了, 这里再验一次 — 界面不是安全边界
        for t in targets:
            want = engine.confirm_phrase(t)
            if phrase != want:
                return "phrase:%s" % want
        bdir = ((backup_dir or "").strip() or str(_default_backup_dir())) if backup else None
        self._start(lambda: self._do_erase(targets, phrase, bdir), "擦除")
        return "ok"

    def _do_erase(self, targets, phrase, backup_dir):
        whole = phrase == engine.confirm_phrase("all")
        regions = [t for t in targets if t != "all"]
        return self._run_job(_ns(
            capture=False, erase_all=whole, erase=",".join(regions) or None,
            backup=backup_dir))

    def restore_begin(self):
        """选备份文件 + 目标分区 — 文件对话框得在 GUI 线程里开。"""
        win = self.win[0]
        if win is None:
            return ""
        import webview
        res = win.create_file_dialog(webview.OPEN_DIALOG,
                                     file_types=("备份文件 (*.bin)", "所有文件 (*.*)"))
        return (res[0] if res else "")

    def restore(self, path, target, phrase):
        if self.busy:
            return "busy"
        if not path or not target:
            return "missing"
        if phrase != RESTORE_PHRASE:      # 同擦除: 令牌在这一层再验一遍
            return "phrase:%s" % RESTORE_PHRASE
        self._start(lambda: self._do_restore(path, target), "写回备份")
        return "ok"

    def _do_restore(self, path, target):
        return self._run_job(_ns(verify=False, restore=path, target=target))

    # ── 提取分区 ──
    def dump_list(self, port=""):
        """读板上分区表 — 要开串口, 后台做, 结果走 dump_list 事件。"""
        if self.busy:
            return "busy"
        self._start(lambda: self._do_dump_list(port or ""), "读取板上分区表")
        return "ok"

    def _do_dump_list(self, port):
        dev = port or next((p["dev"] for p in self.refresh_ports()
                            if p["role"] == "flashable"), "")
        items, note = [], ""
        if not dev:
            note = "没发现可烧录的设备 — 插上板子再点一次刷新"
        else:
            self.sink.log(">>> 读 %s 的分区表…" % dev)
            try:
                parts = engine.read_partition_table(dev, device.DEFAULT_BAUD, "default-reset")
                # 偏移按板上实测的排; 读不到就退回内置表 (说明里会标)
                for label, (off, size, desc) in sorted(engine.dump_avail(parts).items(),
                                                       key=lambda kv: kv[1][0]):
                    items.append({
                        "label": label, "offset": off, "size": size, "desc": desc,
                        "phrase": engine.dump_phrase(label),
                        "why": engine.DUMP_SENSITIVE.get(label, ("", ""))[0],
                    })
                if not parts:
                    note = "没读到板上分区表, 列出的是内置表 — 偏移可能跟这块板不一致"
            except Exception as e:  # noqa: BLE001
                note = "读取失败: %s" % e
        self.q.put({"t": "dump_list", "items": items, "note": note, "port": dev})
        # 读到了几个是结果不是成败: 说明文字随事件走, 别在 done 里再报一遍
        return True, 0, ""

    def dump(self, labels_json, phrase, out_dir="", port=""):
        if self.busy:
            return "busy"
        labels = json.loads(labels_json or "[]")
        if not labels:
            return "none"
        # 含凭据的目标要确认词 — 界面拦一道, 引擎层还会再验一次
        for lb in labels:
            want = engine.dump_phrase(lb)
            if want and phrase != want:
                return "phrase:%s" % want
        out = (out_dir or "").strip() or str(_default_dump_dir())
        self._start(lambda: self._do_dump(labels, out, port or None), "提取分区")
        return "ok"

    def _do_dump(self, labels, out_dir, port):
        # cli 的 --dump 是 append 型: 这里必须给 list, 给字符串会被当成一串单字符
        return self._run_job(_ns(dump=list(labels), dump_out=out_dir,
                                 capture=False, port=port))

    # ── 控制 ──
    def cancel(self):
        self.sink.cancel()
        return "ok"

    def power_answer(self, ok):
        self.sink.power_answer(ok)
        return "ok"

    def open_folder(self, what):
        """打开日志/备份/提取所在的文件夹 (产出在哪得让人找得到)。

        what 给目录路径就直接开那个目录 — 提取完要去看产出的 bin。
        """
        try:
            import subprocess
            import tempfile
            if what and what not in ("log", "cwd"):
                target = Path(what)
                target.mkdir(parents=True, exist_ok=True)   # 还没提取过也打得开
            else:
                target = Path(tempfile.gettempdir()) if what == "log" else Path.cwd()
            subprocess.Popen(["explorer", str(target)])
            return "ok"
        except Exception:  # noqa: BLE001
            return "fail"

    def quit(self):
        self.sink.cancel()
        win = self.win[0]
        if win is not None:
            threading.Thread(target=win.destroy, daemon=True).start()
        return "ok"

    # ── 工作线程 ──
    def _start(self, fn, what):
        with self._lock:
            if self.busy:
                return
            self.busy = True
        self.sink.reset_cancel()
        self.q.put({"t": "begin", "what": what})

        def run():
            try:
                ok, code, err = fn()
            except Exception as e:  # noqa: BLE001
                ok, code, err = False, 1, "%s: %s" % (type(e).__name__, e)
                self.q.put({"t": "log", "line": traceback.format_exc()})
            self.busy = False
            self.q.put({"t": "done", "ok": bool(ok), "exit": code, "error": err or ""})

        threading.Thread(target=run, daemon=True).start()


def _tool_stamp() -> str:
    try:
        import esptool
        return "esptool %s" % getattr(esptool, "__version__", "?")
    except Exception:  # noqa: BLE001
        return "esptool ?"


def _work_area() -> tuple:
    """桌面可用区 (已扣掉任务栏)。窗口默认尺寸按它定 — 定死 980x780 在高分屏/小屏上
    会一开就装不下自己, 上面的操作区只能靠手动滚。"""
    try:
        import ctypes
        from ctypes import wintypes

        class RECT(ctypes.Structure):
            _fields_ = [("left", wintypes.LONG), ("top", wintypes.LONG),
                        ("right", wintypes.LONG), ("bottom", wintypes.LONG)]

        r = RECT()
        if ctypes.windll.user32.SystemParametersInfoW(0x0030, 0, ctypes.byref(r), 0):
            return r.right - r.left, r.bottom - r.top
    except Exception:  # noqa: BLE001
        pass
    return 1440, 900


# ══════════════════════════════════════════════════════════════════════
def run() -> int:
    import webview

    holder = [None]
    api = Api(holder)
    html = Path(__file__).resolve().parent / "gui.html"
    if not html.is_file():
        print("!! 找不到界面文件 %s" % html, file=sys.stderr)
        return 2
    aw, ah = _work_area()
    w, h = min(1040, max(760, aw - 60)), min(1050, max(560, ah - 46))
    win = webview.create_window("Virtualpet 烧录器", str(html), js_api=api,
                               width=w, height=h, min_size=(min(820, w), min(600, h)))
    holder[0] = win
    webview.start()          # 阻塞到窗口关闭
    return 0


if __name__ == "__main__":
    sys.exit(run())
