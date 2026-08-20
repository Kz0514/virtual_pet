/**
 * @file usb_storage.c
 * @brief U盘模式 — USB MSC 将 /data (FatFS) 暴露给电脑直读/写入
 *
 * 架构 (1.0.221 重构, 挂载生命周期全权归本模块):
 *   - sensor_logger_init 只做 WL 层初始化 (wl_mount)。FatFS 由本模块在
 *     开机注册一次 (diskio + VFS ctx + f_mount), **运行期永不注销** —
 *     进出 U盘模式只做 f_mount 分离/重挂 (纯 FatFS 操作, 零分配,
 *     不可能因堆失败; 1.0.216-220 反复实测运行期 register_cfg 会
 *     ESP_ERR_NO_MEM 且失败态留悬垂 FatFs 指针 → 0x4c 崩溃, 那套
 *     组件管理挂载 + 运行时重建的架构整体废弃)。
 *   - 组件 (esp_tinyusb) 已被 patch 成纯状态切换: msc_storage_mount /
 *     unmount 不再碰 FatFS, 只切 mount_point 标志; SCSI 读写经 wl 直读
 *     介质, 与 FatFS 无耦合。创建时 mount_point=USB, 组件从出生就不
 *     会尝试挂载。
 *   - **设备栈动态启停**: USB-SERIAL-JTAG 与 USB OTG 共享 D+/D-,
 *     常启 OTG 会抢占串口口 (COM 消失, 开发日志没得看) —
 *     平时不启设备栈 (COM6 保持), 进入 U盘模式才 tinyusb_driver_install,
 *     退出时 tinyusb_driver_uninstall + usb_phy_restore_serial_jtag()
 *     (mux 位在 RTC 域, 仅删 PHY 不会切回, 必须显式切) → 串口口恢复。
 *     APP 态插线时设备不枚举, 主机无感知。
 *   - Windows"安全弹出" (SCSI EJECT) → 组件状态切回 APP — tick 检测到
 *     mount_point 意外回 APP 即同步退出流程。
 *   - 无 VBUS 检测 GPIO → 拔线检测: 曾枚举后断开 → 30 秒宽限 (宽限内
 *     重插直接恢复会话, 不需重开模式) → 超时才退出重挂。**无 5 分钟
 *     未枚举超时** — 用户开启后随时插线都能看到磁盘 (1.0.221 起,
 *     原超时是 COM 口被抢占的补偿, 现在退出廉价可靠, 保留它反而
 *     违背"开启一次随时可用"的诉求)。
 *   - MSC 期间 /cfg (LittleFS) 照常可写 — memory 继续落盘
 *
 * 用户数据安全: 运行期自愈只重挂/重建, 永不自动 f_mkfs (主机可能刚
 * 写过文件); 格式化只有两条路: ① 设置页"格式化存储" (NVS 标志 →
 * 重启 → 整区擦除 → boot 自动 f_mkfs); ② boot 时检测到全新空白分区
 * (全 0xFF) 自动建 FAT。README.txt 由本模块在挂载后维护。
 */
#include "usb_storage.h"
#include "usb_icon.h"          /* pet.ico 字节数组 — tools/gen_icon.py 生成 (空=不设图标) */
#include "sensor_logger.h"
#include "tinyusb.h"
#include "tinyusb_msc.h"
#include "wear_levelling.h"
#include "diskio_wl.h"         /* ff_diskio_get_pdrv_wl / register — 盘号动态分配 */
#include "diskio_impl.h"       /* ff_diskio_get_drive / unregister — 重挂修复 */
#include "ff.h"                /* f_mount/f_mkfs/f_setlabel/f_chmod — 挂载生命周期 */
#include "esp_vfs_fat.h"       /* esp_vfs_fat_register_cfg — 开机一次性注册 */
#include "esp_private/usb_phy.h"
#include "esp_partition.h"     /* 重启后格式化: data 分区整区擦除 */
#include "nvs.h"               /* 格式化请求标志 (重启后执行) */
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_heap_caps.h"   /* enter 诊断: 启动设备栈前的堆状态打印 */
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"   /* vTaskDelay — 重启前留日志时间 */
#include "freertos/task.h"
#include <stdio.h>
#include <sys/stat.h>

/* 重启后格式化标志 (1.0.218): 设置页确认 → 写 NVS → esp_restart → boot 时
 * 整区擦除 data 分区 → boot 挂载失败 → medium_blank() → 自动 f_mkfs。
 * 不再运行时做 VFS 注册/f_mkfs — 1.0.216-217 实测运行数小时后堆碎片化
 * 会让那些操作 ESP_ERR_NO_MEM, 且失败态留下悬垂 FatFs 指针导致后续
 * f_getfree 崩溃 (0x4c 垃圾锁句柄 LoadProhibited)。 */
#define FMT_REQ_NS   "device"   /* 与 api_client 同命名空间 */
#define FMT_REQ_KEY  "fmt_req"

static const char *TAG = "usb_storage";
static const char *DATA_MOUNT = "/data";

/* Windows 显示的磁盘名 (FAT 卷标 ≤11 字符, ASCII; 未设置时 Windows 默认显示"U盘") */
#define USB_VOL_LABEL "VirtualPet"

/* 拔线后宽限: 期内重插直接恢复会话 (设备栈未停, tinyusb 自动 resume),
 * 超时自动退出重挂 (数据回设备端, COM 口恢复) */
