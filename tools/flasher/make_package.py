#!/usr/bin/env python3
"""一条命令产出可外发的烧录器分发包。

    C:/Espressif/tools/python/python.exe tools/flasher/make_package.py --with-assets
    C:/Espressif/tools/python/python.exe tools/flasher/make_package.py --skip-build   # 只验拷贝+闸门+zip

产出: dist/flasher/VirtualpetFlasher_<版本>[_assets].zip (+ .sha256)

不许做的事 (都是有理由的):
  * 不许整目录拷贝 — PROJECT_MAP.md 里有服务器明文密码, 这里**不 import、不读取**它,
    凡是要进包的文件一律逐条白名单列出。
  * 不许用 IDF venv 的 python — 那套里没有 PyInstaller, 而且 esptool 是 4.x (语法不同)。
  * 不许把 --workpath 留在默认位置 — PyInstaller 默认 ./build/<name> 会污染 IDF 的 build/。
"""
import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import time
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
BUILD = REPO / "build"
DIST = REPO / "dist" / "flasher"
WORK = REPO / "tmp" / "flasher_build"
STAGE_ROOT = DIST / "pkg"

BASE_PY = Path(r"C:\Espressif\tools\python\python.exe")   # 打包与运行共用这一套
APP_NAME = "VirtualpetFlasher"
TOOL_VERSION = "1.1"

# 进 images/ 的文件: 相对 build/ 的路径 → 是否随 --with-assets 才带
IMAGES = {
    "Virtualpet.bin": True,                    # 主程序, 永远带
    "bootloader/bootloader.bin": True,
    "partition_table/partition-table.bin": True,
    "ota_data_initial.bin": True,
    "flasher_args.json": True,
    "assets.bin": False,                       # 26MB, 只在 --with-assets 时带
}
# 包根目录平铺的文件 (与 images/ 同级)
ROOT_FILES = {"version.txt": "版本号 (供交叉校验)"}
# 顶层只允许这些东西 — 白名单精确匹配, 不做名字猜测
ALLOW_TOP = {APP_NAME + ".exe", "_internal", "images"} | set(ROOT_FILES) | {"使用说明.txt"}
# 全树扫描: 名字里出现这些词一律中止 (PROJECT_MAP.md 含服务器明文密码, 是首要目标)
BANNED_NAMES = ("project_map.md", "hw_secrets", "sdkconfig", "secrets", "credential", "password")
REAL_SECRET_SUFFIX = (".env", ".pem", ".key", ".log", ".pdb", ".pfx", ".p12")
VENDOR_DOCS = ("readme.md", "license.md", "license.txt")   # 第三方包里自带的说明, 无敏感内容


class Gate(Exception):
    """闸门失败 — 一律中止, 不带病出包。"""


def md5(p: Path) -> str:
    h = hashlib.md5()
    with open(p, "rb") as f:
        for blk in iter(lambda: f.read(1 << 20), b""):
            h.update(blk)
    return h.hexdigest()


def sha256(p: Path) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for blk in iter(lambda: f.read(1 << 20), b""):
            h.update(blk)
    return h.hexdigest()


def app_version() -> str:
    """版本以镜像头为准 (和烧录器运行时用的是同一个来源)。"""
    p = BUILD / "Virtualpet.bin"
    if not p.is_file():
        raise Gate("找不到 %s — 先构建" % p)
    head = p.read_bytes()[:0xB0]
    if struct.unpack_from("<I", head, 0x20)[0] != 0xABCD5432:
        raise Gate("%s 不是 ESP 应用镜像" % p)
    return head[0x30:0x50].split(b"\x00")[0].decode("utf-8", "replace")


def git_commit() -> str:
    try:
        return subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=REPO,
                              capture_output=True, text=True, timeout=20).stdout.strip()
    except Exception:  # noqa: BLE001
        return ""


# ══════════════════════════════════════════════════════════════════════
def gate_version(ver: str):
    """版本闸门 — zip 名和说明文件都要写版本号, 所以这里比运行时更严。"""
    vf = REPO / "version.txt"
    if not vf.is_file():
        raise Gate("仓库根没有 version.txt")
    repo_v = vf.read_text("utf-8").strip()
    if repo_v != ver:
        raise Gate("版本不一致: 镜像里是 %s, version.txt 是 %s — "
                   "要么改完代码忘了递增版本, 要么改了版本没重新构建" % (ver, repo_v))
    print("PASS 版本闸门     镜像 %s = version.txt" % ver)


