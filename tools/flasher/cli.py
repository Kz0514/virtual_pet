#!/usr/bin/env python3
"""命令行入口 — argparse 全参数面 → Job → engine.run_job → 判定表 + 退出码。

模式判定: **有任一动作参数就是 CLI, 否则开 GUI**。所以脚本必须显式写动作,
永远不会因为漏参数而弹出一个窗口把产线卡住。

擦除类动作一律要 --yes: 不给就把后果清单打出来并 exit 2 (改好命令再重跑)。
退出码 0 成功 / 1 硬件或烧录失败 / 2 用法或环境错误。
"""
import argparse
import sys
import tempfile
import time
from pathlib import Path

import device
import engine
import images

ACTIONS = ("full", "app_only", "erase", "erase_all", "erase_otadata", "restore")
ERASE_CHOICES = ("otadata", "nvs", "config", "data", "assets")


class UsageError(Exception):
    pass


# ══════════════════════════════════════════════════════════════════════
def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="VirtualpetFlasher",
        description="Virtualpet 专属烧录器 — 不依赖 ESP-IDF 环境, 自带 esptool。\n"
                    "不带任何动作参数时打开图形界面。",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""示例:
  VirtualpetFlasher.exe                          打开图形界面
  VirtualpetFlasher.exe --selftest               不接板子检查这个包是否完好
  VirtualpetFlasher.exe --list-ports             看有哪些串口、能不能烧
  VirtualpetFlasher.exe --full --dry-run         只打印将要执行的命令, 不碰硬件
  VirtualpetFlasher.exe --full                   全烧 (bootloader + 分区表 + 主程序)
  VirtualpetFlasher.exe --app-only -p COM6       只烧主程序 (开发内循环)
  VirtualpetFlasher.exe --full --assets          连资源包一起烧
  VirtualpetFlasher.exe --erase data --yes       擦掉 /data (日记) — 危险
  VirtualpetFlasher.exe --erase data --yes --backup D:\\bak   擦前先备份
  VirtualpetFlasher.exe --restore D:\\bak\\data_xxx.bin --target data --yes

