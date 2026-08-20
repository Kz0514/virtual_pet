/**
 * @file wifi_manager.c
 * @brief WiFi 多网络管理 + SoftAP 配网门户
 *
 * 网络列表存 /data/wifi.json (最多 8 个, 按 last_ok 优先逐个尝试):
 *   - 疑似密码错误/AP 不存在 → 直接尝试下一个
 *   - 其他原因 → 同网络重试 3 次
 *   - 全部失败或列表为空 → 自动开启 SoftAP 门户 (DNS 劫持 + HTTP 页面)
 * 门户支持: 扫描 / 新增 / 删除网络, 保存后立即切 STA 连接 (无需重启).
 * 旧 NVS 单网络凭据一次性迁移.
 */

#include "board.h"
#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

static const char *TAG = "wifi";

/* 存储 — /cfg (LittleFS 内部分区, 掉电安全; 由 sensor_logger_init 挂载) */
#define WIFI_SAVE_FILE      "/cfg/wifi.json"
#define WIFI_SAVE_TMP       "/cfg/wifi.tmp"
#define WIFI_MAX_NETWORKS   8

/* 旧 NVS 单网络凭据 (仅迁移用) */
#define NVS_NAMESPACE       "wifi_cfg"
#define NVS_KEY_SSID        "ssid"
#define NVS_KEY_PASS        "password"

/* SoftAP 配置 */
#define SOFTAP_SSID_PREFIX  "Virtualpet-"
#define SOFTAP_MAX_CONN     4

/* 连接超时 */
#define WIFI_CONNECT_TIMEOUT_MS  30000
#define WIFI_RETRY_MAX           3

/* ── 状态 ── */
static wifi_state_t s_state = WIFI_DISCONNECTED;
static char s_ip_str[16] = {0};

/* Netif 句柄 */
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif  = NULL;

/* 网络列表 */
typedef struct {
    char ssid[33];
    char password[65];
    bool last_ok;
} saved_network_t;
static saved_network_t s_networks[WIFI_MAX_NETWORKS];
static int  s_net_count = 0;
static int  s_cur_idx   = -1;      /* 当前正在尝试的网络 */
static int  s_attempts  = 0;

static esp_timer_handle_t s_timeout_timer = NULL;
static httpd_handle_t s_httpd = NULL;
/* 控制任务 — 门户启动/切STA 等重操作必须脱离 WiFi 事件回调与 httpd
 * 任务上下文执行 (事件回调里阻塞扫描会死锁; httpd 任务里 httpd_stop
 * 会杀掉自己, 后续连接代码不再执行) */
static TaskHandle_t s_ctrl_task = NULL;
static int  s_ctrl_cmd = 0;      /* 0=空闲, 1=开门户, 2=关门户并切 STA */
static int  s_ctrl_arg = 0;

/* 预扫描缓存 — 门户启动前 STA 纯模式扫描 (AP 激活后 scan 受单射频/
 * 信道限制不可靠, 还会把已连手机的连接踢掉) */
#define SCAN_CACHE_MAX 20
static char s_scan_cache[SCAN_CACHE_MAX][33];
static int  s_scan_cache_count = 0;

static esp_err_t wifi_station_connect(int idx);
static esp_err_t wifi_softap_start(void);

