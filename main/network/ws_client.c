/**
 * @file ws_client.c
 * @brief WebSocket 客户端 — 服务器实时通信 (仅文本, 不做音频流)
 */
#include "ws_client.h"
#include "server_config.h"
#include "chat_bubble.h"
#include "tts_client.h"
#include "pet_engine.h"
#include "pet_avatar.h"
#include "wifi_scanner.h"
#include "memory_store.h"
#include "life_log.h"
#include "config_mgr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_websocket_client.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "ws";
static esp_websocket_client_handle_t s_client = NULL;
static bool s_connected = false;
static bool s_paused = false;            /* 息屏期暂停 (WiFi 已停, 重连必败) */
static volatile uint32_t s_chat_seq = 0; /* chat_done 计数 — 会话模式等待回复信号 */
static char s_last_user_text[512] = {0}; /* 服务端回显的用户原文 — 设备端记忆用 */
/* P1 流式回复 (chat_text 帧) 的气泡累计 — 逐句追加打字机, chat_done 重置 */
static char s_stream_text[512] = {0};
/* 聚合协议 (1.0.257→1.0.259): 恒为聚合模式 — 服务器 LLM 流式期间增量
 * 合成, WS 二进制帧直推 (audio_start/binary/audio_end)。单设备恒最新
 * 固件 — 无逐句合成 (旧协议) 兼容路径 (1.0.259 已删)。
 * s_ws_audio_seen = 本轮收到过 audio_start (音频由 WS 流推送,
 * chat_done 时不再 POST; 未收到 = 服务器 TTS 失败 → 兜底整段 POST) */
static bool s_ws_audio_seen = false;

/* : UTF-8 清洗 — 坏字节就地替换为 '?' (长度不变, 返回坏字节数,
 * 输出仍合法 UTF-8)。坏点源于 512B snprintf 行尾截断。
 * 演进: 截断式 (早期版) 坏点后内容全丢 — 连后续完整记录一起删;
 * 剔除式 坏字节跳过 — 相邻字节重新对齐可能错配成假字符;
 * '?' 替换 坏点独立占位, 其余字节解析不受影响, LLM/日志可感知缺失。
 * 背景: 半个字符 append 进 memory.txt → mem_summary 随 chat 帧上送 →
 * 服务端 uvicorn decode 失败直接关连接 (每次语音必被踢)。 */
static size_t utf8_sanitize_inplace(char *s, size_t len)
{
    size_t i = 0, bad = 0;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        size_t need;
        if (c < 0x80) {
            i++;
            continue;
        }
        if (c >= 0xC2 && c <= 0xDF)
            need = 2; /* 2 字节 */
        else if (c >= 0xE0 && c <= 0xEF)
            need = 3; /* 3 字节 */
        else if (c >= 0xF0 && c <= 0xF4)
            need = 4; /* 4 字节 */
        else {
            s[i] = '?';
            bad++;
            i++;
            continue;
        }                               /* 孤立 continuation/非法起始 */
        bool is_bad = (i + need > len); /* 字符被截断 */
        for (size_t k = 1; !is_bad && k < need; k++) {
            if (((unsigned char)s[i + k] & 0xC0) != 0x80) is_bad = true; /* continuation 非法 */
        }
        if (is_bad) {
            s[i] = '?';
            bad++;
            i++;
            continue;
        } /* 坏起始字节 — 占位, 后续重新解析 */
        i += need;
    }
    return bad;
}

/* chat_done/chat_text 共用的回复清洗 (原两条内联循环收敛为单函数):
 * clean  剥 |pXXX / [inst:…] / [tag] / /tools.xxx — 显示/记忆/日志用
 * tts    剥 |pXXX / [inst:…] / /tools.xxx, 保留 [tag] — 语音用 (原行为)
 * 均 UTF-8 清洗 (坏字节替换 '?'), 输出显式终止符 */
