#!/usr/bin/env python3
"""串口侧设备操作 — 找板 / 抢窗等待 / 硬复位 / 抓串口。

用法 (库):
    from device import find_port, wait_port, hard_reset, capture, list_ports
    port = find_port()          # 303A:1001 (USB-Serial-JTAG); 找不到返回 None
    print(list_ports())         # 人看的端口清单

板子走 ESP32-S3 原生 USB-Serial-JTAG (VID 303A / PID 1001), Windows 10/11 免驱。
PID 4004 是同一个口的 U 盘模式, 此时不能烧录 — 单独标出来别让它冒充可烧录设备。
"""
import time

VID_ESPRESSIF = 0x303A
PID_USB_JTAG = 0x1001   # 可烧录
PID_USB_DISK = 0x4004   # U 盘模式, 不可烧录

DEFAULT_BAUD = 115200


class DeviceError(Exception):
    pass


def _serial():
    import serial  # 延迟导入: 缺 pyserial 时错误信息更清楚
    return serial


def list_ports() -> list:
    """[(device, vid, pid, role)] — role: flashable / disk / other"""
    import serial.tools.list_ports as lp

    out = []
    for p in lp.comports():
        vid, pid = getattr(p, "vid", None), getattr(p, "pid", None)
        if vid == VID_ESPRESSIF and pid == PID_USB_JTAG:
            role = "flashable"
        elif vid == VID_ESPRESSIF and pid == PID_USB_DISK:
            role = "disk"
        else:
            role = "other"
        out.append((p.device, vid, pid, role))
    return out


def describe_ports() -> str:
    rows = list_ports()
    if not rows:
        return "没有发现任何串口 — 板子插上了吗? 数据线能传数据吗 (不是纯充电线)?"
    lines = []
    for dev, vid, pid, role in rows:
        if role == "flashable":
            lines.append("  %-6s 可烧录   ESP32-S3 USB-Serial-JTAG (%04x:%04x)" % (dev, vid, pid))
        elif role == "disk":
            lines.append("  %-6s U盘模式  此模式不能烧录 (%04x:%04x)" % (dev, vid, pid))
        else:
            # 主板自带串口没有 USB ID — 直说, 别打成 (?:?)
            tag = "%04x:%04x" % (vid, pid) if vid and pid else "主板串口, 非 USB"
            lines.append("  %-6s 其它串口 (%s)" % (dev, tag))
    return "\n".join(lines)


def find_port() -> str:
    """返回可烧录的串口名; 有多个取第一个, 没有返回 None。"""
    for dev, _vid, _pid, role in list_ports():
        if role == "flashable":
            return dev
    return None


def find_all_flashable() -> list:
    return [d for d, _v, _p, r in list_ports() if r == "flashable"]


def has_disk_mode() -> bool:
    return any(r == "disk" for _d, _v, _p, r in list_ports())


def port_present(port: str) -> bool:
    import serial.tools.list_ports as lp
    return port in {p.device for p in lp.comports()}


def wait_port(timeout: float, want: str = None, poll: float = 0.1, on_tick=None):
    """等端口出现 (抢窗模式用: 复位后 ROM 窗口里才会枚举出来)。

    want 给定则只认这个口; 否则认任意可烧录口。
    返回端口名; 超时返回 None。
    """
    deadline = time.time() + timeout
    tick = 0
    while time.time() < deadline:
        if want:
            if port_present(want):
                return want
        else:
            p = find_port()
            if p:
                return p
        tick += 1
        if on_tick and tick % 10 == 0:
            on_tick(int(deadline - time.time()))
        time.sleep(poll)
    return None


def _set_rts(ser, state: bool):
    """置 RTS 并补一次 DTR 写操作。

    Windows usbser.sys 只在伴随 DTR 写操作时才会把 RTS 的新状态真正下发 —
    少了这一步 setRTS 不生效, 复位等于没做。(esptool 的 ResetStrategy._setRTS
    是同一套补偿。)
    """
    ser.setRTS(state)
    ser.setDTR(ser.dtr)


def open_serial(port: str, baud: int = DEFAULT_BAUD):
    """打开串口并保持 IO0 为高 (别把芯片带进下载模式)。"""
    serial = _serial()
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = 0.2
    ser.dtr = False   # DTR=IO0 保持高
    ser.rts = False
    ser.open()
    return ser


def hard_reset(port: str, baud: int = DEFAULT_BAUD):
    """硬复位到正常运行, 返回一个接着能读的句柄。

    RTS(=EN) 拉低不会让 USB 掉线 — USB-Serial-JTAG 外设复位后 CDC 链接还在
    (实测端口一次都不消失), 所以句柄别关: 不关就能从 ROM 之后第一行开始收,
    含 I(893) 那行 App version。旧实现复位后关掉重开, 那 40 次 comports()
    轮询在 Windows 上要 6 秒, 整段启动日志全丢在关口的空档里。
    """
    try:
        ser = open_serial(port, baud)
    except Exception as e:  # noqa: BLE001
        raise DeviceError("打不开串口 %s: %s" % (port, e))
    ser.setDTR(False)      # IO0=HIGH
    _set_rts(ser, True)    # EN=LOW
    time.sleep(0.1)
    _set_rts(ser, False)   # EN=HIGH
    return ser


def capture(ser, seconds: float, until=None, on_line=None) -> list:
    """从已打开的句柄抓行, 最多 seconds 秒; until(line) 为真则提前停。"""
    lines = []
    t0 = time.time()
    buf = b""
    while time.time() - t0 < seconds:
        try:
            chunk = ser.read(4096)
        except Exception:  # noqa: BLE001  句柄中途失效 — 抓到多少算多少
            break
        if not chunk:
            continue
        buf += chunk
        while b"\n" in buf:
            raw, buf = buf.split(b"\n", 1)
            line = raw.decode("utf-8", errors="replace").rstrip("\r")
            if not line.strip():
                continue
            lines.append(line)
            if on_line:
                on_line(line)
            if until and until(line):
                return lines
    return lines


def capture_after_reset(port: str, seconds: float = 6.0, until=None, on_line=None) -> list:
    """复位并从启动第一行开始抓 — 用于核对烧进去的版本号。"""
    ser = hard_reset(port)
    try:
        return capture(ser, seconds, until=until, on_line=on_line)
    finally:
        ser.close()