#define USB_UNPLUG_GRACE_MS  (30u * 1000u)
#define TINYUSB_TASK_STACK   8192   /* 1.0.222: 组件已改 PSRAM 栈 (见 tinyusb_task.c 补丁), 加大无成本 */

static tinyusb_msc_storage_handle_t s_handle = NULL;
static bool  s_active       = false;   /* U盘模式标志 */
static bool  s_seen_mounted = false;   /* 本会话是否见过主机枚举 */
static uint32_t s_last_unplug_ms = 0;  /* 拔线时刻 (宽限计时) */
static SemaphoreHandle_t s_fs_mutex = NULL;  /* 串行化 探测 vs 挂载切换 —
                                             * 切换窗口 ("盘号已注册卷未挂") 曾被
                                             * 设置页探测抓到 → FRESULT=12 (1.0.218) */

/* 开机注册的 FATFS 对象 — 1.0.221 起 fs 与 VFS ctx 注册一次后运行期
 * 永不释放, f_mount(NULL)+f_mount(fs) 重同步是纯 FatFS 操作 (零分配,
 * 不可能因堆失败), 悬垂/双注册/槽位耗尽从架构上不可能 */
static FATFS *s_fatfs_fs = NULL;

/* FatFS 卷可用性探测: 任一盘号 f_getfree 成功且有簇大小即有效 */
static bool data_fs_probe(void);

/* 轻量重挂 (分离 + 用开机注册的 fs 重挂, 零分配) — enter 回滚也用它 */
static FRESULT fatfs_remount(void);

/* 手动重建 /data 挂载 — 开机一次性注册 (boot 堆干净, 可靠) + 自愈兜底 */
static esp_err_t repair_data_mount(bool allow_format);

/* /data 预置说明文件 — USB 直读时告知目录用途与恢复方式 */
static void write_data_readme(void)
{
    FILE *f = fopen("/data/README.txt", "w");
    if (!f) return;
    fprintf(f,
        "欢迎! 这是 Virtualpet 的存储盘, USB 连接电脑后可见。\n"
        "\n"
        "目录:\n"
        "  diary/  — 萝莉丝的日记 (HTML 文件, 双击用浏览器打开)\n"
        "  life/   — 交互日志 (与萝莉丝的每一次对话和互动)\n"
        "\n"
        "提示:\n"
        "  - 文件可自由删除/复制 (比如删掉某篇日记, 设备会自动重新同步)\n"
        "  - 若电脑提示\"此磁盘未格式化\", 请在设备设置页执行\n"
        "    \"格式化存储\", 然后重新插拔 USB\n");
    fclose(f);
}

/* FAT 隐藏属性 — pet.ico/autorun.inf 物理上必须在盘根 (Windows 靠它们
 * 找图标), 但资源管理器默认不显示 (文件资源管理器默认隐藏这两个) */
static void set_hidden_attr(const char *path)
{
    FILINFO finfo;
    if (f_stat(path, &finfo) != FR_OK) return;
    f_chmod(path, AM_HID | AM_SYS, AM_HID | AM_SYS);
}

/* /data 预置 USB 展示文件: 内置磁盘图标 + autorun.inf (Windows 读取
 * ICON= 指令在资源管理器显示自定义盘符图标; 无任何自动运行内容)。
 * 只写缺失 (宿主可能删文件, 每次退出 U盘模式后由 exit 兜底重建)。
 * 内置图标来源: tools/gen_icon.py 生成的 usb_icon.h — 想换默认图标把
 * tools/pet.ico 放好后直接构建即可 (build.bat 自动重跑 gen_icon.py)。
 * 无内置图标 (usb_icon_ico_len==0) → 不写图标文件, Windows 用系统默认
 * 磁盘图标。旧固件残留的 pet.ico/autorun.inf 不做自动清理 — 只有这一个
 * 用户, 盘上文件以"用户删了才消失"为准 (1.0.214 起) */
static void write_usb_assets(void)
{
    struct stat st;
    if (usb_icon_ico_len == 0)
        return;
    if (stat("/data/pet.ico", &st) != 0) {
        FILE *f = fopen("/data/pet.ico", "wb");
        if (f) {
            fwrite(usb_icon_ico, 1, usb_icon_ico_len, f);
            fclose(f);
        }
    }
    if (stat("/data/autorun.inf", &st) != 0) {
        FILE *f = fopen("/data/autorun.inf", "w");
        if (f) {
            fprintf(f, "[autorun]\r\nICON=pet.ico\r\n");
            fclose(f);
        }
    }
    set_hidden_attr("/data/pet.ico");
    set_hidden_attr("/data/autorun.inf");
}

/* 磁盘卷标 — Windows 以卷标显示盘名。盘号由 ff_diskio_get_drive 动态分配,
 * 不能硬编码 "0:"; 需 CONFIG_FATFS_USE_LABEL=y (f_setlabel 才编译进) */
static void ensure_volume_label(void)
{
    wl_handle_t wl = sensor_logger_get_wl_handle();
    if (wl == WL_INVALID_HANDLE) return;
    BYTE pdrv = ff_diskio_get_pdrv_wl(wl);
    if (pdrv == 0xff) return;
    char label[24];
    snprintf(label, sizeof(label), "%d:%s", (int)pdrv, USB_VOL_LABEL);
    FRESULT fr = f_setlabel(label);
    if (fr != FR_OK)
        ESP_LOGW(TAG, "f_setlabel(%s) 失败: %d", label, (int)fr);
    else
        ESP_LOGI(TAG, "磁盘卷标已设: %s", USB_VOL_LABEL);
}

