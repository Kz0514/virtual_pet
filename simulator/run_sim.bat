@echo off
rem LVGL PC simulator launcher (double-click to run)
rem SDL2.dll lives in MSYS2 ucrt64 toolchain dir; must be on PATH.
rem Run dir is pinned to simulator/ (path translation uses relative paths).
set PATH=C:\msys64\ucrt64\bin;%PATH%
cd /d %~dp0
start "Virtualpet Simulator" build\lvgl_simulator.exe
