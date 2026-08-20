@echo off
rem Build helper: Git Bash can't run idf.py (MSys unsupported), cmd/PowerShell quoting gets mangled by MSYS path conversion.
rem Env replicated 1:1 from C:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1 (EIM install):
rem   IDF_PATH/IDF_TOOLS_PATH/IDF_PYTHON_ENV_PATH + hardcoded tool PATH. No export.bat —
rem   EIM's tool versions are NEWER than IDF 5.5.4 tools.json, so idf_tools detection
rem   reports "no installed versions" and export fails; PATH alone is sufficient.
set MSYSTEM=
set IDF_PATH=H:\esp-idf\v5.5.4\esp-idf
set IDF_TOOLS_PATH=C:\Espressif\tools
set IDF_PYTHON_ENV_PATH=C:\Espressif\tools\python\v5.5.4\venv
set IDF_CCACHE_ENABLE=1
set ESP_ROM_ELF_DIR=C:\Espressif\tools\esp-rom-elfs\20241011/
set OPENOCD_SCRIPTS=C:\Espressif\tools\openocd-esp32\v0.12.0-esp32-20251215\openocd-esp32\share\openocd\scripts
set PATH=C:\Espressif\tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64;C:\Espressif\tools\cmake\3.30.2\bin;C:\Espressif\tools\dfu-util\0.11\dfu-util-0.11-win64;C:\Espressif\tools\esp-clang\esp-19.1.2_20250312\esp-clang\bin;C:\Espressif\tools\esp-rom-elfs\20241011\;C:\Espressif\tools\esp32ulp-elf\2.38_20240113\esp32ulp-elf\bin;C:\Espressif\tools\esp32ulp-elf\2.38_20240113\esp32ulp-elf\esp32ulp-elf\bin;C:\Espressif\tools\idf-exe\1.0.3\;C:\Espressif\tools\ninja\1.12.1\;C:\Espressif\tools\openocd-esp32\v0.12.0-esp32-20251215\openocd-esp32\bin;C:\Espressif\tools\riscv32-esp-elf-gdb\16.3_20250913\riscv32-esp-elf-gdb\bin;C:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin;C:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\riscv32-esp-elf\bin;C:\Espressif\tools\xtensa-esp-elf-gdb\16.3_20250913\xtensa-esp-elf-gdb\bin;C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin;C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\xtensa-esp-elf\bin;C:\Espressif\tools\python\v5.5.4\venv\Scripts
cd /d c:\Users\c1364\Documents\esp-idf\Virtualpet
rem Drive icon: put tools\pet.ico to embed it into the firmware (missing = empty = system default icon)
C:\Espressif\tools\python\v5.5.4\venv\Scripts\python.exe tools\gen_icon.py
if errorlevel 1 exit /b %errorlevel%
C:\Espressif\tools\python\v5.5.4\venv\Scripts\python.exe H:\esp-idf\v5.5.4\esp-idf\tools\idf.py build
exit /b %errorlevel%