/* ── 配网页 HTML ── */
static const char *CAPTIVE_HTML =
"<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Virtualpet 配网</title><style>"
"*{margin:0;padding:0;box-sizing:border-box}"
"body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;background:#1a1a2e;color:#eee;min-height:100vh;display:flex;align-items:center;justify-content:center}"
".card{background:#16213e;border-radius:16px;padding:32px 24px;max-width:380px;width:92%;text-align:center;box-shadow:0 8px 32px rgba(0,0,0,.3)}"
"h1{font-size:24px;margin-bottom:8px}h1 span{font-size:16px;opacity:.7}"
"p.sub{font-size:13px;opacity:.6;margin-bottom:24px}"
"label{display:block;text-align:left;font-size:13px;margin:12px 0 4px;opacity:.8}"
"input,select{width:100%;padding:12px;border:1px solid #0f3460;border-radius:10px;background:#1a1a2e;color:#fff;font-size:15px;margin-bottom:8px}"
"button{width:100%;padding:14px;border:none;border-radius:10px;background:#e94560;color:#fff;font-size:16px;font-weight:700;cursor:pointer;margin-top:8px}"
"button:active{background:#c23152}"
".ok{color:#4ecca3;font-size:14px;margin-top:12px;display:none}"
".net{display:flex;align-items:center;justify-content:space-between;background:#1a1a2e;border:1px solid #0f3460;border-radius:10px;padding:10px 12px;margin-bottom:8px;font-size:14px}"
".net b{max-width:200px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}"
".net button{width:auto;padding:6px 14px;font-size:12px;margin:0;background:#0f3460}"
"hr{border:none;border-top:1px solid #0f3460;margin:24px 0}"
"</style></head><body><div class='card'>"
"<h1>🐾 Virtualpet<br><span>萝莉丝 配网</span></h1>"
"<p class='sub' id='sub'>已保存的网络与新增连接</p>"
"<div id='saved'></div>"
"<hr>"
"<form id='f' onsubmit='return submitWifi(event)'>"
"<label>WiFi 名称 (SSID)</label>"
"<select id='ssid_select'><option value=''>扫描中…</option></select>"
"<input type='text' id='ssid' placeholder='或手动输入WiFi名称'>"
"<label>WiFi 密码</label>"
"<input type='password' id='pwd' placeholder='请输入WiFi密码'>"
"<button type='submit'>连接 Wi-Fi</button>"
"</form><p class='ok' id='ok'>✅ 已保存! 设备正在连接…</p>"
"</div><script>"
"async function loadSaved(){try{const r=await fetch('/list');const list=await r.json();"
"const d=document.getElementById('saved');"
"d.innerHTML=list.map(n=>`<div class='net'><b>${n}</b><button onclick='del(\"${n}\")'>删除</button></div>`).join('')}"
"catch(e){}}"   /* 闭合 catch + loadSaved 函数体 — 缺此括号整段 JS 语法错误不执行 */
"async function del(ssid){await fetch('/delete',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid})});loadSaved()}"
"async function scan(){try{const r=await fetch('/scan');const list=await r.json();"
"const s=document.getElementById('ssid_select');s.innerHTML=list.map(ssid=>`<option value='${ssid}'>${ssid}</option>`).join('');"
"document.getElementById('ssid').style.display=(list.length?'none':'block')}"
"catch(e){document.getElementById('ssid').style.display='block'}}"
"async function submitWifi(e){e.preventDefault();"
"const sel=document.getElementById('ssid_select');"
"const ssid=sel.value||document.getElementById('ssid').value;"
"const pwd=document.getElementById('pwd').value;"
"await fetch('/connect',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid,pwd})});"
"document.getElementById('f').style.display='none';document.getElementById('ok').style.display='block';"
"return false}"
"scan();loadSaved();"
"</script></body></html>";

/* ═══════════════ 网络列表存储 ═══════════════ */

static int find_network(const char *ssid)
{
    for (int i = 0; i < s_net_count; i++)
        if (strcmp(s_networks[i].ssid, ssid) == 0) return i;
    return -1;
}

static void save_wifi_list(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < s_net_count; i++) {
        cJSON *n = cJSON_CreateObject();
        cJSON_AddStringToObject(n, "ssid", s_networks[i].ssid);
        cJSON_AddStringToObject(n, "password", s_networks[i].password);
        cJSON_AddBoolToObject(n, "last_ok", s_networks[i].last_ok);
        cJSON_AddItemToArray(arr, n);
    }
    cJSON_AddItemToObject(root, "networks", arr);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return;
    /* 原子写: 先写临时文件再 rename — 断电最坏丢新内容, 不毁旧文件 */
    FILE *f = fopen(WIFI_SAVE_TMP, "w");
    if (!f) { vTaskDelay(pdMS_TO_TICKS(50)); f = fopen(WIFI_SAVE_TMP, "w"); }
    if (f) {
        fwrite(json, 1, strlen(json), f);
        fclose(f);
        remove(WIFI_SAVE_FILE);
        if (rename(WIFI_SAVE_TMP, WIFI_SAVE_FILE) != 0) {
            /* rename 失败兜底: 直写目标文件, 数据不能丢 */
            ESP_LOGW(TAG, "rename 失败 (errno=%d), 直写兜底", errno);
            FILE *g = fopen(WIFI_SAVE_FILE, "w");
            if (g) { fwrite(json, 1, strlen(json), g); fclose(g); }
        }
    } else {
        ESP_LOGW(TAG, "写入 %s 失败 (errno=%d)", WIFI_SAVE_FILE, errno);
    }
    cJSON_free(json);
}

