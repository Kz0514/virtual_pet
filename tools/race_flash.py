#!/usr/bin/env python3
"""抢窗烧录 — USB-Serial-JTAG 失效时的自救工具。

原理: ESP32-S3 的 USB-Serial-JTAG 在 ROM 启动窗口 (上电/复位后 ~1-2s, 应用
接管前) 会短暂枚举; 若应用抢占 PHY (或 PHY 卡在 OTG 态), 平时串口不存在,
只能在此窗口用 esptool --before usb_reset 抢入下载模式完成烧录/擦除。

用法:
    python tools/race_flash.py            # 等端口出现 → 全量烧录 (bootloader/分区/app/资源)
    python tools/race_flash.py --erase    # 等端口出现 → 仅擦除 flash
    python tools/race_flash.py --port COM7  # 指定已知端口 (跳过等待)

操作: 运行脚本后, 在设备上触发一次复位 (设置→关于→重启设备 / 硬件复位 /
拔插电源线), 脚本会在端口出现的瞬间抢窗执行。
"""
import serial.tools.list_ports as lp
import subprocess, sys, time, os, argparse

ESPT_PY = r"C:\Espressif\tools\python\v5.5.4\venv\Scripts\python.exe"
PROJ = r"C:\Users\c1364\Documents\esp-idf\Virtualpet"
FLASH_ARGS = ["--chip", "esp32s3", "-b", "460800",
              "--before", "usb_reset", "--after", "hard_reset"]


def find_esp_port():
    """USB-Serial-JTAG = VID 303A PID 1001 (U盘模式 PID 4004 不可烧录, 跳过)"""
    for p in lp.comports():
        if getattr(p, 'vid', None) == 0x303A and getattr(p, 'pid', None) == 0x1001:
            return p.device
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--erase", action="store_true", help="只擦除 flash, 不烧录")
    ap.add_argument("--port", help="已知端口则跳过等待直接执行")
    args = ap.parse_args()

    if args.port:
        port = args.port
        print(f"[race] 使用指定端口 {port}")
    else:
        print("[race] 等待 ESP32-S3 串口出现 (VID 303A, PID 1001)…")
        print("[race] 现在请在设备上触发复位 (设置→关于→重启设备 或 硬件复位/断电)")
        while True:
            port = find_esp_port()
            if port:
                print(f"[race] 发现 {port} — 抢窗执行")
                break
            time.sleep(0.1)

    cmd = [ESPT_PY, "-m", "esptool"] + FLASH_ARGS + ["-p", port]
    if args.erase:
        cmd.append("erase_flash")
    else:
        cmd += ["write_flash",     # esptool v4: --flash_mode 等是 write_flash 子命令的选项
                "--flash_mode", "dio", "--flash_size", "32MB", "--flash_freq", "80m",
                "0x0",      "build/bootloader/bootloader.bin",
                "0x8000",   "build/partition_table/partition-table.bin",
                "0xf000",   "build/ota_data_initial.bin",
                "0x20000",  "build/Virtualpet.bin",
                "0x500000", "build/assets.bin"]
    r = subprocess.run(cmd, cwd=PROJ)
    print(f"[race] 完成 rc={r.returncode} — 设备将重启进入新固件" if r.returncode == 0
          else f"[race] 失败 rc={r.returncode} — 可重试, 或检查设备是否已复位")
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
