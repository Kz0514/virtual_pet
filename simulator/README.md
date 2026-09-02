# LVGL PC 模拟器

复用 `main/ui/` 代码, 通过 SDL2 在桌面显示 Virtualpet 的 LVGL 画面。

**不影响原项目** — UI 代码不移动不修改, 模拟器通过路径引用编译。

## 资源加载

模拟器资源与实机**分离** (动画包在模拟器目录, 字体共用回落):

- `/spiffs/xxx` → **优先 `simulator/spiffs/xxx`** (模拟器专属资源, 如动画包
  `anims.bin`), **缺失回落 `../assets_fs/xxx`** (实机资源, 如字体 `zh.bin`)
- `/data/xxx`   → `../simdata/xxx` (日记等, 首次访问自动建 `simdata/diary/`)

> 动画已是单文件包架构 `anims.bin` (固件 SPIFFS 文件数 90→2, open 毫秒级;
  帧读 = fd 常开 + lseek/read)。由 [tools/gen_anim_bin.py](../tools/gen_anim_bin.py)
  从 `assets/anim_bin/` 生成, 复制到 `simulator/spiffs/` 即模拟器可用。
> 旧散帧转换 `convert_frames.py` 仅历史参考 (新固件不再读散帧)。
> `SIM_ZH_FONT` 环境变量可覆写 zh.bin 路径 (字体测试用)。

## 环境准备 (Windows MSYS2)

```bash
pacman -S --noconfirm mingw-w64-ucrt-x86_64-SDL2 mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-gcc
```

(MSYS2 ucrt64 无 `make.exe`, 用 `mingw32-make`)

## 构建

```bash
export PATH="/c/msys64/ucrt64/bin:$PATH"   # SDL2.dll / cmake / gcc
cd simulator
cmake -S . -B build -G "MinGW Makefiles"   # SDL2 找不到时加 -DCMAKE_PREFIX_PATH=C:/msys64/ucrt64
cmake --build build -j8
```

## 运行

```bash
cd simulator
./build/lvgl_simulator.exe
```

> 必须在 `simulator/` 目录下运行 (路径翻译用相对路径 `../assets_fs/` `../simdata/`)。
> 退出方式: Q, 或主页时 ESC (设置/日记页内 ESC 是"返回", 不会退出)。

## 键盘快捷键 (需窗口在前台)

| 按键 | 效果 |
|------|------|
| 1~7 | 切换动画 (idle/happy/sad/excited/sleepy/eating/surprised) |
| C | 显示聊天气泡 |
| N | 显示通知 |
| H | 切换 WiFi 状态图标 |
| **S** | **打开/关闭设置页** |
| **D** | **打开日记页 (需 `simdata/diary/` 下有 `YYYY-MM-DD.html`)** |
| **↑ / ↓ / 回车 / ESC** | **设置/日记页导航, 语义与真机 input_handler 一致** (ESC 逐级返回, 根页回主页; 仅主页时退出) |
| Q / 主页 ESC | 退出 |

## 依赖体系

- **LVGL**: `components/lvgl` (9.5.0, 与固件 vendored 版本同源; 本地组件已取代 managed_components)
- **libwebp**: `components/libwebp` 直编 (日记涂鸦解码)。MinGW x86_64 默认定义 `__SSE2__`,
  cpu.c 运行时派发引用 SSE2 init, 必须补编 7 个 `*_sse2.c` (源清单见 CMakeLists.txt WEBP_SRCS)
- **路径翻译**: `--wrap=fopen/open/stat64i32/opendir` (MinGW 坑: `stat()` 编译期重定向到
  `stat64i32`, 只能 wrap 后者)。`/spiffs/` 优先模拟器目录缺失回落实机目录。
  **`open` 强制加 `O_BINARY`** — 固件代码无文本模式概念,
  Windows 默认文本模式会把 `0x1A` 当 EOF 截断二进制资源
- **mock 头**: `mock/` 目录, static-inline 风格, include 顺序 mock 优先于 main/ (遮蔽真 board.h)
- **链接桩**: `mock_impl.c` — TTS/马达/手势/usb_storage/config(NVS 模拟)/OTA 等真实符号

## 文件结构

```
simulator/
├── CMakeLists.txt          # CMake 构建文件 (LVGL_ROOT/webp 源/wrap/mock include)
├── main.c                  # 模拟器入口 (SDL2 + LVGL + 键盘快捷键)
├── lv_conf.h               # LVGL PC 配置 (SDL2 驱动)
├── sim_spiffs.c            # 路径翻译层 (--wrap 实现, 见上)
├── convert_frames.py       # 帧转换脚本 (.c → .bin, 仅历史参考)
├── spiffs/                 # 模拟器专属资源 (anims.bin 动画包, 与实机分离)
├── mock/                   # 固件头 mock (static inline)
│   ├── esp_log.h           # ESP_LOGI → printf
│   ├── esp_err.h           # ESP_OK/ESP_FAIL + esp_err_to_name
│   ├── esp_lvgl_port.h     # lvgl_port_lock → true
│   ├── esp_timer.h         # esp_timer_get_time → clock_gettime
│   ├── esp_app_desc.h      # esp_app_get_description (实现在 mock_impl.c)
│   ├── esp_spiffs.h        # SPIFFS VFS (空)
│   ├── esp_heap_caps.h     # heap_caps_malloc → malloc
│   ├── esp_task_wdt.h      # TWDT → no-op
│   ├── esp_system.h        # esp_restart 声明
│   ├── board.h             # 仅 BATTERY_CRITICAL_THRESHOLD_PCT (遮蔽真 board.h)
│   ├── ff.h                # 空头 (settings_screen 仅 include)
│   ├── st7789.h            # 背光控制 → no-op
│   ├── touch_fpc.h         # 触摸 → no-op
│   ├── tm6604.h            # 马达 → no-op (+vibrate_raw/is_vibrating)
│   ├── tap_detector.h      # 手势 → no-op
│   ├── shake_detector.h    # 手势 → no-op
│   ├── tts_client.h        # TTS → no-op
│   └── freertos/
│       ├── FreeRTOS.h      # 基础类型
│       └── task.h          # vTaskDelay / xTaskCreatePinnedToCore 等
├── mock_impl.c             # 链接桩: TTS/马达/手势/config(键值表)/usb/OTA/esp_restart
└── build/                  # 构建输出 (gitignore)
```