static void load_wifi_list(void)
{
    /* 主文件缺失但 .tmp 存在 → 上次 rename 被打断, 用 .tmp 恢复 */
    FILE *f = fopen(WIFI_SAVE_FILE, "r");
    if (!f) {
        FILE *t = fopen(WIFI_SAVE_TMP, "r");
        if (t) { fclose(t); rename(WIFI_SAVE_TMP, WIFI_SAVE_FILE); }
        f = fopen(WIFI_SAVE_FILE, "r");
    }
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0 && sz < 8192) {
            char *buf = malloc((size_t)sz + 1);
            if (buf) {
                size_t n = fread(buf, 1, (size_t)sz, f);
                buf[n] = '\0';
                cJSON *root = cJSON_Parse(buf);
                cJSON *arr = root ? cJSON_GetObjectItem(root, "networks") : NULL;
                if (arr && cJSON_IsArray(arr)) {
                    int i = 0;
                    cJSON *item;
                    cJSON_ArrayForEach(item, arr) {
                        if (i >= WIFI_MAX_NETWORKS) break;
                        cJSON *ssid = cJSON_GetObjectItem(item, "ssid");
                        cJSON *pwd  = cJSON_GetObjectItem(item, "password");
                        cJSON *ok   = cJSON_GetObjectItem(item, "last_ok");
                        if (!cJSON_IsString(ssid) || !ssid->valuestring[0]) continue;
                        strncpy(s_networks[i].ssid, ssid->valuestring, 32);
                        strncpy(s_networks[i].password,
                                cJSON_IsString(pwd) ? pwd->valuestring : "", 64);
                        s_networks[i].last_ok = cJSON_IsBool(ok) && cJSON_IsTrue(ok);
                        i++;
                    }
                    s_net_count = i;
                }
                if (root) cJSON_Delete(root);
                free(buf);
            }
        }
        fclose(f);
        if (s_net_count > 0) {
            ESP_LOGI(TAG, "加载 %d 个已保存网络", s_net_count);
            return;
        }
    }

    /* 旧 NVS 单网络凭据迁移一次 */
    nvs_handle_t nvs;
    char ssid[33] = {0}, pwd[64] = {0};
    size_t len;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        len = sizeof(ssid);
        if (nvs_get_str(nvs, NVS_KEY_SSID, ssid, &len) == ESP_OK && ssid[0]) {
            len = sizeof(pwd);
            nvs_get_str(nvs, NVS_KEY_PASS, pwd, &len);
            nvs_close(nvs);
            strncpy(s_networks[0].ssid, ssid, 32);
            strncpy(s_networks[0].password, pwd, 64);
            s_networks[0].last_ok = true;
            s_net_count = 1;
            ESP_LOGI(TAG, "NVS 旧凭据迁移到 %s", WIFI_SAVE_FILE);
            save_wifi_list();
            nvs_handle_t rw;
            if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &rw) == ESP_OK) {
                nvs_erase_key(rw, NVS_KEY_SSID);
                nvs_erase_key(rw, NVS_KEY_PASS);
                nvs_erase_key(rw, "backup");   /* 旧版 NVS 兜底键已废弃, 一并清除 */
                nvs_commit(rw);
                nvs_close(rw);
            }
            return;
        }
        nvs_close(nvs);
    }
    ESP_LOGI(TAG, "无已保存网络");
}

/* ═══════════════ DNS 劫持 (门户自动弹出) ═══════════════ */

