# LVGL PC 模拟器

复用 `main/ui/` 代码, 通过 SDL2 在桌面显示 Virtualpet 的 LVGL 画面。

**不影响原项目** — UI 代码不移动不修改, 模拟器通过路径引用编译。

## 资源加载

模拟器**直接读取项目根目录的 `spiffs/`** (实机 SPIFFS 镜像源目录) —
动画帧和 `zh.bin` 字体与实机 100% 一致, 实机更新资源后模拟器自动同步, 无需转换。

> 若需从 `assets/animation/*/frame_XX.c` 重新生成 .bin, 可用 `convert_frames.py`。

## 环境准备 (Windows MSYS2)

```bash
pacman -S --noconfirm mingw-w64-ucrt-x86_64-SDL2 mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-gcc make
```

## 构建

```bash
cd simulator
mkdir -p build && cd build
cmake .. -G "Unix Makefiles"
make -j$(nproc)
```

## 运行

```bash
cd simulator
./build/lvgl_simulator.exe
```

> 必须在 `simulator/` 目录下运行 (`__wrap_fopen` 用相对路径 `../spiffs/`)。

## 键盘快捷键 (需窗口在前台)

| 按键 | 效果 |
|------|------|
| 1~7 | 切换动画 (idle/happy/sad/excited/sleepy/eating/surprised) |
| C | 显示聊天气泡 |
| N | 显示通知 |
| H | 切换 WiFi 状态图标 |
| Q / ESC | 退出 |

## 文件结构

```
simulator/
├── CMakeLists.txt          # CMake 构建文件
├── main.c                  # 模拟器入口 (SDL2 + LVGL)
├── lv_conf.h               # LVGL PC 配置 (SDL2 驱动)
├── sim_spiffs.c            # fopen 包装 → /spiffs 路径重定向
├── convert_frames.py       # 帧转换脚本 (.c → .bin)
├── spiffs/                 # 生成的 .bin 动画帧
├── mock/
│   ├── esp_log.h           # ESP_LOGI → printf
│   ├── esp_err.h           # ESP_OK / ESP_FAIL
│   ├── esp_spiffs.h        # SPIFFS VFS (空)
│   ├── esp_heap_caps.h     # heap_caps_malloc → malloc
│   ├── esp_task_wdt.h      # TWDT → no-op
│   ├── st7789.h            # 背光控制 → no-op
│   ├── touch_fpc.h         # 触摸 → no-op
│   └── freertos/
│       ├── FreeRTOS.h      # 基础类型
│       └── task.h          # vTaskDelay 等
└── build/                  # 构建输出 (gitignore)
```
