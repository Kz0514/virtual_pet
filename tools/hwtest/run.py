#!/usr/bin/env python3
"""hwtest 一键整板自检: build → flash → 抓串口 → 判定表 → 退出码

用法 (任何 python 都行, 缺 pyserial 会自动切到 IDF 环境 python 重跑):
    python tools/hwtest/run.py                      # COM9, 只扫描不连 WiFi
    python tools/hwtest/run.py -p COM6              # 换板
    python tools/hwtest/run.py --ssid MyAP --wifi-pass-file pass.txt   # 带连接校验
    python tools/hwtest/run.py --allow-no-wifi      # 屏蔽房: 扫不到 AP 不算 FAIL
    python tools/hwtest/run.py --no-flash           # 只抓串口 (板子已经在跑自检)
    python tools/hwtest/run.py --full-flash         # 新板: 连 bootloader/分区表一起烧

判据: 退出码 0 = 无 FAIL; 1 = 有 FAIL; 2 = 没收到结果 (超时/没启动)。
凭据纪律: 密码只从文件读 (--wifi-pass-file), 不进命令行/进程列表。
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time
import unicodedata
from datetime import datetime

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import build  # noqa: E402  同目录: 复用 MSYS 规避 + 环境变量拼装

DEFAULT_PORT = "COM9"
BAUD = 115200
HWTEST_RE = re.compile(r"^HWTEST (\{.*\})\s*$")


def _has_serial():
    try:
        import serial  # noqa: F401
        return True
    except Exception:
        return False


# ── 缺 pyserial → 用 IDF 环境 python 重跑自己 (凭据不落参数以外的地方) ──
if not _has_serial() and os.environ.get("HWTEST_REEXEC") != "1":
    _env = build.idf_env()
    _env["HWTEST_REEXEC"] = "1"
    sys.stderr.write(">>> 当前 python 没有 pyserial, 切到 IDF 环境 python 重跑\n")
    os.execve(build.PY, [build.PY, os.path.abspath(__file__)] + sys.argv[1:], _env)


# ══════════════════════════════════════════════════════════════════════
# hw_secrets.h 生成 (凭据只从文件读)
# ══════════════════════════════════════════════════════════════════════
def write_secrets(ssid: str, password: str, allow_no_wifi: bool, skip_wifi: bool):
    src = os.path.join(HERE, "main", "hw_secrets.h.in")
    dst = os.path.join(HERE, "main", "hw_secrets.h")
    with open(src, "r", encoding="utf-8") as f:
        text = f.read()
    text = text.replace('HWTEST_WIFI_SSID ""', 'HWTEST_WIFI_SSID "%s"' % ssid)
    text = text.replace('HWTEST_WIFI_PASS ""', 'HWTEST_WIFI_PASS "%s"' % password)
    text = re.sub(r"#define HWTEST_SKIP_WIFI \d", "#define HWTEST_SKIP_WIFI %d" % (1 if skip_wifi else 0), text)
    text = re.sub(
        r"#define HWTEST_ALLOW_NO_WIFI \d",
        "#define HWTEST_ALLOW_NO_WIFI %d" % (1 if allow_no_wifi else 0),
        text,
    )
    with open(dst, "w", encoding="utf-8") as f:
        f.write(text)
    return dst


def read_pass_file(path: str) -> str:
    with open(path, "r", encoding="utf-8") as f:
        return f.read().strip()


# ══════════════════════════════════════════════════════════════════════
# 构建 / 烧录
# ══════════════════════════════════════════════════════════════════════
def idf(args, timeout=None):
    cmd = [build.PY, os.path.join(build.IDF_PATH, "tools", "idf.py")] + args
    print(">>>", " ".join(cmd), flush=True)
    return subprocess.call(cmd, env=build.idf_env(), cwd=build.PROJ, timeout=timeout)


def app_size():
    p = os.path.join(HERE, "build", "hwtest.bin")
    return os.path.getsize(p) if os.path.exists(p) else None


# ══════════════════════════════════════════════════════════════════════
# 串口抓取
# ══════════════════════════════════════════════════════════════════════
def capture(port: str, timeout_s: float, reset: bool, log_path: str):
    import serial

    ser = serial.Serial()
    ser.port = port
    ser.baudrate = BAUD
    ser.timeout = 0.2
    ser.dtr = False
    ser.rts = False
    ser.open()
    if reset:
        # 硬复位进入正常运行 (照 esptool 的 seq: IO0 高, EN 脉冲)
        ser.setDTR(False)
        ser.setRTS(True)
        time.sleep(0.1)
        ser.setRTS(False)
        time.sleep(0.05)

    lines, payload = [], None
    t0 = time.time()
    buf = b""
    with open(log_path, "w", encoding="utf-8") as log:
        while time.time() - t0 < timeout_s:
            chunk = ser.read(4096)
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("utf-8", errors="replace").rstrip("\r")
                if not line.strip():
                    continue
                lines.append(line)
                log.write(line + "\n")
                log.flush()
                print(line, flush=True)
                m = HWTEST_RE.match(line.strip())
                if m:
                    payload = m.group(1)
                    break
            if payload:
                break
    ser.close()
    return payload, lines


# ══════════════════════════════════════════════════════════════════════
# 判定表
# ══════════════════════════════════════════════════════════════════════
def disp_w(s: str) -> int:
    return sum(2 if unicodedata.east_asian_width(c) in "WF" else 1 for c in s)


def pad(s: str, w: int) -> str:
    d = disp_w(s)
    return s + " " * (w - d) if d < w else s


def print_table(items):
    print()
    print(pad("#", 4) + pad("id", 17) + pad("名称", 20) + pad("状态", 7) + "值 / 备注")
    print("─" * 110)
    for it in items:
        v = it.get("v", "")
        note = it.get("note", "")
        tail = v + ("  ← " + note if note else "")
        print(
            pad(str(it.get("i", 0)), 4)
            + pad(it["id"], 17)
            + pad(it.get("name", ""), 20)
            + pad(it["st"], 7)
            + tail
        )


def main():
    ap = argparse.ArgumentParser(description="hwtest 整板硬件自检")
    ap.add_argument("-p", "--port", default=DEFAULT_PORT, help="串口 (默认 %s)" % DEFAULT_PORT)
    ap.add_argument("--ssid", default="", help="要做连接校验的 SSID (不给则只扫描)")
    ap.add_argument("--wifi-pass-file", default="", help="WiFi 密码文件 (密码不进命令行)")
    ap.add_argument("--allow-no-wifi", action="store_true", help="扫不到 AP 记 WARN 不算 FAIL")
    ap.add_argument("--skip-wifi", action="store_true", help="整段 WiFi 直接 SKIP")
    ap.add_argument("--no-build", action="store_true", help="不重新构建, 用现有 build/")
    ap.add_argument("--no-flash", action="store_true", help="不烧录, 只抓串口")
    ap.add_argument("--full-flash", action="store_true", help="连 bootloader/分区表一起烧 (新板)")
    ap.add_argument("--no-reset", action="store_true", help="不自动复位 (自己按 RST)")
    ap.add_argument("--timeout", type=float, default=300.0, help="抓串口最长秒数 (默认 300)")
    args = ap.parse_args()

    os.makedirs(os.path.join(HERE, "logs"), exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    log_path = os.path.join(HERE, "logs", "hwtest_%s_%s.log" % (args.port.replace("/", "_"), stamp))

    # 1. 凭据 (给了任一 WiFi 开关就重写 hw_secrets.h, 否则不动用户手改的文件)
    if args.ssid or args.allow_no_wifi or args.skip_wifi or not os.path.exists(
        os.path.join(HERE, "main", "hw_secrets.h")
    ):
        pw = read_pass_file(args.wifi_pass_file) if args.wifi_pass_file else ""
        if args.ssid and not args.wifi_pass_file:
            print(">>> 只给了 SSID 没给密码文件: 按开放网络处理")
        write_secrets(args.ssid, pw, args.allow_no_wifi, args.skip_wifi)
        print(">>> 已写 main/hw_secrets.h (ssid=%s)" % (args.ssid or "无"))

    # 2. 构建
    if not args.no_build and not args.no_flash:
        rc = idf(["build"])
        if rc != 0:
            print("!! 构建失败")
            return 2
        sz = app_size()
        if sz:
            print(">>> app 大小 %.2f MB%s" % (sz / 1048576.0, "  ⚠️ 超 2MB 装不进 ota_0!" if sz >= 0x200000 else ""))
            if sz >= 0x200000:
                return 2

    # 3. 烧录
    if not args.no_flash:
        action = "flash" if args.full_flash else "app-flash"
        if idf(["-p", args.port, action]) != 0:
            print("!! 烧录失败 (串口被占? 板子没插?)")
            return 2

    # 4. 抓串口
    print(">>> 抓 %s @%d, 最长 %.0fs (日志 %s)" % (args.port, BAUD, args.timeout, log_path))
    try:
        payload, lines = capture(args.port, args.timeout, not args.no_reset, log_path)
    except Exception as e:
        print("!! 打不开串口 %s: %s" % (args.port, e))
        return 2

    if not payload:
        print("\n!! 没收到 HWTEST 结果行 (超时 %.0fs, 共 %d 行)" % (args.timeout, len(lines)))
        print("   排查: 板子真在跑自检吗? 波特率/串口对吗? 试 --no-reset 手动按 RST")
        return 2

    # 5. 判定表
    res = json.loads(payload)
    if res.get("trunc"):
        print("\n!! JSON 被截断 (少 %d 项) — 串口行超长? 结果不可信, 别当全绿" % res["trunc"])
        return 2
    items = res.get("items", [])
    print_table(items)
    print("─" * 110)
    print(
        "fw=%s  共 %d 项 | PASS %d | FAIL %d | WARN %d | SKIP %d"
        % (res.get("fw", "?"), res.get("n", len(items)), res.get("pass", 0), res.get("fail", 0),
           res.get("warn", 0), res.get("skip", 0))
    )
    fails = [it for it in items if it["st"] == "FAIL"]
    if fails:
        print("\n✘ 失败项:")
        for it in fails:
            print("  [%02d] %s %s | %s %s" % (it["i"], it["id"], it.get("name", ""), it.get("v", ""), it.get("note", "")))
        print("\n日志: %s" % log_path)
        return 1
    print("\n✔ 无 FAIL。日志: %s" % log_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