/* 扫描并去重, 返回数量 — 独立函数供预扫描与页面复用 */
static int do_scan(char ssids[][33], int max)
{
    wifi_scan_config_t scan_cfg = {
        .show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    if (esp_wifi_scan_start(&scan_cfg, true) != ESP_OK) {
        ESP_LOGW(TAG, "扫描失败");
        return 0;
    }
    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) return 0;
    wifi_ap_record_t *aps = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (!aps) return 0;
    esp_wifi_scan_get_ap_records(&ap_count, aps);
    int n = 0;
    for (int i = 0; i < ap_count && n < max; i++) {
        if (aps[i].ssid[0] == '\0') continue;
        bool dup = false;
        for (int j = 0; j < n; j++)
            if (strcmp(ssids[j], (char *)aps[i].ssid) == 0) { dup = true; break; }
        if (dup) continue;
        strncpy(ssids[n], (char *)aps[i].ssid, 32);
        ssids[n][32] = '\0';
        n++;
    }
    free(aps);
    return n;
}

static TaskHandle_t s_dns_task = NULL;
static int s_dns_sock = -1;

static void dns_task(void *pv)
{
    struct sockaddr_in addr = {
        .sin_family = AF_INET, .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    s_dns_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_dns_sock < 0) { ESP_LOGW(TAG, "DNS socket 创建失败"); vTaskDelete(NULL); return; }
    if (bind(s_dns_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(s_dns_sock); s_dns_sock = -1;
        ESP_LOGW(TAG, "DNS bind 失败");
        vTaskDelete(NULL); return;
    }
    ESP_LOGI(TAG, "DNS 劫持就绪 (所有域名 → 8.8.8.8)");
    uint8_t q[512];
    while (1) {
        struct sockaddr_in client;
        socklen_t clen = sizeof(client);
        int n = recvfrom(s_dns_sock, q, sizeof(q), 0,
                         (struct sockaddr *)&client, &clen);
        if (n < 12) continue;
        /* 定位 question 末尾 (QNAME 标签链 + QTYPE/QCLASS) */
        int p = 12;
        while (p < n && q[p] != 0) {
            if ((q[p] & 0xC0) == 0xC0) { p += 2; break; }  /* 压缩指针 */
            p += 1 + q[p];
        }
        if (p < n && q[p] == 0) p++;
        int qend = p + 4;
        /* 注意: 不能检查 qend+16>n — 普通查询 (无 OPT) 恰好 qend 字节,
         * 该检查导致所有查询被静默丢弃、应答从未发出 (手机无限重试的根因) */
        uint8_t resp[512];
        memcpy(resp, q, qend);
        resp[2] = 0x81; resp[3] = 0x80;   /* 标准响应 */
        resp[6] = 0; resp[7] = 0;         /* ANCOUNT = 0 (默认) */
        resp[8] = 0; resp[9] = 0;         /* NSCOUNT = 0 */
        resp[10] = 0; resp[11] = 0;       /* ARCOUNT = 0 — 丢弃 EDNS0 OPT,
                                            否则声称有附加记录却没内容,
                                            整包非法被解析器丢弃 (无限重试) */
        /* 仅 A 查询给 8.8.8.8; AAAA 返回空答案让解析器回退 A */
        uint16_t qtype = ((uint16_t)q[qend - 4] << 8) | q[qend - 3];
        int rlen = qend;
        if (qtype == 0x0001) {
            resp[7] = 1;                   /* ANCOUNT = 1 */
            uint8_t *a = resp + qend;      /* 答案: A 8.8.8.8 (= AP 本机) */
            a[0]=0xc0; a[1]=0x0c; a[2]=0x00; a[3]=0x01; a[4]=0x00; a[5]=0x01;
            a[6]=0; a[7]=0; a[8]=0; a[9]=60;
            a[10]=0x00; a[11]=0x04;
            a[12]=8; a[13]=8; a[14]=8; a[15]=8;
            rlen = qend + 16;
        }
        if (sendto(s_dns_sock, resp, rlen, 0,
                   (struct sockaddr *)&client, clen) < 0)
            ESP_LOGE(TAG, "DNS sendto 失败: errno=%d", errno);
    }
}

static void dns_start(void)
{
    if (s_dns_task) return;
    /* 1.0.224: 回退内部栈 — PSRAM 栈在 flash 写冻结窗口被调度即崩 */
    xTaskCreate(dns_task, "dns53", 4096, NULL, 5, &s_dns_task);
}

static void dns_stop(void)
{
    if (s_dns_sock >= 0) { close(s_dns_sock); s_dns_sock = -1; }
    if (s_dns_task) { vTaskDelete(s_dns_task); s_dns_task = NULL; }
}

/* ═══════════════ 门户 HTTP 处理器 ═══════════════ */

static esp_err_t captive_handler(httpd_req_t *req)
{
    /* 各平台联网检测探测: 必须返回"非预期内容"手机才会判定为认证网络.
     * generate_204 用 302 跳转 (参考实现的做法, 部分 ROM 只认 3xx);
     * 其余 (hotspot-detect.html / connecttest.txt 等) 一律配网页 200 */
    if (strstr(req->uri, "generate_204") || strstr(req->uri, "gen_204")) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://8.8.8.8/");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, CAPTIVE_HTML, strlen(CAPTIVE_HTML));
    return ESP_OK;
}

/* 扫描 → ["SSID1", ...] — 实时扫描为主, 失败/空回退预扫描缓存 */
static esp_err_t wifi_scan(httpd_req_t *req)
{
    char ssids[SCAN_CACHE_MAX][33];
    int n = do_scan(ssids, SCAN_CACHE_MAX);
    if (n == 0) {   /* 实时扫描失败/空 → 回退预扫描缓存 */
        n = s_scan_cache_count;
        for (int i = 0; i < n; i++)
            strncpy(ssids[i], s_scan_cache[i], 33);
    }
    ESP_LOGI(TAG, "/scan → %d 个 AP", n);

    char *json = malloc(n * 40 + 4);
    if (!json) { httpd_resp_sendstr(req, "[]"); return ESP_OK; }
    int pos = sprintf(json, "[");
    for (int i = 0; i < n; i++)
        pos += sprintf(json + pos, "%s\"%s\"", (pos > 1 ? "," : ""), ssids[i]);
    pos += sprintf(json + pos, "]");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, pos);
    free(json);
    return ESP_OK;
}

