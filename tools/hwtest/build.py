#!/usr/bin/env python3
"""hwtest 构建脚本 — 规避 Git Bash/MSYS 坑 (MSYSTEM 注入导致 idf.py 被静默跳过)。

用法: python tools/hwtest/build.py [build|app-flash|erase-flash|size] [-p COM9] [透传参数...]
      默认 build

环境基于 VSCode ESP-IDF 扩展布局 (IDF v5.5.4, tools 在 C:\\Espressif),
手法照搬主工程的 CI 构建脚本 (全部在原生 python 进程内设好, 不依赖 shell)。
可用环境变量覆盖 IDF_PATH / IDF_TOOLS_PATH / IDF_PYTHON_ENV_PATH。
"""
import os
import shutil
import subprocess
import sys

# 控制台/管道按 UTF-8 输出: 否则中文注释混进编译器错误时, idf.py 在 GBK 控制台上
# 直接 UnicodeEncodeError 崩掉 (看不到真正的错误)
for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = HERE
IDF_PATH = os.environ.get("IDF_PATH", r"H:\esp-idf\v5.5.4\esp-idf")
IDF_TOOLS_PATH = os.environ.get("IDF_TOOLS_PATH", r"C:\Espressif")
IDF_PY_ENV = os.environ.get(
    "IDF_PYTHON_ENV_PATH",
    r"C:\Users\c1364\.espressif\python_env\idf5.5_py3.11_env",
)
PY = os.path.join(IDF_PY_ENV, "Scripts", "python.exe")
TOOL_DIRS = [
    r"C:\Espressif\tools\cmake\3.30.2\bin",
    r"C:\Espressif\tools\ninja\1.12.1",
    r"C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin",
    r"C:\Espressif\tools\ccache",
    r"C:\Espressif\tools\esp-clang\esp-19.1.2_20250312\esp-clang\bin",
]


def ensure_secrets():
    """首次构建时从模板生成 main/hw_secrets.h (gitignored, 含现场 WiFi 凭据)"""
    dst = os.path.join(PROJ, "main", "hw_secrets.h")
    if not os.path.exists(dst):
        shutil.copyfile(os.path.join(PROJ, "main", "hw_secrets.h.in"), dst)
        print(">>> 生成默认 main/hw_secrets.h (无凭据: 只扫描不连接)")


def idf_env():
    env = os.environ.copy()
    env["IDF_PATH"] = IDF_PATH
    env["IDF_TOOLS_PATH"] = IDF_TOOLS_PATH
    env["IDF_PYTHON_ENV_PATH"] = IDF_PY_ENV
    for k in ("MSYSTEM", "MSYS", "MINGW_CHOST"):
        env.pop(k, None)
    env["PYTHONIOENCODING"] = "utf-8"  # idf.py/ninja 输出里的中文不再炸 GBK
    env["PATH"] = os.pathsep.join(
        TOOL_DIRS + [p for p in env.get("PATH", "").split(os.pathsep) if "/usr/" not in p and p]
    )
    return env


def main():
    action = sys.argv[1] if len(sys.argv) > 1 else "build"
    extra = sys.argv[2:]
    ensure_secrets()
    cmd = [PY, os.path.join(IDF_PATH, "tools", "idf.py"), action] + extra
    print(">>>", " ".join(cmd))
    sys.exit(subprocess.call(cmd, env=idf_env(), cwd=PROJ))


if __name__ == "__main__":
    main()