/* 把内部 FSLS PHY 从 USB-OTG 切回 USB-Serial-JTAG。
 *
 * 背景: D+/D- 的归属由 RTC 域寄存器 RTCCNTL.usb_conf.sw_usb_phy_sel 决定
 * (0=内部 PHY 给 USJ, 1=给 USB Wrap), 该域不被 esp_restart (软件复位) 清除,
 * 只有真正断电 (POR) 才复位。tinyusb_driver_install 会把 PHY 切给 OTG,
 * 而 tinyusb_driver_uninstall 只关 USB Wrap 时钟、不切回 — 于是退出
 * U盘模式后 COM 口永久消失直到断电。这里用 usb_new_phy(SERIAL_JTAG)
 * 走 usb_serial_jtag_hal_phy_set_external(false) 把 mux 切回 USJ。 */
static esp_err_t usb_phy_restore_serial_jtag(void)
{
    usb_phy_config_t phy_cfg = {
        .controller = USB_PHY_CTRL_SERIAL_JTAG,
        .target     = USB_PHY_TARGET_INT,
    };
    usb_phy_handle_t phy = NULL;
    esp_err_t err = usb_new_phy(&phy_cfg, &phy);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PHY 切回 Serial-JTAG 失败: %s", esp_err_to_name(err));
        return err;
    }
    /* 切换已生效; 必须立即释放句柄 — 否则 INT PHY 保持 IN_USE,
     * 下次进入 U盘模式时 tinyusb_driver_install 的 usb_new_phy(OTG) 会失败 */
    err = usb_del_phy(phy);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "usb_del_phy 失败: %s — 不影响切换结果", esp_err_to_name(err));
    ESP_LOGI(TAG, "PHY 已切回 USB-Serial-JTAG — COM 口恢复 (Windows 可能需重插 USB 线枚举)");
    return ESP_OK;
}

static void msc_evt_cb(tinyusb_msc_storage_handle_t h, tinyusb_msc_event_t *ev, void *arg)
{
    (void) h; (void) arg;
    /* 1.0.221: 组件不再管理 FatFS, FORMAT_REQUIRED 等事件不会再来 — 仅留日志 */
    ESP_LOGI(TAG, "MSC 事件 %d (mount=%d)", (int)ev->id, (int)ev->mount_point);
}

/* 全新分区判定: 前 4 扇区全 0xFF = 擦过的 flash / 新设备, 无数据可保护。
 * 只有这种情形 boot 才自动 f_mkfs — 有数据残留绝不自动销毁 */
static bool medium_blank(void)
{
    wl_handle_t wl = sensor_logger_get_wl_handle();
    if (wl == WL_INVALID_HANDLE) return false;
    uint8_t buf[4096];
    for (int s = 0; s < 4; s++) {
        if (wl_read(wl, (size_t)s * 4096, buf, sizeof(buf)) != ESP_OK) return false;
        for (size_t i = 0; i < sizeof(buf); i++)
            if (buf[i] != 0xFF) return false;
    }
    return true;
}