/* 已保存网络列表 → ["ssid1", ...] */
static esp_err_t wifi_list_handler(httpd_req_t *req)
{
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < s_net_count; i++)
        cJSON_AddItemToArray(arr, cJSON_CreateString(s_networks[i].ssid));
    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!json) { httpd_resp_sendstr(req, "[]"); return ESP_OK; }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    cJSON_free(json);
    return ESP_OK;
}

static esp_err_t wifi_delete_handler(httpd_req_t *req)
{
    char buf[256];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        cJSON *root = cJSON_Parse(buf);
        cJSON *ssid = root ? cJSON_GetObjectItem(root, "ssid") : NULL;
        if (cJSON_IsString(ssid)) {
            int idx = find_network(ssid->valuestring);
            if (idx >= 0) {
                for (int i = idx; i < s_net_count - 1; i++)
                    s_networks[i] = s_networks[i + 1];
                s_net_count--;
                save_wifi_list();
                ESP_LOGI(TAG, "已删除网络: %s", ssid->valuestring);
            }
        }
        if (root) cJSON_Delete(root);
    }
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* 新增/更新网络 → 保存 → 切 STA 立即连接 (AP 与 STA 须同信道, 门户期间 STA 受限) */
static esp_err_t wifi_connect_handler(httpd_req_t *req)
{
    char buf[256];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "空请求"); return ESP_FAIL; }
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    cJSON *ssid = root ? cJSON_GetObjectItem(root, "ssid") : NULL;
    cJSON *pwd  = root ? cJSON_GetObjectItem(root, "pwd")  : NULL;
    if (!cJSON_IsString(ssid) || !ssid->valuestring[0]) {
        if (root) cJSON_Delete(root);
        httpd_resp_sendstr(req, "{\"error\":\"SSID为空\"}");
        return ESP_FAIL;
    }

    int idx = find_network(ssid->valuestring);
    if (idx < 0) {
        idx = s_net_count < WIFI_MAX_NETWORKS ? s_net_count++
                                              : WIFI_MAX_NETWORKS - 1;  /* 满则覆盖末位 */
    }
    strncpy(s_networks[idx].ssid, ssid->valuestring, 32);
    strncpy(s_networks[idx].password,
            cJSON_IsString(pwd) ? pwd->valuestring : "", 64);
    for (int i = 0; i < s_net_count; i++)
        s_networks[i].last_ok = (i == idx);
    if (root) cJSON_Delete(root);
    save_wifi_list();
    ESP_LOGI(TAG, "收到配网: SSID=%s", s_networks[idx].ssid);

    httpd_resp_sendstr(req, "{\"ok\":true}");

    /* 关闭门户 → 切 STA 连接 (无需重启) — 转控制任务执行 */
    s_ctrl_cmd = 2;
    s_ctrl_arg = idx;
    xTaskNotifyGive(s_ctrl_task);
    return ESP_OK;
}

