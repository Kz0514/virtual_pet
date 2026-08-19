# Virtualpet

Virtualpet是一款基于VPET(虚拟主播模拟器)中萝莉丝形象的”电子宠物”。与传统电子宠物不同的是，Virtualpet更侧重于宠物与主人更多样、更主动的互动，通过LLM模型主动调用各类API(天气、景点、商铺查询等)及主动控制语音tts语音语调和屏幕动画，使宠物拥有更加丰富多样的交互和情景感知能力，并可通过归纳总结记忆主人的日常喜好、对话偏好等。

Virtualpet拥有丰富的外设与传感器，可以通过环境噪音、亮度检测、加速度计、历史聊天、使用时间等多种方式熟悉主人的生活习惯，从而降低在非常用时段的主动对话频次。在平时，Virtualpet会尝试根据情景主动进行对话，例如天气、休息提醒，早晚问候，地点、美食推荐等。
通过亮度检测和环境噪音检测、Virtualpet可以自行调节声音大小和亮度。

Virtualpet拥有包括语音对话在内的多种互动方式，比如摇晃检测、触摸检测(非屏幕)等。

Virtualpet支持OTA功能，固件升级便捷。将来会持续更新可用动画及完善优化更多功能

Virtualpet预计包含日记功能，当单日的互动次数较多时，就可以收到一篇来自宠物的日记，有小概率包含可爱的涂鸦或简笔画。

本项目基于esp32-s3开发，相关硬件及外壳设计已在立创开源平台开源。[硬件开源链接](https://oshwhub.com/c1364/project_ndjrtbhi?jlc_vid=QVUIBVZTTwcKBgZeFldZBFFSQwJbVQcFRgddAgFVTwAxVlNeQVRcUlJVQ1hZVzsOAxUeFF5JWBEPFBcWGBMaSQ4KFE8NCAlJ)

在使用项目中的BIN烧录时，目前您可以免费使用我的服务器作为服务端。

与VPET类似，Virtualpet所有功能也可以作为一个电子宠物框架使用，只需您自行更换动画与LLM控制指令即可。(由于需要服务端配置，所以需要自行部署服务端)
如需修改、分发本项目，请仔细阅读并遵守[VPET](https://github.com/LorisYounger/VPet)的开源使用协议或去除任何与VPET动画资源及形象相关的内容。

当前已经集成的API:

LLM,ASR,TTS,IP定位，天气查询，地点、周边查询，地图(暂未使用)，地址解析和逆地址解析(用于测算两点距离)，智能硬件定位(即根据WIFI/蓝牙信标的精准定位方式，项目里有相关代码，但我的KEY无权限，未实测)

由于开发时间较短，框架尚不完善，将会持续更新，预计1.1.0版本完成整个框架

开源项目使用:
[VPET](https://github.com/LorisYounger/VPet)
[ESP-IDF](https://github.com/espressif/esp-idf)
[LVGL](https://github.com/lvgl/lvgl)
[FastAPI](https://github.com/fastapi/fastapi)

## 项目结构(V1.0.0)

```
Virtualpet/
├── main/                  # ESP32 固件源码
│   ├── ai/                # AI 模块（TTS、唤醒词、Prompt）
│   ├── app/               # 应用层（宠物引擎、动画管理、传感器融合）
│   ├── drivers/           # 硬件驱动（屏幕、触摸、音频、传感器）
│   ├── network/           # 网络（WiFi、WebSocket、OTA）
│   ├── system/            # 系统（电源、时间、GPIO）
│   └── ui/                # UI（屏幕、控件、字体）
├── server/                # 服务器端（FastAPI 后端）
│   ├── app/api/           # REST + WebSocket API
│   ├── app/services/      # 业务逻辑（LLM、TTS、天气、日记）
│   ├── app/core/          # 基础设施（数据库、Redis、安全）
│   └── app/models/        # 数据模型
├── assets/                # 动画资源
├── tools/                 # 工具脚本
└── spiffs/                # SPIFFS 文件系统数据
```


## 快速开始

### ESP32 固件

```bash
# 安装 ESP-IDF v5.5+
# 配置 Wi-Fi 和服务器地址：main/network/server_config.h

idf.py build
idf.py flash
```

### 服务器

```bash
cd server
cp .env.example .env       # 编辑 .env 填入数据库密码、JWT密钥、TTS音色ID
cp api.example.json api.json  # 填入 API Key
docker compose up -d
```

## 许可证

[Apache License 2.0](LICENSE)