static void parse_reply_text(const char *src, char *clean, size_t cs,
                             char *tts_text, size_t ts)
{
    int ci = 0, ti = 0;
    const char *s = src;
    while (*s && ci < (int)cs - 1) {
        if (s[0] == '|' && s[1] == 'p' && s[2] >= '0' && s[2] <= '9') {
            s += 2;
            while (*s >= '0' && *s <= '9')
                s++;
            continue;
        }
        if (strncmp(s, "[inst:", 6) == 0) {
            const char *end = strchr(s + 6, ']');
            if (end) {
                s = end + 1;
                continue;
            }
        }
        if (*s == '[') {
            const char *end = strchr(s, ']');
            if (end) {
                s = end + 1;
                continue;
            }
        }
        if (strncmp(s, "/tools.", 7) == 0) {
            s += 7;
            while (*s && (*s != ' ' && *s != '(' && *s != ','))
                s++;
            if (*s == '(') {
                const char *e = strchr(s, ')');
                if (e) s = e + 1;
            }
            while (*s == ' ')
                s++;
            continue;
        }
        clean[ci++] = *s++;
    }
    clean[ci] = '\0'; /* 必须显式终止 — 缺此 strlen 读到栈垃圾 */
    utf8_sanitize_inplace(clean, (size_t)ci);
    if (!tts_text || ts < 1)
        return; /* 仅需显示文本 (聚合协议 chat_text) — 跳过 tts 清洗 */
    s = src;
    while (*s && ti < (int)ts - 1) {
        if (s[0] == '|' && s[1] == 'p' && s[2] >= '0' && s[2] <= '9') {
            s += 2;
            while (*s >= '0' && *s <= '9')
                s++;
            continue;
        }
        if (strncmp(s, "[inst:", 6) == 0) {
            const char *end = strchr(s + 6, ']');
            if (end) {
                s = end + 1;
                continue;
            }
        }
        if (strncmp(s, "/tools.", 7) == 0) {
            s += 7;
            while (*s && (*s != ' ' && *s != '(' && *s != ','))
                s++;
            if (*s == '(') {
                const char *e = strchr(s, ')');
                if (e) s = e + 1;
            }
            while (*s == ' ')
                s++;
            continue;
        }
        tts_text[ti++] = *s++;
    }
    tts_text[ti] = '\0'; /* 缺此 → TTS 文本尾部栈垃圾 (实测乱码) */
    utf8_sanitize_inplace(tts_text, (size_t)ti);
}

/* 分片重组缓冲 (PSRAM) — 大消息 (memory_update 等) 按片到达 */
static char *s_rx_buf = NULL;
static int s_rx_len = 0, s_rx_total = 0;

uint32_t ws_client_get_chat_seq(void) { return s_chat_seq; }

/* 动画名 → pet_anim_t (PET_ANIM_COUNT = 未识别) */
static pet_anim_t parse_anim_name(const char *a)
{
    if (strcmp(a, "idle") == 0)
        return PET_ANIM_IDLE;
    else if (strcmp(a, "happy") == 0)
        return PET_ANIM_HAPPY;
    else if (strcmp(a, "sad") == 0)
        return PET_ANIM_SAD;
    else if (strcmp(a, "excited") == 0)
        return PET_ANIM_EXCITED;
    else if (strcmp(a, "surprised") == 0)
        return PET_ANIM_SURPRISED;
    else if (strcmp(a, "sleepy") == 0)
        return PET_ANIM_SLEEPY;
    else if (strcmp(a, "eating") == 0)
        return PET_ANIM_EATING;
    else if (strcmp(a, "blush") == 0)
        return PET_ANIM_BLUSH;
    else if (strcmp(a, "pathead") == 0)
        return PET_ANIM_PATHEAD;
    else if (strcmp(a, "scratch") == 0)
        return PET_ANIM_SCRATCH;
    else if (strcmp(a, "pointself") == 0)
        return PET_ANIM_POINTSELF;
    return PET_ANIM_COUNT;
}

/* get_memory 读盘结果回调 — 写盘任务上下文执行 (仅 socket 发送, 零 flash 访问)。
 * WS 任务栈在 PSRAM, flash 读期间同样禁用 cache — 任何 flash 访问都会在
 * PSRAM 栈上 double exception (1.0.213 修了写, 1.0.214 补上读) */
