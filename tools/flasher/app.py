#!/usr/bin/env python3
"""Virtualpet 专属烧录器 — 唯一入口。

    VirtualpetFlasher.exe                  不带动作 → 图形界面
    VirtualpetFlasher.exe --full ...       带动作   → 命令行

冻结成 exe 之后, 引擎要再调 esptool 就只能调自己 (没有 python 可用), 所以这里有一个
隐藏模式 `--run-esptool`: 把参数原样交给打包进来的 esptool 跑。必须在 import 任何
别的模块之前判掉, 否则一次烧录会白起一遍整个 GUI 依赖栈。
"""
import sys

# ── 内部模式: 自我再入当 esptool 用 (必须排在最前面) ────────────────────
if len(sys.argv) > 1 and sys.argv[1] == "--run-esptool":
    import esptool

    sys.argv = ["esptool"] + sys.argv[2:]
    esptool._main()          # FatalError→2 / SerialException→1, 与本工具退出码语义一致
    raise SystemExit(0)

import os        # noqa: E402
from pathlib import Path  # noqa: E402

# 同目录导入 (源码直跑与冻结包都成立)
_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))


def _utf8_console():
    """控制台可能是 GBK — 中文输出会直接崩, 先把三个流掰成 UTF-8。

    同时强制行缓冲: 输出重定向到文件时 Python 默认块缓冲, 一次慢读/慢烧的日志要等
    几 KB 才吐一次, 看起来就像卡住了 (产线把日志重定向到文件时尤其致命)。
    esptool 的进度行以 \\r 结尾、不带 \\n, 所以行缓冲不会让进度刷屏。
    """
    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(encoding="utf-8", errors="replace", line_buffering=True)
        except Exception:  # noqa: BLE001
            pass
    os.environ.setdefault("PYTHONIOENCODING", "utf-8")


def _hide_console():
    """GUI 模式把黑窗藏掉 (exe 必须是 console 子系统, 否则子进程拿不到 stdout)。"""
    if sys.platform != "win32" or not getattr(sys, "frozen", False):
        return
    try:
        import ctypes
        hwnd = ctypes.windll.kernel32.GetConsoleWindow()
        if hwnd:
            ctypes.windll.user32.ShowWindow(hwnd, 0)
    except Exception:  # noqa: BLE001
        pass


def main() -> int:
    _utf8_console()
    argv = sys.argv[1:]

    def gui_mode():
        _hide_console()
        import gui
        return gui.run()

    if not argv:                  # 双击 exe: 没有任何参数 → 图形界面
        return gui_mode()
    if argv[0] == "--gui":
        return gui_mode()

    import cli
    args = cli.build_parser().parse_args(argv)
    if cli.wants_gui(args):
        return gui_mode()
    return cli.main(argv)


if __name__ == "__main__":
    sys.exit(main())