static esp_err_t http_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_open_sockets = 12;   /* 激进门户探测会并发大量连接, 默认 7 不够 */
    ESP_RETURN_ON_ERROR(httpd_start(&s_httpd, &cfg), TAG, "HTTP服务器启动失败");

    httpd_uri_t uri_page   = { .uri = "/",        .method = HTTP_GET,  .handler = captive_handler };
    httpd_uri_t uri_conn   = { .uri = "/connect", .method = HTTP_POST, .handler = wifi_connect_handler };
    httpd_uri_t uri_scan   = { .uri = "/scan",    .method = HTTP_GET,  .handler = wifi_scan };
    httpd_uri_t uri_list   = { .uri = "/list",    .method = HTTP_GET,  .handler = wifi_list_handler };
    httpd_uri_t uri_delete = { .uri = "/delete",  .method = HTTP_POST, .handler = wifi_delete_handler };
    httpd_uri_t uri_any    = { .uri = "/*",       .method = HTTP_GET,  .handler = captive_handler };
    httpd_uri_t uri_any_p  = { .uri = "/*",       .method = HTTP_POST, .handler = captive_handler };
    httpd_register_uri_handler(s_httpd, &uri_page);
    httpd_register_uri_handler(s_httpd, &uri_conn);
    httpd_register_uri_handler(s_httpd, &uri_scan);
    httpd_register_uri_handler(s_httpd, &uri_list);
    httpd_register_uri_handler(s_httpd, &uri_delete);
    httpd_register_uri_handler(s_httpd, &uri_any);     /* 兜底: 其余请求一律门户页/探测应答 */
    httpd_register_uri_handler(s_httpd, &uri_any_p);
    ESP_LOGI(TAG, "配网HTTP服务器已启动 (http://8.8.8.8)");
    return ESP_OK;
}

/* ═══════════════ 连接逻辑 ═══════════════ */

static bool is_bad_cred_reason(uint16_t reason)
{
    switch (reason) {
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:  /* 密码错误常见表现 */
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
        return true;
    default:
        /* NO_AP_FOUND(201) 等走重试路径 — AP 可能暂时没搜到, 重试常成功 */
        return false;
    }
}

static void try_next_network(void)
{
    s_attempts = 0;
    if (++s_cur_idx >= s_net_count) {
        ESP_LOGW(TAG, "所有已保存网络均失败, 进入配网模式");
        /* 本函数运行在 WIFI 事件回调上下文 — 阻塞扫描/set_mode/httpd
         * 会卡死 WiFi 事件循环 (扫描完成事件无法送达), 必须转任务执行 */
        s_ctrl_cmd = 1;
        xTaskNotifyGive(s_ctrl_task);
        return;
    }
    wifi_station_connect(s_cur_idx);
}