static void ws_memory_read_cb(const char *content, size_t len, void *arg)
{
    char *rid = (char *)arg;
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "type", "memory_data");
    if (rid && rid[0]) cJSON_AddStringToObject(resp, "req_id", rid);
    cJSON_AddNumberToObject(resp, "size", (double)len);
    cJSON_AddStringToObject(resp, "content", content ? content : "");
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (json) {
        ESP_LOGI(TAG, "memory_data: %u B (req=%s)", (unsigned)len, rid ? rid : "?");
        ws_client_send_json(json);
        cJSON_free(json);
    }
    free(rid); /* 调用方 strdup 的 req_id, 回调上下文释放 */
}

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_websocket_event_data_t *evt = data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WS 已连接! (ws 任务栈水位 %u B)",
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
        s_connected = true;
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WS 断开");
        s_connected = false;
        /* WS 音频流模式: 断连 = 不会有更多音频也不会有 audio_end —
         * 立即置 rb_done 让播放排空收尾, 免 10s 死等 (1.0.261:
         * 1.0.260 实测断连后排空死等 10s 才强制换句) */
        if (tts_client_is_ws_active())
            tts_client_ws_end();
        if (s_rx_buf) {
            free(s_rx_buf);
            s_rx_buf = NULL;
        } /* 丢弃半截消息 */
        s_rx_len = s_rx_total = 0;
        break;
    case WEBSOCKET_EVENT_DATA: {
        /* ── 二进制帧 = WS 音频流 PCM (1.0.258 聚合二期) ──
         * 逐片直接入环, 绝不分片重组: PCM 是字节流, 环按序消费, 分片
         * 边界无意义。重组需按消息 malloc 大缓冲 — 播放期 PSRAM 被
         * 双槽环+动画帧占满, 每条消息每片都分配 payload_len+1 必失败
         * (1.0.259 实测: 全刷"重组缓冲分配失败", PCM 全丢 = 没听到播完)。
         * 零分配零拷贝, 组件任务最轻。 */
        if (evt->op_code == 2 && evt->data_len > 0) {
            tts_client_ws_feed((const uint8_t *)evt->data_ptr,
                               (uint32_t)evt->data_len);
            break;
        }
        /* 分片消息重组: payload_len > data_len 表示本条只是其中一片
         * (仅文本帧走此路径 — 二进制帧已在上方直接喂环) */
        const char *msg;
        int msg_len;
        if (evt->payload_len > evt->data_len) {
            if (!s_rx_buf) {
                s_rx_buf = heap_caps_malloc(evt->payload_len + 1, MALLOC_CAP_SPIRAM);
                if (!s_rx_buf) {
                    ESP_LOGE(TAG, "重组缓冲分配失败");
                    break;
                }
                s_rx_len = 0;
                s_rx_total = evt->payload_len;
            }
            memcpy(s_rx_buf + evt->payload_offset, evt->data_ptr, evt->data_len);
            s_rx_len += evt->data_len;
            if (s_rx_len < s_rx_total) break; /* 等剩余分片 */
            s_rx_buf[s_rx_len] = '\0';
            msg = s_rx_buf;
            msg_len = s_rx_len;
            s_rx_buf = NULL;
            s_rx_len = s_rx_total = 0;
        } else {
            msg = (const char *)evt->data_ptr;
            msg_len = evt->data_len;
        }
        /* Only log JSON messages (ignore binary/control frames) */
        if (msg_len > 0 && msg[0] == '{')
            ESP_LOGI(TAG, "<<< %.*s", msg_len, msg);
        if (strstr(msg, "\"error\"") ||
            strstr(msg, "未授权")) {
            ESP_LOGE(TAG, "WS error!");
            s_connected = false;
            break;
        }
        { /* parse and handle */
            cJSON *root = cJSON_ParseWithLength(msg, msg_len);
            if (root) {
                cJSON *type = cJSON_GetObjectItem(root, "type");
                cJSON *txt = cJSON_GetObjectItem(root, "text");

                /* ── scan_wifi: server requests WiFi scan for network location ── */
                if (cJSON_IsString(type) && strcmp(type->valuestring, "scan_wifi") == 0) {
                    ESP_LOGI(TAG, "scan_wifi: starting scan");
                    wifi_ap_info_t aps[WIFI_SCAN_MAX_APS];
                    int count = wifi_scan_aps(aps);
                    ESP_LOGI(TAG, "scan_wifi: found %d APs", count);
                    static char wifi_json[800];
                    static char resp[1024];
                    int wj_len = wifi_scan_build_json(aps, count, wifi_json, sizeof(wifi_json));
                    int n = snprintf(resp, sizeof(resp),
                                     "{\"type\":\"scan_result\",\"wifiinfo\":%.*s}",
                                     wj_len > 0 ? wj_len : 2,
                                     wj_len > 0 ? wifi_json : "[]");
                    ESP_LOGI(TAG, "scan_wifi: sending result (%d bytes, %d APs)", n, count);
                    ws_client_send_json(resp);
                }
                /* ── get_memory: 服务端工具 /tools.history 拉取完整对话记忆 ── */
                else if (cJSON_IsString(type) && strcmp(type->valuestring, "get_memory") == 0) {
                    /* 读盘委托写盘任务 (回调在写盘任务上下文) — WS 任务栈在
                     * PSRAM, memory_store_get 的 stat/fopen 同样是 cache 禁用期
                     * flash 读, 会 double exception (1.0.214 根因) */
                    cJSON *req_id = cJSON_GetObjectItem(root, "req_id");
                    char *rid = NULL;
                    if (cJSON_IsString(req_id) && req_id->valuestring[0])
                        rid = strdup(req_id->valuestring); /* 回调上下文释放 */
                    if (memory_store_read_async(ws_memory_read_cb, rid) != ESP_OK) {
                        free(rid);
                        /* 入队失败仍要应答, 否则服务端工具调用挂起 */
                        cJSON *resp = cJSON_CreateObject();
                        cJSON_AddStringToObject(resp, "type", "memory_data");
                        if (cJSON_IsString(req_id))
                            cJSON_AddStringToObject(resp, "req_id", req_id->valuestring);
                        cJSON_AddNumberToObject(resp, "size", 0);
                        cJSON_AddStringToObject(resp, "content", "");
                        char *json = cJSON_PrintUnformatted(resp);
                        cJSON_Delete(resp);
                        if (json) {
                            ws_client_send_json(json);
                            cJSON_free(json);
                        }
                    }
                }
                /* ── memory_update: 服务端压缩后下发覆盖 ── */
                else if (cJSON_IsString(type) && strcmp(type->valuestring, "memory_update") == 0) {
                    cJSON *content = cJSON_GetObjectItem(root, "content");
                    if (cJSON_IsString(content))
                        memory_store_overwrite(content->valuestring);
                }
                /* ── audio_start / audio_end: WS 音频流边界 (1.0.258 聚合
                    二期) — LLM 流式期间增量合成的 PCM 经二进制帧直推, 本
                    帧只做起链/收尾 ── */
                else if (cJSON_IsString(type) &&
                         strcmp(type->valuestring, "audio_start") == 0) {
                    if (tts_client_ws_start()) {
                        s_ws_audio_seen = true;
                        ESP_LOGI(TAG, "WS 音频流起链");
                    } else {
                        ESP_LOGW(TAG, "audio_start: WS 起链失败");
                    }
                } else if (cJSON_IsString(type) &&
                           strcmp(type->valuestring, "audio_end") == 0) {
                    tts_client_ws_end();
                    ESP_LOGI(TAG, "WS 音频流结束");
                }
                /* ── chat_text: 聚合协议 (1.0.259) — LLM 流式句子逐句到达,
                    只累计气泡打字机显示; 音频由 WS 二进制帧直推 (audio_start
                    已起链)。不递增 chat_seq、不落记忆/日志 — 那些只在
                    chat_done 做一次, 防重复 ── */
                else if (cJSON_IsString(type) &&
                         strcmp(type->valuestring, "chat_text") == 0) {
                    if (cJSON_IsString(txt)) {
                        char clean[512];
                        parse_reply_text(txt->valuestring, clean, sizeof(clean),
                                         NULL, 0);
                        /* 气泡累计 — 每次到达都刷新打字机与停留时长,
                         * chat_done 会以整段文本刷新总时长 */
                        size_t bl = strlen(s_stream_text);
                        size_t cl = strlen(clean);
                        if (cl > 0) {
                            if (bl + cl + 1 > sizeof(s_stream_text))
                                cl = sizeof(s_stream_text) - bl - 1; /* 截断保护 */
                            if (cl > 0) {
                                memcpy(s_stream_text + bl, clean, cl);
                                s_stream_text[bl + cl] = '\0';
                                chat_bubble_show(s_stream_text,
                                                 2000 + strlen(s_stream_text) * 220);
                            }
                        }
                        ESP_LOGI(TAG, "chat_text: %s", clean);
                    }
                } else if (cJSON_IsString(type) && cJSON_IsString(txt)) {
                    if (strcmp(type->valuestring, "chat_done") == 0 ||
                        strcmp(type->valuestring, "chat_reply") == 0) {
                        s_chat_seq++; /* 会话模式等回复的信号 */
                        s_stream_text[0] = '\0'; /* 新一轮回复 — 重置流式气泡累计 */
                        /* 服务端回显的用户原文 (后续阶段用于设备端记忆) */
                        cJSON *llm_user = cJSON_GetObjectItem(root, "user_text");
                        if (cJSON_IsString(llm_user)) {
                            snprintf(s_last_user_text, sizeof(s_last_user_text), "%s",
                                     llm_user->valuestring);
                            /* : 512B 截断可能切在 UTF-8 字符中间 —
                             * 半个字符进记忆文件 = 每次 chat 被服务端拒连 */
                            utf8_sanitize_inplace(s_last_user_text,
                                                  strlen(s_last_user_text));
                        }
                        /* strip |pXXX and [tags] for log, |pXXX only for TTS */
                        char clean[512], tts_text[512];
                        parse_reply_text(txt->valuestring, clean, sizeof(clean),
                                         tts_text, sizeof(tts_text));

                        /* 解析动画 */
                        cJSON *llm_anim = cJSON_GetObjectItem(root, "animation");
                        pet_anim_t anim = PET_ANIM_COUNT;
                        if (cJSON_IsString(llm_anim))
                            anim = parse_anim_name(llm_anim->valuestring);

                        /* 名字取预热缓存 (PSRAM 栈禁 nvs_open, init 期已预热) */
                        ESP_LOGI(TAG, "%s: %s", config_get_str("pet_name", "萝莉丝"), clean);

                        /* 动画先播 — 与流式 TTS 下载/播放并行, 不再等语音 */
                        if (anim < PET_ANIM_COUNT &&
                            strcmp(llm_anim->valuestring, "none") != 0)
                            pet_avatar_play(anim);

                        /* 设备端记忆: 追加本轮对话 (flash 写在 TTS 开播前, 不卡音频)。
                         * marker 日志: 崩溃定位 — 若停在 mem_append 之后 / TTS 之前,
                         * 即为 /cfg 写入 (LittleFS) 卡死或 INTWDT 复位点 */
                        ESP_LOGI(TAG, "chat: memory_store_append…");
                        if (s_last_user_text[0] && clean[0])
                            memory_store_append(s_last_user_text, clean);
                        ESP_LOGI(TAG, "chat: memory ok → life_log…");

                        /* 全量交互日志 (USB 直读) — 用户原文 + 宠物回复 */
                        if (s_last_user_text[0])
                            life_log_line("[%s] %s", config_get_str("owner_name", "主人"), s_last_user_text);
                        if (clean[0])
                            life_log_line("[%s] %s", config_get_str("pet_name", "萝莉丝"), clean);
                        ESP_LOGI(TAG, "chat: life_log ok → tts…");

                        /* 空文本 = 静默模式 (只做动作不说话) */
                        if (tts_text[0]) {
                            /* 聚合协议 (1.0.259): WS 音频流直推 (audio_start 已
                             * 收到) → 跳过 POST; 未收到 (服务器 TTS 失败) →
                             * 兜底整段 POST. 气泡已由 chat_text 打字机显示, 不重刷 */
                            if (s_ws_audio_seen) {
                                ESP_LOGI(TAG, "agg: WS 音频流已推, 跳过整段 TTS");
                            } else {
                                ESP_LOGW(TAG, "agg: 无 WS 音频 — 兜底整段 POST");
                                bool tts_ok = tts_speak(tts_text);
                                if (!tts_ok)
                                    tts_ok = tts_client_interrupt_speak(tts_text);
                                if (!tts_ok) {
                                    ESP_LOGE(TAG, "tts FAILED (兜底整段)");
                                    chat_bubble_show(txt->valuestring, 8000);
                                }
                            }
                        } else {
                            chat_bubble_show(txt->valuestring, 8000);
                        }

                        /* Pet engine: 新协议 mood_delta 增量直喂; 旧服务端(mood 绝对值)降级求差 */
                        cJSON *llm_mood_d = cJSON_GetObjectItem(root, "mood_delta");
                        cJSON *llm_mood = cJSON_GetObjectItem(root, "mood");
                        cJSON *llm_exp = cJSON_GetObjectItem(root, "exp");
                        int8_t exp_d = cJSON_IsNumber(llm_exp)
                                           ? (int8_t)llm_exp->valueint
                                           : 0;
                        if (cJSON_IsNumber(llm_mood_d)) {
                            pet_process_chat((int8_t)llm_mood_d->valueint, exp_d);
                        } else if (cJSON_IsNumber(llm_mood)) {
                            pet_state_t st = pet_engine_get_state();
                            pet_process_chat((int8_t)llm_mood->valueint - (int8_t)st.mood,
                                             exp_d);
                        }
                        /* 一轮结束 — 下轮 audio_start 重新置位 (防跨轮误判
                         * 兜底 POST: 上轮 WS 推过、本轮服务器 TTS 失败) */
                        s_ws_audio_seen = false;
                    }
                }
                cJSON_Delete(root);
            }
        }
    } /* end WEBSOCKET_EVENT_DATA block */
    break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WS 错误");
        s_connected = false;
        break;
    default:
        break;
    }
}