def gate_sources(with_assets: bool):
    """逐条确认源文件都在 build/ 底下 — 防止白名单被改成指向仓库别处。"""
    picked = {}
    for rel, always in IMAGES.items():
        if rel == "assets.bin" and not with_assets:
            continue
        src = (BUILD / rel).resolve()
        if not str(src).startswith(str(BUILD.resolve()) + os.sep):
            raise Gate("%s 不在 build/ 目录下, 拒绝拷贝" % src)
        if not src.is_file():
            if rel == "assets.bin":
                raise Gate("要带 assets 但 %s 不存在 — 先构建资源包" % src)
            raise Gate("缺文件 %s" % src)
        picked[rel] = src
    for name in ROOT_FILES:
        src = (REPO / name).resolve()
        if not str(src).startswith(str(REPO.resolve()) + os.sep):
            raise Gate("%s 不在仓库里" % src)
        if not src.is_file():
            raise Gate("缺文件 %s" % src)
    print("PASS 源文件白名单   %d 个镜像 + %d 个根文件" % (len(picked), len(ROOT_FILES)))
    return picked


def build_exe(clean: bool):
    if not BASE_PY.is_file():
        raise Gate("找不到打包用的 python: %s" % BASE_PY)
    cmd = [str(BASE_PY), "-m", "PyInstaller", "--noconfirm",
           "--name", APP_NAME, "--onedir", "--console",
           "--paths", str(HERE),
           "--collect-data", "esptool",              # stub json 是包数据; 少了它一烧就崩
           "--collect-submodules", "esptool",
           "--hidden-import", "serial.tools.list_ports_windows",
           "--exclude-module", "numpy", "--exclude-module", "PIL",
           "--exclude-module", "matplotlib", "--exclude-module", "pytest",
           "--exclude-module", "setuptools", "--exclude-module", "pip",
           # webview 只在 __generate_ssl_cert() 里懒 import 这几个 (自签证书用),
           # 我们从不起 https 服务 → 排掉能省下约 19MB (cryptography + OpenSSL 两个 dll)
           "--exclude-module", "cryptography", "--exclude-module", "bcrypt",
           "--exclude-module", "paramiko", "--exclude-module", "nacl",
           "--noupx",                                # 加壳会招杀软误报
           "--add-data", "%s;." % (HERE / "gui.html"),
           # specpath 也得挪走: 默认写到 CWD, 每次打包在仓库根丢一个 .spec
           "--distpath", str(DIST), "--workpath", str(WORK), "--specpath", str(WORK),
           str(HERE / "app.py")]
    if clean:
        cmd.insert(3, "--clean")     # 放 --noconfirm 后面
    print(">>> PyInstaller …")
    r = subprocess.run(cmd, cwd=str(REPO))
    if r.returncode != 0:
        raise Gate("PyInstaller 失败 (rc=%d)" % r.returncode)
    exe = DIST / APP_NAME / ("%s.exe" % APP_NAME)
    if not exe.is_file():
        raise Gate("打包完了却没看到 %s" % exe)
    print("PASS 打包          %s" % exe)
    return exe


def stage(stage: Path, exe: Path, picked: dict, ver: str, with_assets: bool, skip_build: bool):
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)

    if not skip_build:
        # PyInstaller 的输出目录整个搬过去 (会随后被敏感文件扫描兜底)
        shutil.copytree(exe.parent, stage, dirs_exist_ok=True)

    imgs = stage / "images"
    imgs.mkdir()
    files = {}
    for rel, src in picked.items():
        dst = imgs / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
        files["images/" + rel] = {"size": dst.stat().st_size, "md5": md5(dst)}
    for name in ROOT_FILES:
        src = REPO / name
        shutil.copy2(src, stage / name)
        files[name] = {"size": (stage / name).stat().st_size, "md5": md5(stage / name)}

    manifest = {
        "tool": APP_NAME, "tool_version": TOOL_VERSION,
        "image_version": ver, "project_name": "Virtualpet",
        "idf_ver": _idf_ver(), "built_at": time.strftime("%Y-%m-%d %H:%M:%S"),
        "git_commit": git_commit(), "assets_included": with_assets,
        "python": sys.version.split()[0], "files": files,
    }
    (imgs / "manifest.json").write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False), "utf-8")
    (stage / "使用说明.txt").write_text(readme_text(ver, with_assets, manifest), "utf-8")
    print("PASS 打包目录      %s" % stage)
    return manifest


def _idf_ver() -> str:
    try:
        sys.path.insert(0, str(HERE))
        import images
        return images.load(bin_dir=BUILD).app.idf_ver
    except Exception:  # noqa: BLE001
        return ""