esp_err_t usb_storage_init(void)
{
    if (s_handle) return ESP_OK;

    /* 1.0.222: boot 自愈 PHY — 上次会话若 U盘模式启动中途失败 (tinyusb
     * 设备栈起不来), D+/D- 可能卡在 OTG (RTCCNTL.usb_conf.sw_usb_phy_sel,
     * RTC 域, esp_restart 不清, 只有断电才复位) → COM 口永久消失。
     * 这里无条件把 mux 切回 USB-Serial-JTAG: 正常 boot (mux 已在 USJ) 时
     * usb_new_phy(SERIAL_JTAG)+usb_del_phy 是无害往返 (句柄表清零过,
     * 不报 IN_USE); 卡在 OTG 时则真正切回 — 软件重启即可恢复串口,
     * 不再需要物理断电。 */
    esp_err_t phy_err = usb_phy_restore_serial_jtag();
    if (phy_err != ESP_OK)
        ESP_LOGW(TAG, "boot PHY 自愈失败: %s — COM 口可能需断电恢复",
                 esp_err_to_name(phy_err));

    /* 1.0.223 诊断: boot 内存地图 — 曾实测开 U盘模式时内部堆仅剩 3715B
     * (任务栈 + LVGL DMA 缓冲全挤内部堆, 系统设计满载), 此日志验证
     * PSRAM 化 + ALWAYSINTERNAL=4096 后的释放效果 */
    ESP_LOGI(TAG, "内存地图: 内部堆 free=%u max=%u | PSRAM free=%u max=%u",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    /* 重启后格式化: 必须赶在 FatFS 注册之前擦除 — 卷上残留有效 FAT 时
     * 挂载会成功, "格式化"就白做了。擦完后 boot 挂载报 FR_NO_FILESYSTEM
     * → medium_blank()=true → 自动 f_mkfs 重建全新分区 */
    nvs_handle_t nvs;
    esp_err_t nvs_err = nvs_open(FMT_REQ_NS, NVS_READWRITE, &nvs);
    if (nvs_err == ESP_OK) {
        uint8_t fmt_req = 0;
        esp_err_t gerr = nvs_get_u8(nvs, FMT_REQ_KEY, &fmt_req);
        if (gerr == ESP_OK && fmt_req) {
            ESP_LOGW(TAG, "检测到格式化请求 — 擦除 data 分区");
            nvs_erase_key(nvs, FMT_REQ_KEY);   /* 先清标志 — 擦除/重启失败不再重试 */
            nvs_commit(nvs);
            wl_handle_t wl = sensor_logger_get_wl_handle();
            const esp_partition_t *part = esp_partition_find_first(
                ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "data");
            if (wl != WL_INVALID_HANDLE && part) {
                esp_err_t e = wl_erase_range(wl, 0, part->size);
                ESP_LOGI(TAG, "重启后格式化: 整区擦除 %u 字节 %s (走 boot 自动建卷)",
                         (unsigned)part->size,
                         e == ESP_OK ? "完成" : esp_err_to_name(e));
            } else {
                ESP_LOGE(TAG, "重启后格式化: 取 WL 句柄或 data 分区失败 — 跳过擦除");
            }
        } else {
            /* 1.0.222 诊断: 区分 无标志 / 键不存在 / NVS 异常 — 曾遇
             * 格式化请求后重启仍显示旧 FAT (724K), 需确认标志链 */
            ESP_LOGD(TAG, "格式化标志: %s",
                     (gerr == ESP_OK) ? (fmt_req ? "有(但非1?!)" : "无")
                                      : ((gerr == ESP_ERR_NVS_NOT_FOUND)
                                             ? "键不存在" : "读取失败"));
        }
        nvs_close(nvs);
    } else {
        ESP_LOGW(TAG, "NVS 打开失败 (%s) — 格式化请求无法执行",
                 esp_err_to_name(nvs_err));
    }

    wl_handle_t wl = sensor_logger_get_wl_handle();
    if (wl == WL_INVALID_HANDLE) return ESP_ERR_INVALID_STATE;

    if (!s_fs_mutex) {
        s_fs_mutex = xSemaphoreCreateMutex();
        if (!s_fs_mutex) return ESP_ERR_NO_MEM;
    }

    /* MSC driver (带事件回调, 替代默认的 WARN 日志回调)。
     * 设备栈不在此启动 — 它会抢占 USB-SERIAL-JTAG 串口口,
     * 仅 U盘模式期间临时启停 (见 enter/exit) */
    tinyusb_msc_driver_config_t drv_cfg = { .callback = msc_evt_cb };
    esp_err_t err = tinyusb_msc_install_driver(&drv_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MSC driver 安装失败: %s", esp_err_to_name(err));
        return err;
    }

    /* 存储实例: mount_point=USB — 1.0.221 起组件绝不尝试挂载 FatFS
     * (mount/unmount 已 patch 成纯状态切换), FatFS 由下面我们自己注册 */
    tinyusb_msc_storage_config_t cfg = {
        .medium = { .wl_handle = wl },
        .fat_fs = {
            .base_path = (char *)DATA_MOUNT,
            .config = {
                .format_if_mount_failed = false,  /* 与 do_not_format 语义一致 */
                .max_files = 4,   /* 设备同时打开 /data 文件 ≤ 2 (日记同步+life_log), 4 富余 */
                .allocation_unit_size = 4096,
            },
            .do_not_format = true,
            .format_flags = 0,
        },
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
    };
    err = tinyusb_msc_new_storage_spiflash(&cfg, &s_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MSC storage 创建失败: %s — /data 不可用, 降级运行", esp_err_to_name(err));
        return err;
    }

    /* 开机挂载: 从零注册一次 (boot 堆干净, 可靠)。FAT 缺失时:
     * 全新空白分区 (擦除/新设备) → 自动 f_mkfs; 有数据残留 → 降级,
     * 由用户走设置页"格式化存储" (不自动销毁可能可恢复的数据) */
    esp_err_t rerr = repair_data_mount(true);
    if (rerr != ESP_OK) {
        ESP_LOGE(TAG, "开机挂载失败: %s — /data 降级运行, 请到设置页执行"
                      "\"格式化存储\" (日记可经服务端重同步)",
                 esp_err_to_name(rerr));
    } else {
        ESP_LOGI(TAG, "/data 挂载成功 (盘号 %d)", usb_storage_get_drive());
    }

    /* 兜底探测: 挂载报成功但卷不可用 → 从零重建 (自愈, 1.0.215 起) */
    if (!data_fs_probe()) {
        ESP_LOGW(TAG, "启动挂载后探测失败 — 从零重建挂载");
        esp_err_t rerr2 = repair_data_mount(false);
        if (rerr2 != ESP_OK)
            ESP_LOGE(TAG, "重建失败: %s — FAT 可能损坏, 请到设置页执行"
                          "\"格式化存储\" (日记可经服务端重同步)",
                     esp_err_to_name(rerr2));
        else
            ESP_LOGI(TAG, "重建挂载成功 (盘号 %d)", usb_storage_get_drive());
    }
    if (data_fs_probe()) {
        write_data_readme();
        write_usb_assets();
        ensure_volume_label();
    }
    ESP_LOGI(TAG, "USB 存储就绪 — /data 已挂载 (U盘模式: 设置页开启, 盘号 %d)",
             usb_storage_get_drive());
    return ESP_OK;
}

bool usb_storage_is_active(void) { return s_active; }

/* 1.0.221: FatFS 挂载生命周期归本模块 — 非 U盘模式即挂载态 (写者闸门) */
bool usb_storage_data_mounted(void)
{
    return s_handle != NULL && !s_active;
}

esp_err_t usb_storage_enter(void)
{
    if (s_active) return ESP_OK;
    if (!s_handle) return ESP_ERR_INVALID_STATE;

    /* 1) 分离 FatFS — 介质交还 USB 主机独占。f_mount(NULL) 只清 FatFs[vol],
     *    fs 对象与 VFS ctx 保持注册 (零分配, 不可能因堆失败) */
    if (xSemaphoreTake(s_fs_mutex, pdMS_TO_TICKS(2000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    wl_handle_t wl = sensor_logger_get_wl_handle();
    BYTE pdrv = (wl == WL_INVALID_HANDLE) ? 0xff : ff_diskio_get_pdrv_wl(wl);
    if (pdrv != 0xff) {
        char drv[3] = {(char)('0' + pdrv), ':', 0};
        f_mount(NULL, drv, 1);
    }
    xSemaphoreGive(s_fs_mutex);

    /* 2) 先启设备栈 (插线即枚举), 再切挂载权 — 顺序反了插线会枚举失败 */
    ESP_LOGI(TAG, "U盘模式: 启动 tinyusb 设备栈 (heap 内部=%u PSRAM=%u)...",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    tinyusb_config_t tusb_cfg = {
        .task = {
            .size     = TINYUSB_TASK_STACK,
            .priority = 5,
            .xCoreID  = 1,
        },
    };
    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        /* 1.0.222: install 内部 usb_new_phy(OTG) 可能已把 D+/D- 切给 USB
         * Wrap — 组件失败路径只释放句柄, 不切回 mux → 必须显式 restore,
         * 否则 COM 口永久消失 (RTC 域, 断电才复位)。这是 1.0.221 实测
         * 症状 (开关仍关 + COM 丢 + 无U盘) 的回滚修复。 */
        ESP_LOGE(TAG, "tinyusb 设备栈启动失败: %s — 切回 Serial-JTAG, 恢复 /data 挂载 (内部堆 free=%u largest=%u)",
                 esp_err_to_name(err),
                 heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        esp_err_t perr = usb_phy_restore_serial_jtag();
        if (perr != ESP_OK)
            ESP_LOGE(TAG, "PHY 回滚也失败: %s — COM 口需断电恢复", esp_err_to_name(perr));
        /* 回滚: 重挂 FatFS (零分配) */
        if (xSemaphoreTake(s_fs_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            fatfs_remount();
            xSemaphoreGive(s_fs_mutex);
        }
        return err;
    }
    ESP_LOGI(TAG, "tinyusb 设备栈已启动 — 切换 MSC 挂载权 (内部堆 free=%u largest=%u)",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    err = tinyusb_msc_set_storage_mount_point(s_handle, TINYUSB_MSC_STORAGE_MOUNT_USB);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "切换 USB 失败: %s", esp_err_to_name(err));
        tinyusb_driver_uninstall();          /* 回滚: 释放 OTG */
        usb_phy_restore_serial_jtag();       /* 并把 PHY 切回串口口 */
        if (xSemaphoreTake(s_fs_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            fatfs_remount();
            xSemaphoreGive(s_fs_mutex);
        }
        return err;
    }
    s_active = true;
    s_seen_mounted = false;
    s_last_unplug_ms = 0;
    ESP_LOGI(TAG, "U盘模式开启 — 插上 USB 线访问磁盘; 拔线 30 秒内重插可直接恢复, "
                  "超时自动退出; 手动关闭: 设置页开关");
    return ESP_OK;
}

esp_err_t usb_storage_exit(void)
{
    if (!s_active) return ESP_OK;
    if (!s_handle) { s_active = false; return ESP_ERR_INVALID_STATE; }
    s_active = false;   /* 先清标志 — 退出期间 tick (2s 节拍, 另一任务) 检测到
                         * "已回 APP" 会再次进入本函数, 防并发重入 */

    /* 先停设备栈 (主机断开连接, 进行中的 SCSI 命令中止), 再重挂 FatFS —
     * 与 enter 对称。先挂载的话 Windows 可能仍握着卷执行 IO, 与挂载竞态,
     * FAT 状态不一致 → 设置页显示 "-" */
    esp_err_t err = tinyusb_driver_uninstall();
    if (err != ESP_OK)
        ESP_LOGW(TAG, "tinyusb 卸载失败: %s — 串口口可能需重启恢复", esp_err_to_name(err));
    /* 卸载失败也要继续重挂 /data — 否则卷一直挂不回来。PHY 切回失败则
     * COM 口需断电恢复, 已单独告警 */
    usb_phy_restore_serial_jtag();

    err = tinyusb_msc_set_storage_mount_point(s_handle, TINYUSB_MSC_STORAGE_MOUNT_APP);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "组件切回 APP 返回 %s — 由重挂流程兜底", esp_err_to_name(err));

    /* 重挂 FatFS: fs 自开机注册后从未释放, f_mount(NULL)+f_mount(fs)
     * 从磁盘重读 FAT — 主机在 U盘模式期间的改动 (拷入/删除文件) 立即可见;
     * 零分配, 不可能因堆失败 (1.0.221) */
    FRESULT fr = FR_INT_ERR;
    if (xSemaphoreTake(s_fs_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
        fr = fatfs_remount();
        xSemaphoreGive(s_fs_mutex);
    }
    if (fr == FR_OK) {
        ESP_LOGI(TAG, "U盘模式关闭 — /data 已重挂 (盘号 %d), COM 口已恢复",
                 usb_storage_get_drive());
    } else {
        /* 主机把卷格成 exFAT/NTFS 等不可挂载格式, 或介质损坏 →
         * 恢复入口 = 设置页"格式化存储" (绝不自动 f_mkfs 销毁主机刚写的数据) */
        ESP_LOGE(TAG, "重挂失败 FRESULT=%d — FAT 可能被主机改动或损坏, "
                      "请到设置页执行\"格式化存储\"", (int)fr);
    }

    write_data_readme();   /* 主机可能删过引导文件 → 恢复 (重挂失败时静默跳过) */
    write_usb_assets();
    return ESP_OK;
}

/* FatFS 卷可用性探测: 只探 WL 注册盘号 — IDF ff_disk_initialize 对未注册
 * 槽无守卫 (diskio.c:88 直接解引用), 注销盘号后全盘扫描必崩 (1.0.216) */
static bool data_fs_probe(void)
{
    wl_handle_t wl = sensor_logger_get_wl_handle();
    if (wl == WL_INVALID_HANDLE) return false;
    BYTE pdrv = ff_diskio_get_pdrv_wl(wl);
    if (pdrv == 0xff) return false;   /* 盘号未注册 → 无卷可探 */
    char dpath[12];
    snprintf(dpath, sizeof(dpath), "%d:", pdrv);
    DWORD fre_clu = 0;
    FATFS *fs = NULL;
    if (f_getfree(dpath, &fre_clu, &fs) != FR_OK || !fs || !fs->csize)
        return false;
    return true;
}

/* 轻量重挂 (1.0.221): 分离 + 用开机注册的 fs 重挂 — 纯 FatFS 操作,
 * 零分配。fs 与 VFS ctx 运行期从不释放, 故不可能悬垂/双注册/槽位耗尽。
 * 调用方必须已持有 s_fs_mutex */
static FRESULT fatfs_remount(void)
{
    wl_handle_t wl = sensor_logger_get_wl_handle();
    if (wl == WL_INVALID_HANDLE || !s_fatfs_fs) return FR_INT_ERR;
    BYTE pdrv = ff_diskio_get_pdrv_wl(wl);
    if (pdrv == 0xff) return FR_INT_ERR;
    char drv[3] = {(char)('0' + pdrv), ':', 0};
    f_mount(NULL, drv, 1);                 /* 空槽时为无害 no-op */
    return f_mount(s_fatfs_fs, drv, 1);    /* 重挂 — 从磁盘重读 FAT */
}

/* 从零重建 /data 挂载 (开机一次性注册 + 运行期自愈兜底)。
 * 全量清理: 分离所有卷 → 清 VFS ctx → 清全部盘号注册 → 注册 WL 盘号 →
 * VFS ctx → f_mount, 每步检查错误, 失败回滚不留残留。系统只有唯一 WL
 * 卷, 全量清理不会误伤其他卷。allow_format=true 时 (仅开机调用):
 * f_mount 失败且 medium_blank() → f_mkfs 建全新 FAT (FM_ANY, 与出厂
 * 自动格式化一致, 1MB 分区自动选 FAT12/16) */
static esp_err_t repair_data_mount_locked(bool allow_format)
{
    wl_handle_t wl = sensor_logger_get_wl_handle();
    if (wl == WL_INVALID_HANDLE) return ESP_ERR_INVALID_STATE;

    /* 先分离所有卷再释放 ctx — f_mount 挂载失败后 FatFs[drv] 仍指向 fs
     * (ff.c 先注册后挂载, 失败无回滚), 若直接 unregister_path 释放 ctx,
     * FatFs[drv] 变悬垂, 此后任意 f_getfree/f_mount 走进已释放内存 →
     * spinlock 垃圾锁句柄 LoadProhibited (1.0.217 设置页崩溃)。fs 还活着
     * 时 f_mount(NULL) 分离是唯一安全清理; 空槽 (FatFs 为 NULL) 无操作 */
    for (BYTE drv = 0; drv < FF_VOLUMES; drv++) {
        char d[3] = {(char)('0' + drv), ':', 0};
        f_mount(NULL, d, 1);
    }
    esp_vfs_fat_unregister_path(DATA_MOUNT);   /* 无 ctx 时返回 NOT_FOUND, 无害 */
    for (BYTE drv = 0; drv < FF_VOLUMES; drv++)
        ff_diskio_unregister(drv);             /* 无注册的槽为无害 no-op */
    s_fatfs_fs = NULL;

    BYTE pdrv = 0xFF;
    esp_err_t err = ff_diskio_get_drive(&pdrv);
    if (err != ESP_OK) return err;
    err = ff_diskio_register_wl_partition(pdrv, wl);
    if (err != ESP_OK) return err;

    char drv[3] = {(char)('0' + pdrv), ':', 0};
    esp_vfs_fat_conf_t conf = {
        .base_path = DATA_MOUNT,
        .fat_drive = drv,
        .max_files = 4,
    };
    FATFS *fs = NULL;
    err = esp_vfs_fat_register_cfg(&conf, &fs);
    if (err != ESP_OK) {
        ff_diskio_unregister(pdrv);
        return err;
    }
    FRESULT fr = f_mount(fs, drv, 1);
    if (fr != FR_OK && allow_format && medium_blank()) {
        /* 全新空白分区 (格式化请求擦除 / 新设备) → 自动建 FAT。
         * f_mkfs 需 FatFs[vol] 为空 (f_mount 失败后已分离) + diskio 已注册 */
        static uint8_t fmt_work[4096];
        ESP_LOGW(TAG, "/data 为全新分区 — 自动建 FAT (f_mkfs)");
        MKFS_PARM mkfs = { .fmt = FM_ANY, .au_size = 0 };  /* 自动选 FAT 类型/簇 (与出厂路径一致) */
        FRESULT mfr = f_mkfs(drv, &mkfs, fmt_work, sizeof(fmt_work));
        if (mfr == FR_OK)
            fr = f_mount(fs, drv, 1);
        else
            ESP_LOGE(TAG, "f_mkfs 失败: %d", (int)mfr);
    }
    if (fr != FR_OK) {
        /* fs 还活着 — 先分离再释放, 防悬垂 */
        f_mount(NULL, drv, 1);
        esp_vfs_fat_unregister_path(DATA_MOUNT);
        ff_diskio_unregister(pdrv);
        return ESP_FAIL;
    }
    s_fatfs_fs = fs;   /* 1.0.221: 此后运行期只重挂, 不再释放 */
    /* 1.0.223 诊断: 卷几何 — 曾见格式化重建后仍显示 724K (512B 几何),
     * 需确认 f_mkfs 建出的是 4Kn (ssize=4096) 还是 512B 卷 */
    ESP_LOGI(TAG, "卷几何: ssize=%u csize=%u n_fatent=%u fat=%s",
             (unsigned)fs->ssize, (unsigned)fs->csize, (unsigned)fs->n_fatent,
             fs->fs_type == FS_FAT12 ? "FAT12" : fs->fs_type == FS_FAT16 ? "FAT16" :
             fs->fs_type == FS_FAT32 ? "FAT32" : "exFAT");
    return ESP_OK;
}

/* 手动重建 /data 挂载 — 持锁执行, 探测 (LVGL 任务) 在此期间阻塞等待,
 * 不会看到"盘号已注册卷未挂"的中间态 (FRESULT=12, 1.0.218 实测) */
static esp_err_t repair_data_mount(bool allow_format)
{
    if (!s_fs_mutex) return ESP_ERR_INVALID_STATE;
    esp_err_t ret;
    if (xSemaphoreTake(s_fs_mutex, pdMS_TO_TICKS(2000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;                /* 探测卡死不应阻塞重建 */
    ret = repair_data_mount_locked(allow_format);
    xSemaphoreGive(s_fs_mutex);
    return ret;
}

/* 设置页数据分区探测 — 与 repair/挂载切换互斥。f_getfree 在切换窗口会看到
 * FatFs[vol]==NULL → FRESULT=12 (FR_NOT_ENABLED), 曾误报"-" (1.0.218) */
bool usb_storage_probe_data(uint32_t *total_kb, uint32_t *free_kb)
{
    if (total_kb) *total_kb = 0;
    if (free_kb)  *free_kb  = 0;
    if (!s_fs_mutex) return false;
    wl_handle_t wl = sensor_logger_get_wl_handle();
    if (wl == WL_INVALID_HANDLE) return false;

    bool ok = false;
    if (xSemaphoreTake(s_fs_mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
        return false;                          /* 挂载切换/重建进行中 — 探测让路 */
    BYTE pdrv = ff_diskio_get_pdrv_wl(wl);     /* 只探 WL 注册盘号 (1.0.216) */
    if (pdrv != 0xff) {
        char dpath[12];
        snprintf(dpath, sizeof(dpath), "%d:", pdrv);
        DWORD fre_clu = 0;
        FATFS *fs = NULL;
        FRESULT fr = f_getfree(dpath, &fre_clu, &fs);
        if (fr == FR_OK && fs && fs->csize) {
            if (total_kb)
                *total_kb = (uint32_t)(((uint64_t)(fs->n_fatent - 2) * fs->csize * fs->ssize) / 1024);
            if (free_kb)
                *free_kb = (uint32_t)((uint64_t)fre_clu * fs->csize * fs->ssize / 1024);
            ok = true;
        } else {
            /* 1.0.220 诊断: 区分 盘号未注册 / 卷未挂 (FRESULT=12) / 卷数据异常 */
            ESP_LOGW(TAG, "探测异常: 盘 %d f_getfree FRESULT=%d (12=卷未挂) fs=%p csize=%u",
                     (int)pdrv, (int)fr, (void *)fs,
                     (fs && fr == FR_OK) ? (unsigned)fs->csize : 0u);
        }
    } else {
        ESP_LOGW(TAG, "探测异常: WL 盘号未注册 (0xff) — /data 不可用");
    }
    xSemaphoreGive(s_fs_mutex);
    return ok;
}

/* 非 U盘模式下的卷自愈: 探测失败 → 轻量重挂 (零分配) → 全重建 (1.0.221 起
 * 无泄漏源, 安全) → 60s 降频重试。持续失败 = FAT 被主机改动或损坏,
 * 走设置页"格式化存储" (重启后格式化, 出厂路径从未失败) */
static uint32_t s_last_health_ms = 0;
static uint32_t s_health_interval_ms = 10000;

void usb_storage_tick(void)
{
    if (!s_handle) return;

    if (!s_active) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (now_ms - s_last_health_ms < s_health_interval_ms) return;
        s_last_health_ms = now_ms;
        uint32_t t = 0, f = 0;
        if (usb_storage_probe_data(&t, &f)) {
            s_health_interval_ms = 10000;
            return;
        }
        /* 先轻量重挂 (零分配) — 覆盖"卷被分离未重挂"类失败 */
        FRESULT fr = FR_INT_ERR;
        if (xSemaphoreTake(s_fs_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            fr = fatfs_remount();
            xSemaphoreGive(s_fs_mutex);
        }
        if (fr == FR_OK) {
            s_health_interval_ms = 10000;
            ESP_LOGI(TAG, "卷自愈: 重挂成功 (盘号 %d)", usb_storage_get_drive());
            return;
        }
        ESP_LOGW(TAG, "卷自愈: 轻量重挂失败 (FRESULT=%d) — 从零重建挂载", (int)fr);
        esp_err_t rerr = repair_data_mount(false);
        if (rerr == ESP_OK) {
            s_health_interval_ms = 10000;
            ESP_LOGI(TAG, "卷自愈: 重建成功 (盘号 %d)", usb_storage_get_drive());
        } else {
            s_health_interval_ms = 60000;
            ESP_LOGE(TAG, "卷自愈: 重建失败 (%s) — 60s 后重试; "
                          "持续失败请设置页\"格式化存储\"", esp_err_to_name(rerr));
        }
        return;
    }

    tinyusb_msc_mount_point_t mp;
    if (tinyusb_msc_get_storage_mount_point(s_handle, &mp) != ESP_OK) return;

    /* 1) 组件层已回 APP (Windows 安全弹出) → 同步退出流程 */
    if (mp == TINYUSB_MSC_STORAGE_MOUNT_APP) {
        ESP_LOGI(TAG, "主机已安全弹出 — 退出 U盘模式");
        usb_storage_exit();
        return;
    }

    /* 2) 拔线检测: 曾枚举后断开 → 30 秒宽限 — 期内重插直接恢复会话
     *    (设备栈未停, tinyusb 自动 resume), 超时才退出重挂 */
    bool mounted = tud_mounted();
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (mounted) {
        s_seen_mounted = true;
        s_last_unplug_ms = 0;
    } else if (s_seen_mounted) {
        if (s_last_unplug_ms == 0) {
            s_last_unplug_ms = now_ms;
            ESP_LOGI(TAG, "USB 已断开 — %u 秒内重插可直接访问, 超时自动退出",
                     USB_UNPLUG_GRACE_MS / 1000);
        } else if (now_ms - s_last_unplug_ms > USB_UNPLUG_GRACE_MS) {
            ESP_LOGI(TAG, "USB 断开超时 — 退出 U盘模式, /data 重挂");
            usb_storage_exit();
        }
    }
    /* 无"从未枚举超时" (1.0.221 起): 开启后随时插线都能看到磁盘 */
}

/* 设置页"格式化存储"确认 → 请求重启后格式化。运行时不碰 FatFS —
 * 1.0.216-217 实测: 运行数小时后内部堆仅剩 10KB, VFS ctx 注册/f_mkfs
 * 这类操作要么 NO_MEM 失败, 要么失败态留下悬垂 FatFs 指针崩掉设置页。
 * 改为: 写 NVS 标志 → esp_restart → boot 整区擦除 → 自动建 FAT
 * (出厂流程, 从未失败) */
esp_err_t usb_storage_request_format(void)
{
    if (s_active) return ESP_ERR_INVALID_STATE;   /* USB 主机正持有, 不可格式化 */

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(FMT_REQ_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "格式化请求: NVS 打开失败 (%s)", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_u8(nvs, FMT_REQ_KEY, 1);
    if (err == ESP_OK) err = nvs_commit(nvs);   /* 必须落盘 — esp_restart 不等 NVS 后台写 */
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "格式化请求: NVS 写标志失败 (%s)", esp_err_to_name(err));
        return err;
    }
    ESP_LOGW(TAG, "格式化请求已记录 — 3 秒后重启并擦除 data 分区");
    vTaskDelay(pdMS_TO_TICKS(3000));            /* 日志可见后再重启 */
    esp_restart();
    return ESP_OK;                              /* 不返回 (esp_restart 不返回) */
}

esp_err_t usb_storage_capacity(uint32_t *sector_count, uint32_t *sector_size)
{
    if (!s_handle || !sector_count || !sector_size) return ESP_ERR_INVALID_STATE;
    esp_err_t e1 = tinyusb_msc_get_storage_capacity(s_handle, sector_count);
    esp_err_t e2 = tinyusb_msc_get_storage_sector_size(s_handle, sector_size);
    if (e1 != ESP_OK || e2 != ESP_OK) return ESP_ERR_INVALID_STATE;
    return ESP_OK;
}

int usb_storage_get_drive(void)
{
    wl_handle_t wl = sensor_logger_get_wl_handle();
    if (wl == WL_INVALID_HANDLE) return -1;
    BYTE pdrv = ff_diskio_get_pdrv_wl(wl);
    return (pdrv == 0xff) ? -1 : (int)pdrv;
}