/* cJSON 全走 PSRAM — memory_data 的 100KB 字符串内嵌 RAM 放不下 */
static void *cjson_psram_malloc(size_t sz) { return heap_caps_malloc(sz, MALLOC_CAP_SPIRAM); }

esp_err_t ws_client_connect(const char *token)
{
    /* 幂等 + 可重建: 组件内置重连只覆盖"已连接后断开", 不覆盖任务创建
     * 失败 (注册后 "Error create websocket task") — 主循环 30s 周期调用
     * 本函数重建客户端。已连则直接跳过, 避免误杀正常连接。 */
    if (s_paused) return ESP_OK; /* 息屏期不重建 (WiFi 已停) */
    if (s_connected) return ESP_OK;
    if (s_client) {
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
    }
    s_connected = false;
    cJSON_InitHooks(&(cJSON_Hooks){.malloc_fn = cjson_psram_malloc, .free_fn = free});
    char uri[512];
    snprintf(uri, sizeof(uri), "ws://%s:%d/ws/device?token=%s", SERVER_HOST, SERVER_PORT, token);
    esp_websocket_client_config_t cfg = {
        .uri = uri,
        .ping_interval_sec = 30, /* 30s ping 检测死连接 (服务器重启后自动重连) */
        .pingpong_timeout_sec = 10,
        .reconnect_timeout_ms = 10000,
        .network_timeout_ms = 10000,
        /* 调用链深, 12KB 会溢出; 栈实际由 PSRAM 分配 (vendored 组件已改
         * xTaskCreatePinnedToCoreWithCaps) — 16384 恰好等于
         * CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL, 若用普通 malloc 必走内部 RAM */
        .task_stack = 16384,
    };
    s_client = esp_websocket_client_init(&cfg);
    if (!s_client) return ESP_FAIL;
    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, ws_event, NULL);
    return esp_websocket_client_start(s_client);
}