def gate_sensitive(stage: Path):
    """两道独立的检查:

    * 顶层是我们自己铺的 → 用**精确白名单**, 多一个文件都不行。
      对自产内容用文件名启发式太弱 (PROJECT_MAP.md 只是众多 .md 之一)。
    * `_internal/` 是 PyInstaller 的输出 → 只查真凭据名与后缀, 放行厂商自带的
      README/LICENSE.md (esptool 的 stub 目录里就带着 README.md, 那是必须留的)。
    """
    top = {p.name for p in stage.iterdir()}
    extra = top - ALLOW_TOP
    if extra:
        raise Gate("分发包顶层出现不该有的东西: %s\n(只允许 %s)"
                   % (sorted(extra), sorted(ALLOW_TOP)))

    imgs = stage / "images"
    got = {str(p.relative_to(imgs)).replace("\\", "/")
           for p in imgs.rglob("*") if p.is_file()}
    want = {r for r in IMAGES if (imgs / r).is_file()}
    want |= {"manifest.json"}
    if got != want:
        raise Gate("images/ 里的文件与白名单不符:\n    多出 %s\n    缺少 %s"
                   % (sorted(got - want), sorted(want - got)))

    bad = []
    for p in stage.rglob("*"):
        if not p.is_file():
            continue
        low = p.name.lower()
        if any(b in low for b in BANNED_NAMES) or low.endswith(REAL_SECRET_SUFFIX):
            bad.append(p.relative_to(stage))
        elif low.endswith(".md") and low not in VENDOR_DOCS:
            bad.append(p.relative_to(stage))
    if bad:
        raise Gate("分发包里出现可疑文件, 已中止:\n    " +
                   "\n    ".join(str(b) for b in bad[:20]))
    print("PASS 敏感文件扫描   顶层 %d 项精确匹配, 全树 %d 个文件无一可疑"
          % (len(top), len(list(stage.rglob("*")))))


def readme_text(ver: str, with_assets: bool, manifest: dict) -> str:
    z = manifest["files"]
    return """Virtualpet 烧录器 — 使用说明
======================================

固件版本: %s
资源包:   %s
构建时间: %s
git:      %s

这个包不需要装 ESP-IDF, 也不需要装 Python。解压后双击 exe 即可。


三步操作
--------
1. 用 USB 线把设备插到电脑上
2. 双击 %s.exe (不带参数就会打开图形界面)
3. 选好要烧的内容, 点「开始烧录」


想换固件?
---------
把这个文件夹里 images/ 下的 Virtualpet.bin 换成新的就行, 不用重新打包。
文件名必须保持 Virtualpet.bin — 烧录器按文件名认角色, 并会核对文件内容与
名字是否相符 (不符会拒绝烧, 免得把资源包当主程序写进去)。

想连资源包一起烧?
---------------
本包%s资源包 (26MB 的动画/音效/语音模型)。设备上已经有资源包的话, 平时只烧
主程序就行, 资源包不用重烧。要重烧, 把 assets.bin 放进 images/ 再重新打开 —
界面上「附带资源包 assets」那个选项会自动变为可勾选 (灰着就是包里没有它)。

images/ 目录里各文件的 md5 (核对用):
%s

命令行也能用 (自动化/产线):
  %s.exe --selftest            不接设备, 检查这个包是否完好
  %s.exe --list-ports          看有哪些串口、能不能烧
  %s.exe --full                全烧
  %s.exe --app-only            只烧主程序
  %s.exe --list-partitions     列出这块板上真实的分区表
  %s.exe --dump data --yes     把某个分区读成 bin 文件 (只读, 留个底)
  %s.exe --erase data --yes    擦掉 /data (日记) — 危险, 不可逆


找不到设备?
-----------
* 确认用的是数据线 (有些线只能充电)
* 设备上如果开着 U 盘模式, 先关掉 — 那个模式不能烧录
* 设备已经卡死时: 拔掉电源等 3 秒再插上, 或用 --race 抢窗
* 命令行加 --list-ports 看电脑到底认出了什么


杀毒软件报毒?
-------------
本程序由 PyInstaller 打包, 未加壳 (已刻意关掉 UPX)。如果被拦, 请加白名单后再运行。
命令行模式下加 --selftest 可以确认包本身是完好的。


重要说明
--------
* 本包不含任何 WiFi 凭据或设备密钥。
* 烧录**不会**清除设备上的 WiFi 配置和日记 — 只写 bootloader / 分区表 /
  otadata / 主程序 (和资源包, 如果带了)。
* 「提取分区」和擦除前的备份都是**只读**的, 不改设备。但读 nvs 出来的文件含
  WiFi 密码与设备 Token — 界面上要手打确认词才给读, 读出来也别外传。
* 擦除 (维护区那几个按钮) 是**不可逆**的: 擦掉的日记找不回来。
  擦之前建议先读一次备份 — 界面上有「擦除前先备份」的选项。
""" % (ver, "含 (26MB)" if with_assets else "不含",
       manifest["built_at"], manifest["git_commit"] or "(未知)",
       APP_NAME,
       "含" if with_assets else "不含",
       "\n".join("  %-42s %s" % (k, v["md5"]) for k, v in sorted(z.items())),
       APP_NAME, APP_NAME, APP_NAME, APP_NAME, APP_NAME, APP_NAME, APP_NAME)