擦除是不可逆的: 不给 --yes 只打印后果清单, 不会动设备。""")
    g = p.add_argument_group("自检与信息")
    g.add_argument("--selftest", action="store_true", help="不接板子自检 (环境/镜像/断言/正则)")
    g.add_argument("--list-ports", action="store_true", help="列出串口并标注 可烧录 / U盘模式 / 其它")
    g.add_argument("--dry-run", action="store_true", help="只解析镜像并打印将执行的命令, 不碰硬件")

    g = p.add_argument_group("烧录动作")
    g.add_argument("--full", action="store_true", help="全烧: bootloader + 分区表 + 清 otadata + 主程序(双槽)")
    g.add_argument("--app-only", dest="app_only", action="store_true", help="只烧主程序 (默认连 ota_1 一起写)")
    g.add_argument("--assets", dest="assets", action="store_true", default=False,
                   help="附带烧 assets.bin (资源包, 约 26MB)")
    g.add_argument("--no-assets", dest="assets", action="store_false", help="不烧资源包 (默认)")
    g.add_argument("--no-ota1", dest="ota1", action="store_false", default=True,
                   help="只写 ota_0, 不写 ota_1 副本 (OTA 回滚会回到旧版本)")
    g.add_argument("--no-verify", dest="verify", action="store_false", default=True,
                   help="跳过写后回读校验 (不建议)")
    g.add_argument("--verify-all", dest="verify_all", action="store_true",
                   help="回读校验也覆盖 assets (慢, 26MB 要走一遍)")
    g.add_argument("--no-capture", dest="capture", action="store_false", default=True,
                   help="烧完不复位、不抓串口、不核对版本")

    g = p.add_argument_group("擦除与恢复 (--erase/--erase-all/--restore 均需 --yes)")
    g.add_argument("--erase", metavar="分区", help="擦除指定分区, 逗号分隔: " + ",".join(ERASE_CHOICES))
    g.add_argument("--erase-otadata", dest="erase_otadata", action="store_true",
                   help="只清 otadata (让设备回到 ota_0 槽启动)")
    g.add_argument("--erase-all", dest="erase_all", action="store_true", help="整片擦除 32MB (擦完须重烧)")
    g.add_argument("--backup", metavar="目录", help="擦除前把目标分区整区读出存到这个目录")
    g.add_argument("--restore", metavar="文件", help="把备份写回设备 (需配合 --target)")
    g.add_argument("--target", metavar="分区", help="--restore 的目标分区")

    g = p.add_argument_group("提取分区 (把板上区域读成 bin 文件)")
    g.add_argument("--dump", metavar="分区", action="append",
                   help="提取指定分区, 逗号分隔可多个: bootloader,partition-table,otadata,"
                        "ota_0,ota_1,assets,config,data 或 all (整片 32MB)")
    g.add_argument("--dump-out", metavar="目录", help="提取文件的存放目录 (默认 ./dump)")
    g.add_argument("--list-partitions", dest="list_partitions", action="store_true",
                   help="列出这块板上实际的分区表 (从芯片读, 不依赖镜像目录)")

    g = p.add_argument_group("设备")
    g.add_argument("-p", "--port", help="指定串口 (如 COM6); 不给则自动发现并等待")
    g.add_argument("--wait", type=float, default=60.0, metavar="N", help="等设备出现的秒数, 0=不等待 (默认 60)")
    g.add_argument("--race", action="store_true", help="抢窗模式: 用 USB 复位硬抢, 适用于板子已经卡死")
    g.add_argument("--baud", type=int, default=921600, help="首档波特率 (默认 921600)")
    g.add_argument("--baud-ladder", metavar="A,B,C", help="降级阶梯 (默认 921600,460800,115200)")
    g.add_argument("--no-prompt", dest="prompt", action="store_false", default=True,
                   help="卡死态不询问, 直接失败 (产线/脚本用)")

    g = p.add_argument_group("镜像")
    g.add_argument("--bin-dir", metavar="PATH", help="镜像目录 (默认自动找 images/ 或 build/)")
    g.add_argument("--flash-mode", default=None, choices=("dio", "qio", "dout", "qout"), help="覆盖 flash 模式")
    g.add_argument("--flash-freq", default=None, help="覆盖 flash 频率, 如 80m")
    g.add_argument("--flash-size", default=None, help="覆盖 flash 容量, 如 32MB")

    g = p.add_argument_group("用户数据取证")
    g.add_argument("--protect-dump", metavar="FILE", help="读 nvs/config/data 三段存 md5 基线到 FILE")
    g.add_argument("--protect-compare", metavar="FILE", help="读同样三段与基线 FILE 比对")
    g.add_argument("--protect-check", action="store_true",
                   help="烧前自动留基线、烧后自动比对 (证明工具没碰用户数据)")

    g = p.add_argument_group("其它")
    g.add_argument("--yes", action="store_true", help="跳过交互确认 (擦除类动作必须给)")
    g.add_argument("--verbose", action="store_true", help="打印 esptool 的全部原始输出")
    g.add_argument("--gui", action="store_true", help="强制打开图形界面")
    p.add_argument("--run-esptool", action="store_true", help=argparse.SUPPRESS)
    return p


# 这些参数自己就是一次完整的命令行动作 (不烧录但会碰硬件/本地) —
# 写了就绝不开窗, 否则脚本里会静默卡在一个没人看的窗口上
CLI_ONLY = ("selftest", "list_ports", "dry_run", "protect_dump", "protect_compare",
            "dump", "list_partitions")


def wants_gui(args) -> bool:
    if args.gui:
        return True
    if any(getattr(args, a) for a in CLI_ONLY):
        return False
    return not any(getattr(args, a) for a in ACTIONS)


# ══════════════════════════════════════════════════════════════════════
def parse_erase(text: str) -> list:
    out = []
    for raw in (text or "").replace("，", ",").split(","):
        name = raw.strip().lower()
        if not name:
            continue
        if name in engine.ERASE_DENIED:
            raise UsageError("不提供 %s 的单独擦除: %s" % (name, engine.ERASE_DENIED[name]))
        if name not in ERASE_CHOICES:
            raise UsageError("未知分区 %r (可选: %s)" % (name, ", ".join(ERASE_CHOICES)))
        if name not in out:
            out.append(name)
    return out


def parse_dump(args) -> list:
    """--dump 可重复也可逗号分隔; 标签合法性在连上板子后按板上分区表再验一次。"""
    out = []
    for text in (args.dump or []):
        for raw in text.replace("，", ",").split(","):
            name = raw.strip().lower().replace("_", "-")
            if not name:
                continue
            if name in engine.DUMP_DENIED:
                raise UsageError("不提供 %s 的提取: %s" % (name, engine.DUMP_DENIED[name]))
            if name not in out:
                out.append(name)
    return out


def dump_warning(labels, out_dir) -> str:
    """提取前的后果说明 — 含凭据的目标单独点名。"""
    lines = ["提取会把板上区域读出来存成文件:"]
    for label in labels:
        ent = engine.DUMP_SENSITIVE.get(label)
        if ent:
            lines.append("  !! %s 含%s — 产出文件等同凭据, 切勿外传" % (label, ent[0]))
        else:
            lines.append("  %s" % label)
    lines.append("")
    lines.append("存到: %s" % out_dir)
    sens = [l for l in labels if l in engine.DUMP_SENSITIVE]
    if sens:
        lines.append("")
        lines.append("确认无误就加上 --yes 重跑 (含凭据的 %s 还需确认词「%s」):"
                     % (",".join(sens), engine.dump_phrase(sens[0])))
    else:
        lines.append("")
        lines.append("确认无误就加上 --yes 重跑:")
    return "\n".join(lines)


def print_erase_warning(targets, erase_all, restore=None):
    """不给 --yes 时打印后果清单 — 这是唯一的"再想一次"机会。"""
    print(">>> 这是不可逆操作, 会永久丢掉设备上的数据。将要执行:")
    if restore:
        dst = images.FALLBACK_PARTITIONS.get(restore[1], (None, None))[0]
        print("  写回      %-9s ← %s%s" % (restore[1], Path(restore[0]).name,
                                          " (0x%x)" % dst if dst is not None else ""))
    for label in targets:
        what, conseq, _lvl = engine.ERASE_TARGETS[label]
        off = images.FALLBACK_PARTITIONS.get(label, (None,))[0]
        print("  擦除      %-9s %-11s %s" % (label, "0x%x" % off if off is not None else "",
                                            what))
        print("            %s" % conseq)
    if erase_all:
        what, conseq, _lvl = engine.ERASE_TARGETS["all"]
        print("  整片擦除  %-9s %s" % ("0x0", what))
        print("            %s" % conseq)
    print("\n确认无误就加上 --yes 重跑:")
    print("  " + " ".join(['"%s"' % sys.argv[0]] + sys.argv[1:] + ["--yes"]))


def make_job(args, sink):
    """参数 → Job (含镜像解析与各类闸门)。"""
    iss = None
    need_images = args.full or args.app_only
    if need_images:
        iss = images.load(bin_dir=args.bin_dir, verbose=args.verbose)
    elif not (args.erase or args.erase_all or args.erase_otadata or args.restore
              or args.protect_dump or args.protect_compare
              or args.dump or args.list_partitions):
        # 取证与提取都是独立只读动作: 不烧不擦也能跑 (给这块板此刻的状态留个底)
        raise UsageError("没有指定任何动作 — 见 --help")

    job = engine.Job(images=iss, port=args.port, race=args.race, wait=args.wait,
                     verify=args.verify, verify_all=args.verify_all,
                     dry_run=args.dry_run, verbose=args.verbose,
                     scripted=not args.prompt, backup_dir=Path(args.backup) if args.backup else None)
    job.ladder = parse_ladder(args.baud_ladder) if args.baud_ladder else _ladder(args.baud)
    if iss:
        mode = "full" if args.full else "app"
        job.ops = iss.ops(mode=mode, with_assets=args.assets, dual_slot=args.ota1)
        if not job.ops:
            raise UsageError("解析出来的写集合是空的 — 镜像目录 %s 里没有可用镜像" % iss.root)
        for attr, key in (("flash_mode", "flash_mode"), ("flash_freq", "flash_freq"),
                          ("flash_size", "flash_size")):
            v = getattr(args, key)
            if v:
                setattr(job, attr, v)
        # 全烧要保证 otadata 是干净的; 有 ota_data_initial.bin 就写它, 没有就擦掉
        if args.full and "otadata" not in iss.images:
            off, size = images.FALLBACK_PARTITIONS["otadata"]
            job.erase.append(("otadata", off, size, engine.confirm_phrase("otadata")))
            sink.log(">>> 镜像里没有 ota_data_initial.bin — 全烧改为清空 otadata (设备回到 ota_0 启动)")

    if args.erase_otadata:
        job.erase += engine.erase_plan(["otadata"], confirmed=True)
        sink.log(">>> 只清 otadata: 设备下次启动回到 ota_0 槽")

    # 擦除确认闸门: 没 --yes 就只打印后果, 令牌一个都不发
    erase_list = parse_erase(args.erase)
    if (erase_list or args.erase_all or args.restore) and not (args.yes or args.dry_run):
        print_erase_warning(erase_list, args.erase_all,
                            restore=(args.restore, args.target) if args.restore else None)
        raise UsageError("缺少 --yes — 已列出后果, 确认后重跑")

    if erase_list:
        job.erase += engine.erase_plan(erase_list, confirmed=True)
    if args.erase_all:
        job.erase_all = True
        job.erase_token = engine.confirm_phrase("all")

    if args.restore:
        if not args.target:
            raise UsageError("--restore 要配合 --target 指明写到哪个分区")
        if not Path(args.restore).is_file():
            raise UsageError("备份文件不存在: %s" % args.restore)
        job.restore_file, job.restore_target = Path(args.restore), args.target

    # 提取: 只读, 但含凭据的目标要额外确认词 (引擎层再验一次)
    dump_list = parse_dump(args)
    job.dump_dir = Path(args.dump_out) if args.dump_out else Path("dump")
    if dump_list and not (args.yes or args.dry_run):
        print(dump_warning(dump_list, job.dump_dir))
        raise UsageError("缺少 --yes — 已列出后果, 确认后重跑")
    job.dump = dump_list
    job.dump_confirmed = bool(args.yes)
    job.list_partitions = bool(args.list_partitions)

    # 取证 — 三个开关各自独立:
    #   --protect-dump F   读基线存 F (本身就是动作, 不依赖 --protect-check)
    #   --protect-compare F 读同样三段与 F 比对
    #   --protect-check    烧前/烧后自动做上面两件事 (F 缺省落到临时目录)
    if args.dry_run:
        job.protect_before = False
    else:
        auto = bool(args.protect_check)
        base = Path(args.protect_dump) if args.protect_dump else (
            Path(tempfile.gettempdir()) / ("vp_protect_%s.json" % time.strftime("%Y%m%d_%H%M%S"))
            if auto else None)
        job.protect_dump = base
        job.protect_compare = Path(args.protect_compare) if args.protect_compare else (
            base if auto else None)
        # 有基线要读就得真读 — 光给 --protect-dump 却没开 check 也要落盘
        job.protect_before = bool(job.protect_dump)
        if base:
            sink.log(">>> 用户数据取证: 基线 %s" % base)
    if not args.capture:
        job.capture_seconds = 0.0
    return job


def _ladder(first_baud) -> tuple:
    steps = [first_baud]
    for b in engine.DEFAULT_LADDER:
        if b < first_baud:
            steps.append(b)
    return tuple(steps)


def parse_ladder(text) -> tuple:
    out = []
    for raw in text.replace("，", ",").split(","):
        raw = raw.strip()
        if not raw:
            continue
        try:
            out.append(int(raw))
        except ValueError:
            raise UsageError("波特率阶梯里 %r 不是数字" % raw)
    if not out:
        raise UsageError("--baud-ladder 是空的")
    return tuple(out)


def _die(msg):
    """先冲干净 stdout, 免得 2>&1 时错误行插到后果清单前面去。"""
    sys.stdout.flush()
    print("!! %s" % msg, file=sys.stderr)
    sys.stderr.flush()


def print_verdict(res: engine.Result):
    print("\n=== 结果 ===")
    for name, status, detail in res.steps:
        print("  %-5s %-12s %s" % (status, name, detail))
    if res.error:
        print("\n!! %s" % res.error)
    if res.need_power:
        print("!! 需要断电: 拔掉电源等 3 秒再插上")


# ══════════════════════════════════════════════════════════════════════
def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    sink = engine.ConsoleSink(scripted=not args.prompt, verbose=args.verbose)

    if args.selftest:
        import selftest
        return selftest.run(bin_dir=args.bin_dir, verbose=args.verbose)
    if args.list_ports:
        print(device.describe_ports())
        return 0

    try:
        job = make_job(args, sink)
    except UsageError as e:
        _die(e)
        return 2
    except images.UnsafeImageSet as e:
        _die("镜像不安全, 已拒绝: %s" % e)
        return 2
    except engine.EngineError as e:
        _die(e)
        return 2

    if job.images:
        print(job.images.report())
    res = engine.run_job(job, sink)
    if job.dry_run:
        return res.exit_code
    print_verdict(res)
    return res.exit_code


if __name__ == "__main__":
    sys.exit(main())