bool ws_client_is_connected(void) { return s_connected; }

/* 息屏暂停/亮屏恢复 (2026-08-22) — 与 main.c 的息屏/亮屏块配对:
 * stop 停掉组件及其内置重连计时器 (10s 周期, WiFi 停后 DNS 必败, 实测与
 * 息屏期 USB TX 静默强相关), start 重新发起连接; 对象保留, 无需重建。 */
void ws_client_pause(void)
{
    s_paused = true;
    if (s_client) esp_websocket_client_stop(s_client);
    s_connected = false;
    ESP_LOGI(TAG, "WS 暂停 (息屏)");
}

void ws_client_resume(void)
{
    s_paused = false;
    if (s_client) {
        esp_err_t ret = esp_websocket_client_start(s_client);
        if (ret != ESP_OK)
            ESP_LOGE(TAG, "WS 恢复失败: %s — 主循环 30s 周期会重建",
                     esp_err_to_name(ret));
    }
    ESP_LOGI(TAG, "WS 恢复 (亮屏)");
}
esp_err_t ws_client_send_text(const char *text)
{
    if (!s_client || !s_connected) return ESP_FAIL;
    char json[512];
    snprintf(json, sizeof(json), "{\"type\":\"chat\",\"text\":\"%s\"}", text);
    return esp_websocket_client_send_text(s_client, json, strlen(json), pdMS_TO_TICKS(100));
}
esp_err_t ws_client_send_json(const char *json)
{
    if (!s_client || !s_connected) return ESP_FAIL;
    return esp_websocket_client_send_text(s_client, json, strlen(json), pdMS_TO_TICKS(100));
}