def make_zip(stage: Path, ver: str, with_assets: bool) -> Path:
    tag = "%s_%s%s" % (APP_NAME, ver, "_assets" if with_assets else "")
    out = DIST / ("%s.zip" % tag)
    if out.exists():
        out.unlink()
    fixed = (2026, 1, 1, 0, 0, 0)          # 固定时间戳, 同一个输入出同一个包
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for p in sorted(stage.rglob("*")):
            if not p.is_file():
                continue
            zi = zipfile.ZipInfo("%s/%s" % (tag, p.relative_to(stage).as_posix()), fixed)
            zi.compress_type = zipfile.ZIP_DEFLATED
            zi.external_attr = 0o644 << 16
            z.writestr(zi, p.read_bytes())
    digest = sha256(out)
    (out.with_suffix(".zip.sha256")).write_text(
        "%s  %s\n" % (digest, out.name), "utf-8")
    print("PASS 出包          %s  (%.1f MB)" % (out, out.stat().st_size / 1048576.0))
    print("     sha256        %s" % digest)
    return out


def verify_exe(stage: Path):
    """在分发包上跑自检 — 冻结后最容易崩的是 stub 数据与资源定位, 只有这个能暴露。"""
    target = stage / ("%s.exe" % APP_NAME)
    if not target.is_file():
        print("SKIP 冻结包自检    没有 exe (用了 --skip-build)")
        return
    for args in (["--selftest"], ["--list-ports"], ["--run-esptool", "version"]):
        r = subprocess.run([str(target)] + args, capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=180)
        head = (r.stdout or "").strip().splitlines()
        tail = head[-1] if head else ""
        ok = r.returncode == 0
        print("%-5s 冻结包 %-22s %s" % ("PASS" if ok else "FAIL",
                                        " ".join(args), tail[:70] if ok else ""))
        if not ok:
            print((r.stdout or "")[-1500:])
            print((r.stderr or "")[-800:])
            raise Gate("冻结包自检失败: %s" % " ".join(args))


# ══════════════════════════════════════════════════════════════════════
def main() -> int:
    ap = argparse.ArgumentParser(description="产出可外发的 Virtualpet 烧录器分发包")
    ap.add_argument("--with-assets", action="store_true", help="把 assets.bin (26MB) 也打进去")
    ap.add_argument("--skip-build", action="store_true", help="不跑 PyInstaller, 只验拷贝/闸门/zip")
    ap.add_argument("--keep-stage", action="store_true", help="保留 dist/flasher/pkg 中间目录")
    args = ap.parse_args()

    try:
        ver = app_version()
        print("镜像版本: %s" % ver)
        gate_version(ver)
        picked = gate_sources(args.with_assets)
        if not args.skip_build:
            exe = build_exe(clean=True)
        else:
            exe = DIST / APP_NAME / ("%s.exe" % APP_NAME)
            if not exe.is_file():
                print("!! --skip-build 但 %s 还不存在, 目录内容将只有 images/" % exe)
        tag = "%s_%s%s" % (APP_NAME, ver, "_assets" if args.with_assets else "")
        stage_dir = STAGE_ROOT / tag
        manifest = stage(stage_dir, exe, picked, ver, args.with_assets, args.skip_build)
        gate_sensitive(stage_dir)
        if not args.skip_build:
            verify_exe(stage_dir)
        out = make_zip(stage_dir, ver, args.with_assets)

        print("\n包内容:")
        for p in sorted(stage_dir.rglob("*")):
            if p.is_file():
                print("  %9.1f KB  %s" % (p.stat().st_size / 1024.0,
                                          p.relative_to(stage_dir)))
        if not args.keep_stage:
            shutil.rmtree(stage_dir, ignore_errors=True)
        print("\n>>> 分发包: %s" % out)
        return 0
    except Gate as e:
        print("\n!! %s" % e, file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        return 2
    finally:
        if args.keep_stage:
            print("(中间目录保留在 %s)" % STAGE_ROOT)


if __name__ == "__main__":
    sys.exit(main())