static void wifi_ctrl_task(void *pv)
{
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        int cmd = s_ctrl_cmd;
        s_ctrl_cmd = 0;
        if (cmd == 1) {
            wifi_softap_start();
        } else if (cmd == 2) {
            /* 关门户切 STA: 先让 HTTP 响应发出, 再停服务器 — 不能在
             * httpd 任务内自停 (会杀掉任务, 连接代码永不执行) */
            vTaskDelay(pdMS_TO_TICKS(500));
            if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }
            dns_stop();
            wifi_station_connect(s_ctrl_arg);
        }
    }
}

static void connect_timeout_cb(void *arg)
{
    if (s_state != WIFI_CONNECTING) return;
    ESP_LOGW(TAG, "连接超时 (%ds) — 切换下一个网络",
             WIFI_CONNECT_TIMEOUT_MS / 1000);
    s_attempts = WIFI_RETRY_MAX;   /* 让 DISCONNECTED 事件直接切换网络 */
    esp_wifi_disconnect();
}

static esp_err_t wifi_station_connect(int idx)
{
    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, s_networks[idx].ssid, 32);
    strncpy((char *)cfg.sta.password, s_networks[idx].password, 63);
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "设置STA模式失败");
    vTaskDelay(pdMS_TO_TICKS(100));  /* 等 STA 状态机就绪 */
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "设置STA配置失败");

    s_cur_idx = idx;
    s_attempts = 0;
    s_state = WIFI_CONNECTING;
    ESP_LOGI(TAG, "正在连接 WiFi: %s…", s_networks[idx].ssid);
    esp_wifi_connect();
    esp_timer_start_once(s_timeout_timer, WIFI_CONNECT_TIMEOUT_MS * 1000ULL);
    return ESP_OK;
}

/* ── WiFi 事件处理 ── */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *d = data;
            if (s_state == WIFI_CONNECTED) {
                /* 已连上后掉线: 同网络重连 */
                ESP_LOGW(TAG, "WiFi 断开: reason=%d, 重连…", d->reason);
                s_state = WIFI_CONNECTING;
                esp_wifi_connect();
                esp_timer_start_once(s_timeout_timer, WIFI_CONNECT_TIMEOUT_MS * 1000ULL);
            } else if (s_state == WIFI_CONNECTING) {
                bool bad_cred = is_bad_cred_reason(d->reason);
                if (!bad_cred && ++s_attempts < WIFI_RETRY_MAX) {
                    ESP_LOGW(TAG, "连接失败 reason=%d, 重试 %d/%d",
                             d->reason, s_attempts, WIFI_RETRY_MAX);
                    esp_wifi_connect();
                } else {
                    ESP_LOGW(TAG, "网络 \"%s\" 失败 (reason=%d%s) — 尝试下一个",
                             s_cur_idx >= 0 ? s_networks[s_cur_idx].ssid : "?",
                             d->reason, bad_cred ? ", 疑似密码错误" : "");
                    try_next_network();
                }
            }
            break;
        }
        case WIFI_EVENT_AP_STACONNECTED:
            ESP_LOGI(TAG, "设备已连接SoftAP");
            break;
        default: break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&evt->ip_info.ip));
        ESP_LOGI(TAG, "WiFi 已连接! IP: %s", s_ip_str);
        esp_wifi_set_ps(WIFI_PS_NONE);  /* 禁用省电避免断连 */
        esp_timer_stop(s_timeout_timer);
        if (s_cur_idx >= 0) {
            for (int i = 0; i < s_net_count; i++)
                s_networks[i].last_ok = (i == s_cur_idx);
            save_wifi_list();
        }
        s_state = WIFI_CONNECTED;
    }
}