esp_err_t ws_client_send_chat(const char *text)
{
    /* cJSON 构建: 正确处理引号/换行 + 附带记忆元数据 (缓存值, 零 FatFS 访问 —
     * 本函数会在 sess 任务 12KB 栈上执行, FatFS+WL 调用链曾栈溢出) */
    if (!s_client || !s_connected) return ESP_FAIL;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "chat");
    /* : ASR 文本同样清洗 — 任何坏字节进 WS 帧都会被服务端拒连
     * v2.19.1: 坏字节替换 '?', 全坏发空串 */
    {
        size_t tl = strlen(text);
        char *ttmp = heap_caps_malloc(tl + 1, MALLOC_CAP_SPIRAM);
        if (ttmp) {
            memcpy(ttmp, text, tl + 1);
            size_t bad = utf8_sanitize_inplace(ttmp, tl);
            cJSON_AddStringToObject(root, "text", (bad < tl) ? ttmp : "");
            free(ttmp);
        } else {
            cJSON_AddStringToObject(root, "text", text); /* PSRAM 不足 — 原样发 (极端场景) */
        }
    }
    /* 兜底: 摘要再验一次 UTF-8 — 缓存可能早于文件修复 (修复由
     * 主循环 tick 触发), 坏字节上送 = 服务端拒连。坏字节替换 '?', 全坏不带。 */
    const char *summary = memory_store_cached_summary();
    if (summary) {
        size_t sl = strlen(summary);
        char *tmp = heap_caps_malloc(sl + 1, MALLOC_CAP_SPIRAM);
        if (tmp) {
            memcpy(tmp, summary, sl + 1);
            size_t bad = utf8_sanitize_inplace(tmp, sl);
            if (bad < sl) cJSON_AddStringToObject(root, "mem_summary", tmp);
            free(tmp);
        } else {
            cJSON_AddStringToObject(root, "mem_summary", summary); /* PSRAM 不足 — 原样发 */
        }
    }
    cJSON_AddNumberToObject(root, "mem_size", (double)memory_store_cached_size());
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return ESP_FAIL;
    esp_err_t err = esp_websocket_client_send_text(s_client, json, strlen(json), pdMS_TO_TICKS(100));
    cJSON_free(json);
    return err;
}