/* ── SoftAP 模式 ── */
static esp_err_t wifi_softap_start(void)
{
    /* 从 MAC 地址生成唯一 SSID */
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    char ssid[32];
    snprintf(ssid, sizeof(ssid), SOFTAP_SSID_PREFIX "%02X%02X",
             mac[4], mac[5]);

    /* 预扫描: 门户启动前 STA 纯模式扫一次作兜底缓存 (页面打开时的
     * 实时扫描为主路径 — 旧版行为, 实测可靠) */
    esp_wifi_set_mode(WIFI_MODE_STA);
    vTaskDelay(pdMS_TO_TICKS(100));
    s_scan_cache_count = do_scan(s_scan_cache, SCAN_CACHE_MAX);
    ESP_LOGI(TAG, "预扫描完成: %d 个 AP", s_scan_cache_count);

    wifi_config_t cfg = {
        .ap = {
            .ssid_len = 0,
            .channel = 1,
            .authmode = WIFI_AUTH_OPEN,
            .max_connection = SOFTAP_MAX_CONN,
        },
    };
    strncpy((char *)cfg.ap.ssid, ssid, 32);
    strncpy((char *)cfg.ap.password, "", 64);

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "设置AP+STA模式失败");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &cfg), TAG, "设置AP配置失败");

    /* AP IP 设为 8.8.8.8 — 部分 Android/ROM 硬编码 8.8.8.8 为 DNS 服务器
     * 无视 DHCP 下发; AP 自己占住 8.8.8.8 后, 这些查询也会落到我们的
     * DNS 套接字上, 门户探测不再漏 (Arduino 论坛实测 4.3~12 稳定触发) */
    esp_netif_ip_info_t ip_info;
    ip_info.ip.addr      = esp_ip4addr_aton("8.8.8.8");
    ip_info.gw.addr      = esp_ip4addr_aton("8.8.8.8");
    ip_info.netmask.addr = esp_ip4addr_aton("255.255.255.0");
    esp_netif_dhcps_stop(s_ap_netif);
    esp_netif_set_ip_info(s_ap_netif, &ip_info);
    esp_netif_dhcps_start(s_ap_netif);

    /* DHCP option 6: DNS 广告为本机 (8.8.8.8) */
    esp_netif_dns_info_t dns = { .ip.u_addr.ip4.addr = esp_ip4addr_aton("8.8.8.8") };
    esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET,
                           ESP_NETIF_DOMAIN_NAME_SERVER, &dns, sizeof(dns));
    ESP_LOGI(TAG, "DHCP DNS 已广告为 8.8.8.8");

    /* 启动 HTTP 配网服务器 + DNS 劫持 */
    ESP_RETURN_ON_ERROR(http_server_start(), TAG, "HTTP服务器启动失败");
    dns_start();

    s_state = WIFI_SOFTAP_MODE;
    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, "  配网模式已启动!");
    ESP_LOGI(TAG, "  1. 手机连接 WiFi: %s", ssid);
    ESP_LOGI(TAG, "  2. 浏览器打开 http://8.8.8.8");
    ESP_LOGI(TAG, "============================================");
    return ESP_OK;
}

/* ── 公共 API ── */
esp_err_t wifi_manager_init(void)
{
    ESP_LOGI(TAG, "初始化 WiFi…");

    /* 初始化 TCP/IP 栈 + 事件循环 */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* 创建 Station 和 AP 接口 */
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif  = esp_netif_create_default_wifi_ap();

    /* WiFi 初始化 */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* 注册事件处理 */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                        &wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* 连接超时定时器 */
    esp_timer_create_args_t tcfg = {
        .callback = connect_timeout_cb, .name = "wifi_timeout",
    };
    ESP_ERROR_CHECK(esp_timer_create(&tcfg, &s_timeout_timer));

    /* WiFi 控制任务 (门户启动/关闭切换, 必须脱离事件回调与 httpd 上下文) */
    xTaskCreate(wifi_ctrl_task, "wifi_ctrl", 4096, NULL, 5, &s_ctrl_task);

    /* 加载网络列表 (JSON + 旧 NVS 迁移) */
    load_wifi_list();

    if (s_net_count > 0) {
        int idx = 0;
        for (int i = 0; i < s_net_count; i++)
            if (s_networks[i].last_ok) { idx = i; break; }
        return wifi_station_connect(idx);
    }
    ESP_LOGI(TAG, "无已保存网络, 进入配网模式");
    return wifi_softap_start();
}

wifi_state_t wifi_get_state(void)   { return s_state; }
bool wifi_is_connected(void)        { return s_state == WIFI_CONNECTED; }
const char *wifi_get_ip(void)       { return s_ip_str; }
