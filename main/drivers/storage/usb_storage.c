/**
 * @file usb_storage.c
 * @brief U盘模式 — USB MSC 将 /data (FatFS) 暴露给电脑直读/写入
 *
 * 架构:
 *   - sensor_logger_init 只做 WL 层初始化 (wl_mount)。FatFS 由本模块在
 *     开机注册一次 (diskio + VFS ctx + f_mount), **运行期永不注销** —
 *     进出 U盘模式只做 f_mount 分离/重挂 (纯 FatFS 操作, 零分配,
 *     不可能因堆失败; 运行期 register_cfg 会 NO_MEM 且留悬垂指针崩溃)。
 *   - 组件 (esp_tinyusb) 已被 patch 成纯状态切换: msc_storage_mount /
 *     unmount 不再碰 FatFS, 只切 mount_point 标志; SCSI 读写经 wl 直读
 *     介质, 与 FatFS 无耦合。创建时 mount_point=USB, 组件从出生就不
 *     会尝试挂载。
 *   - **设备栈动态启停**: USB-SERIAL-JTAG 与 USB OTG 共享 D+/D-,
 *     常启 OTG 会抢占串口口 (COM 消失) — 平时不启设备栈, 进入 U盘
 *     模式才 tinyusb_driver_install, 退出时 uninstall +
 *     usb_phy_restore_serial_jtag() (mux 位在 RTC 域, 必须显式切回)。
 *     APP 态插线时设备不枚举, 主机无感知。
 *   - **模式粘滞**: U盘开关 = 手动进入/退出 (拔线不自动退出 — S3 dcd
 *     不检测拔线, SEDET 依赖 VBUS 感应, 自动退出从未生效)。设备栈保持,
 *     重插 USB reset 自动重枚举。
 *   - **禁睡跟随充电状态**: usb 禁睡锁跟随 bq27220 电流 — 插线
 *     (充电中或满电停充, current_ma ≥ -5mA) 持锁禁睡保活不掉盘;
 *     拔线 (放电) 30s 宽限后释放锁 → 恢复轻睡。睡着时重插 (设备未醒)
 *     会掉盘 — 触摸唤醒后充电检测自动重新持锁, 主机重枚举恢复。
 *   - Windows"安全弹出" (SCSI EJECT) → 组件状态切回 APP — tick 检测到
 *     mount_point 意外回 APP 时重新武装 USB (重插即恢复磁盘), 不退出模式。
 *   - MSC 期间 /cfg (LittleFS) 照常可写 — memory 继续落盘
 *
 * 用户数据安全: 运行期自愈只重挂/重建, 永不自动 f_mkfs (主机可能刚
 * 写过文件); 格式化只有两条路: ① 设置页"格式化存储" (NVS 标志 →
 * 重启 → 整区擦除 → boot 自动 f_mkfs); ② boot 时检测到全新空白分区
 * (全 0xFF) 自动建 FAT。README.txt 由本模块在挂载后维护。
 */
#include "usb_storage.h"
#include "usb_icon.h" /* pet.ico 字节数组 — tools/gen_icon.py 生成 (空=不设图标) */
#include "config_mgr.h"
#include "config_keys.h"
#include "sensor_logger.h"
#include "touch_fpc.h" /* : USB 事件 → 探针免疫窗 (供电瞬态假唤醒) */
#include "esp_pm.h"    /* : U盘模式禁轻睡锁 (USB 设备栈冻结掉盘) */
#include "tinyusb.h"
#include "tinyusb_msc.h"
#include "wear_levelling.h"
#include "diskio_wl.h"   /* ff_diskio_get_pdrv_wl / register — 盘号动态分配 */
#include "diskio_impl.h" /* ff_diskio_get_drive / unregister — 重挂修复 */
#include "diskio.h"      /* disk_read/disk_write/RES_OK — 卷修复直读写扇区 */
#include "ff.h"          /* f_mount/f_mkfs/f_setlabel/f_chmod — 挂载生命周期 */
#include "esp_vfs_fat.h" /* esp_vfs_fat_register_cfg — 开机一次性注册 */
#include "esp_private/usb_phy.h"
#include "esp_partition.h" /* 重启后格式化: data 分区整区擦除 */
#include "nvs.h"           /* 格式化请求标志 (重启后执行) */
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_heap_caps.h" /* enter 诊断: 启动设备栈前的堆状态打印 */
#include "esp_timer.h"
#include "freertos/FreeRTOS.h" /* vTaskDelay — 重启前留日志时间 */
#include "freertos/task.h"
#include <stdio.h>
#include <string.h> /* memcmp — README 比对 */
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

/* 重启后格式化 : 设置页确认 → 写 NVS → esp_restart → boot 时
 * 整区擦除 data 分区 → boot 挂载失败 → medium_blank → 自动 f_mkfs。
 * 运行时不碰 VFS 注册/f_mkfs — 运行期堆碎片化 NO_MEM 或悬垂 FatFs
 * 指针崩溃。 */
#define FMT_REQ_NS "device" /* 与 api_client 同命名空间 */
#define FMT_REQ_KEY "fmt_req"

static const char *TAG = "usb_storage";
static const char *DATA_MOUNT = "/data";

/* Windows 显示的磁盘名 (FAT 卷标 ≤11 字符, ASCII; 未设置时 Windows 默认显示"U盘") */
#define USB_VOL_LABEL "VirtualPet"

#define TINYUSB_TASK_STACK 8192 /* : 组件已改静态 .bss 栈 (tinyusb_task.c 补丁,            \
                                 * 运行期堆碎片化动态 8K 分配必败) — 此值必须与 \
                                 * tinyusb_task.c 的 TINYUSB_TASK_STATIC_STACK_SIZE 一致,        \
                                 * 不符 install 直接报错 */

static tinyusb_msc_storage_handle_t s_handle = NULL;
static bool s_active = false; /* U盘模式标志 (粘滞: 开关=手动进/退) */

/* 卷修复窗口标志 — 修复期间 /data 处于"解除挂载"态 (f_mount(NULL)),
 * 被 usb_storage_data_mounted() 透出, 让 life_log/diary 这类写者在
 * 窗口内自己退避 (同 U盘模式的退避机制, 不新增契约) */
static volatile bool s_repairing = false;

/* 卷不可用 (修复失败) — 单向置起的"停写"信号, 由 data_writer 的闸门消费。
 * 只有 usb_storage_volume_repair() 修好 (或本来就好) 才清 */
static volatile bool s_vol_bad = false;

/* : U盘模式禁轻睡锁 — 轻睡每 ~40ms 冻结 USB OTG 设备栈时钟 →
 * Windows 端枚举失效 "无法识别的设备" + 磁盘消失。锁跟随充电状态:
 * 插线 (充电/满电) acquire, 拔线 (放电) 30s 宽限后 release → 平时
 * 恢复轻睡, 开关常开不再是永久禁睡。锁创建一次保留。
 * 与 WiFi/USJ 锁共存无冲突 (多个 NO_LIGHT_SLEEP 锁 = 禁睡, 无叠加) */
static esp_pm_lock_handle_t s_usb_pm = NULL;

/* : 充电状态驱动禁睡 — set_charging (main 电池块) 按电流翻转动作。
 * s_charging=true = 当前视为插线 (锁计数 1, 由 enter 建立 / exit 归还);
 * 拔线翻转 → 起 30s 宽限, tick 到期释放 (计数归 0)。锁是引用计数 —
 * acquire/release 必须一一配对, 本状态机保证锁计数恒 0 或 1。
 * 跨任务竞态 (enter/exit 在 LVGL, set_charging/tick 在 main) 由
 * s_pm_mux spinlock 串行化 + s_pm_held 自跟踪兜底 (见下) */
static volatile bool s_charging = true;
static volatile uint32_t s_release_grace_ms = 0; /* 拔线宽限到期时刻 (0=无) */
#define USB_PLUG_GRACE_MS 30000                  /* 拔线宽限 — 短期重插不掉盘。 \
                                                  * 插线判据在 main.c (SOC≥100 兜底  \
                                                  * 满电停充, 电流 ≥-5mA 判非满电充电) */

/* esp_pm_lock 是引用计数且无查询 API — 自跟踪 s_pm_held, 所有
 * acquire/release 必须成对且只在 s_pm_held 翻转处发生, 保证锁计数
 * 恒 0 或 1。s_pm_mux 串行化 enter/exit (LVGL) 与 set_charging (main)
 * 与 tick (main) 三处状态转移 */
static portMUX_TYPE s_pm_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_pm_held = false;

static void usb_mode_lock_acquire(void)
{
    if (!s_usb_pm) {
        esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "usb", &s_usb_pm);
        if (!s_usb_pm) {
            ESP_LOGE(TAG, "U盘模式 PM 锁创建失败 — 息屏轻睡会冻结 USB 设备栈");
            return;
        }
    }
    esp_pm_lock_acquire(s_usb_pm);
}

static void usb_mode_lock_release(void)
{
    if (s_usb_pm) esp_pm_lock_release(s_usb_pm);
}

/* : 充电状态驱动禁睡 — main 任务电池块每秒调 set_charging
 * (判据: soc≥100 || ma≥-5): charging → 立即持锁; 拔线 → 起 30s 宽限,
 * 到期由 tick 释放。宽限防换电脑/换线短期重插掉盘; 重插 → 宽限作废
 * 立即重新持锁 */
void usb_storage_set_charging(bool charging)
{
    portENTER_CRITICAL(&s_pm_mux);
    if (!s_active) { /* 非 U盘模式: 锁归 enter/exit 管, 仅记录 */
        s_charging = charging;
        portEXIT_CRITICAL(&s_pm_mux);
        return;
    }
    if (charging) {
        s_release_grace_ms = 0; /* 重插: 宽限作废 */
        if (!s_pm_held) {       /* 拔线期已放锁 → 重新持锁 (放锁时
                                 * s_pm_held=false 必伴 s_charging=false) */
            usb_mode_lock_acquire();
            s_pm_held = true;
        }
        s_charging = true;
    } else {
        s_charging = false;
        s_release_grace_ms = (uint32_t)(esp_timer_get_time() / 1000) + USB_PLUG_GRACE_MS; /* 拔线 / 宽限中再拔: 刷新宽限 */
    }
    portEXIT_CRITICAL(&s_pm_mux);
}
static SemaphoreHandle_t s_fs_mutex = NULL; /* 串行化 探测 vs 挂载切换 —
                                             * 切换窗口 ("盘号已注册卷未挂") 曾被
                                             * 设置页探测抓到 → FRESULT=12 */

/* 开机注册的 FATFS 对象 — 起 fs 与 VFS ctx 注册一次后运行期
 * 永不释放, f_mount(NULL)+f_mount(fs) 重同步是纯 FatFS 操作 (零分配,
 * 不可能因堆失败), 悬垂/双注册/槽位耗尽从架构上不可能 */
static FATFS *s_fatfs_fs = NULL;

/* FatFS 卷可用性探测: 任一盘号 f_getfree 成功且有簇大小即有效 */
static bool data_fs_probe(void);

/* 轻量重挂 (分离 + 用开机注册的 fs 重挂, 零分配) — enter 回滚也用它 */
static FRESULT fatfs_remount(void);

/* 手动重建 /data 挂载 — 开机一次性注册 (boot 堆干净, 可靠) + 自愈兜底 */
static esp_err_t repair_data_mount(bool allow_format);

/* /data 预置说明文件 — USB 直读时告知目录用途与恢复方式。
 * 先读后比, 内容一致就不写 (开机 + 每次退出 U盘模式都会调本函数):
 * 覆写要擦数据扇区 + 目录项两个扇区, 而更坏的是 O_TRUNC **先截断再写** —
 * 卷已经坏了写不进去时, README 反而被自己抹掉。空文件 / 读失败才落笔 */
static void write_data_readme(void)
{
    char txt[512];
    const char *pet = config_get_str(CFG_KEY_PET_NAME, "萝莉丝");
    int n = snprintf(txt, sizeof(txt),
        "欢迎! 这是 Virtualpet 的存储盘, USB 连接电脑后可见。\n"
        "\n"
        "目录:\n"
        " diary/ — %s的日记 (HTML 文件, 双击用浏览器打开)\n"
        " life/ — 交互日志 (与%s的每一次对话和互动)\n"
        "\n"
        "提示:\n"
        " - 文件可自由删除/复制 (比如删掉某篇日记, 设备会自动重新同步)\n"
        " - 若电脑提示\"此磁盘未格式化\", 请在设备设置页执行\n"
        " \"格式化存储\", 然后重新插拔 USB\n",
        pet, pet);
    if (n <= 0 || n >= (int)sizeof(txt)) return;

    int fd = open("/data/README.txt", O_RDONLY);
    if (fd >= 0) {
        char old[512];
        ssize_t r = read(fd, old, sizeof(old));
        close(fd);
        if (r == (ssize_t)n && memcmp(old, txt, (size_t)n) == 0) return;
    }
    fd = open("/data/README.txt", O_CREAT | O_TRUNC | O_WRONLY);
    if (fd < 0) return;
    write(fd, txt, n);
    close(fd);
}

/* FAT 隐藏属性 — pet.ico/autorun.inf 物理上必须在盘根 (Windows 靠它们
 * 找图标), 但资源管理器默认不显示 (文件资源管理器默认隐藏这两个) */
static void set_hidden_attr(const char *path)
{
    FILINFO finfo;
    if (f_stat(path, &finfo) != FR_OK) return;
    /* 属性已对就不调 f_chmod — 它会重写目录项, 每次开机白送一次擦除 */
    if ((finfo.fattrib & (AM_HID | AM_SYS)) == (AM_HID | AM_SYS)) return;
    f_chmod(path, AM_HID | AM_SYS, AM_HID | AM_SYS);
}

/* /data 预置 USB 展示文件: 内置磁盘图标 + autorun.inf (仅 ICON= 指令,
 * 无任何自动运行内容)。只写缺失 (宿主可能删文件, exit 后兜底重建)。
 * 图标来源: tools/gen_icon.py 生成的 usb_icon.h (put tools/pet.ico 后
 * 构建自动重跑)。无内置图标 → 不写, Windows 用系统默认磁盘图标。
 * 旧固件残留文件不自动清理 — 盘上文件以"用户删了才消失"为准 */
static void write_usb_assets(void)
{
    struct stat st;
    if (usb_icon_ico_len == 0)
        return;
    if (stat("/data/pet.ico", &st) != 0) {
        int fd = open("/data/pet.ico", O_CREAT | O_TRUNC | O_WRONLY);
        if (fd >= 0) {
            write(fd, usb_icon_ico, usb_icon_ico_len);
            close(fd);
        }
    }
    if (stat("/data/autorun.inf", &st) != 0) {
        int fd = open("/data/autorun.inf", O_CREAT | O_TRUNC | O_WRONLY);
        if (fd >= 0) {
            static const char txt[] = "[autorun]\r\nICON=pet.ico\r\n";
            write(fd, txt, sizeof(txt) - 1);
            close(fd);
        }
    }
    set_hidden_attr("/data/pet.ico");
    set_hidden_attr("/data/autorun.inf");
}

/* 卷标比较必须忽略大小写: f_setlabel 存盘前过 ff_wtoupper (ff.c 里
 * `wc = ff_uni2oem(ff_wtoupper(dc), CODEPAGE)`), 根目录里躺着的是
 * "VIRTUALPET", f_getlabel 读回来原样返回不做小写还原 —— 直接 strcmp
 * 永远不等, 每次开机照写一次 BPB。FF_LFN_UNICODE 下的 CodePage 转换对
 * 纯 ASCII 是恒等, 手写 ASCII 折叠即可 */
static bool label_ci_equal(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char ca = (*a >= 'a' && *a <= 'z') ? (char)(*a - 32) : *a;
        char cb = (*b >= 'a' && *b <= 'z') ? (char)(*b - 32) : *b;
        if (ca != cb) return false;
    }
    return *a == 0 && *b == 0;
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
    /* 卷标已是目标值就不调 f_setlabel — 它无条件重写 BPB (引导扇区,
     * 卷上最要紧的一个扇区), 每次开机白送一次擦除。读失败 (无卷标 /
     * 卷异常) 回落原行为: 照写 */
    char dpath[3] = {(char)('0' + pdrv), ':', 0};
    char cur[24] = "";
    if (f_getlabel(dpath, cur, NULL) == FR_OK && label_ci_equal(cur, USB_VOL_LABEL))
        return;
    snprintf(label, sizeof(label), "%d:%s", (int)pdrv, USB_VOL_LABEL);
    FRESULT fr = f_setlabel(label);
    if (fr != FR_OK)
        ESP_LOGW(TAG, "f_setlabel(%s) 失败: %d", label, (int)fr);
    else
        ESP_LOGI(TAG, "磁盘卷标已设: %s", USB_VOL_LABEL);
}

/* ══ 卷损坏识别与修复 (FAT 修复阶梯 第 0 级) ═══════════════════════════
 *
 * 指纹: f_getfree 返回 FR_OK 却 **0 空闲簇**。掉电正好落在 FAT 扇区的
 * "已擦未写"窗口时, FAT12 整表成 0xFF → 每项都是 0xFFF(EOC) → FatFS
 * 认为"全部已分配", 体检把它读成"盘满", 真实含义是"盘坏了"。
 * 不能只看 0 空闲 (真满盘也是 0) → 再读 FAT 扇区看 FAT12 头还在不在:
 * 字节 0..2 = F8 FF FF (媒介字节 + 簇0=FF8、簇1=FFF 的 12bit 打包)。
 *
 * ⚠️ 修复**只写 FAT 扇区一个扇区, 绝不碰数据簇** —— 所以不可能毁掉
 * 文件内容, 最坏是映射猜错。这是整个方案的安全底线。
 * 反过来说也有硬上限: 目录项只存**起始簇**, 后续簇走向只存在于 FAT 里
 * (而 FAT 已经没了), 所以:
 *   - 未碎片化文件 → 完整恢复 (我们的写入多为 last_clst 顺序前推)
 *   - 顺序前推撞上别人起始簇的文件 → **截断** (读出来短)
 * 截断 > 静默读错 (读出来是别人的数据); chkdsk 在 FAT 全毁时同样如此。
 * 他人的**起始簇是事实**(目录项里写着)而延伸簇是猜测 → 事实全占先,
 * 猜测只允许落在没被事实占的簇上。
 *
 * 写入前把 BPB/FAT/根目录备份到 /cfg —— 唯一退路, 备份不成就别修。
 * 修完重挂重探当硬验收: 探测不过就当没修过, 交回调用方挂起。 */

#define VOL_META_DIR "/cfg/volmeta"
#define BPB_MEDIA_OFF 21 /* BPB_Media: 卷的媒介描述符 (FAT12/16 识别用) */
#define DE_ATTR 11
#define DE_CLST 26
#define DE_SIZE 28
#define DE_ATTR_VOL 0x08 /* AM_VOL 是 ff.c 的内部宏, ff.h 不导出 */
#define FAT12_EOC 0xFFF
#define VOL_ENT_MAX 256
#define VOL_DIR_MAX 8          /* 子目录扫描上限 (本项目 life/ + diary/ 两个) */
#define VOL_DIR_MAX_CLUSTERS 8 /* 子目录链推测上限 (本项目目录 1 簇装得下) */

/* 卸挂载前抄出来的几何 — f_mount(NULL) 会清 fs_type/pdrv, 其余字段不保证 */
typedef struct {
    UINT ssz, csize;
    DWORD ncl;   /* n_fatent: 合法簇号 2 .. ncl-1 */
    DWORD nroot; /* 根目录项数 (FAT12/16) */
    LBA_t volbase, fatbase, dirbase, database;
} vol_geom_t;

typedef struct {
    DWORD cl;
    DWORD size;
    BYTE attr;
    char dtag[12]; /* 所在目录 ("diary/", 根目录为 "") — 只进日志 */
    char name[16];
} vol_ent_t;

/* 一个子目录认领到的簇链 (根目录不占簇, 不进这个表) */
typedef struct {
    DWORD start;
    DWORD chain[VOL_DIR_MAX_CLUSTERS];
    int nchain;
    char path[24]; /* "diary/" 这样的前缀 — 只进日志 */
} vol_dir_t;

static WORD le16(const BYTE *p) { return (WORD)(p[0] | (p[1] << 8)); }
static DWORD le32(const BYTE *p)
{
    return (DWORD)p[0] | ((DWORD)p[1] << 8) | ((DWORD)p[2] << 16) | ((DWORD)p[3] << 24);
}

/* FAT12 项写入: 第 n 项落在字节 n + n/2, 奇偶项共享中间那个字节 */
static void fat12_set(BYTE *fat, DWORD n, WORD val)
{
    DWORD off = n + (n >> 1);
    if (n & 1) {
        fat[off] = (BYTE)((fat[off] & 0x0F) | ((val << 4) & 0xF0));
        fat[off + 1] = (BYTE)(val >> 4);
    } else {
        fat[off] = (BYTE)(val & 0xFF);
        fat[off + 1] = (BYTE)((fat[off + 1] & 0xF0) | ((val >> 8) & 0x0F));
    }
}

static LBA_t vol_cluster_sec(const vol_geom_t *g, DWORD n)
{
    return g->database + (LBA_t)(n - 2) * g->csize;
}

/* 备份 BPB/FAT/根目录到 /cfg (LittleFS) — 建在即将被覆盖的东西之外,
 * 是重建写坏时唯一的退路。scratch 由调用方给 (整个修复流程共用一块
 * 扇区缓冲, 内部 RAM 只占 4KB —— 运行期修复时内部堆很紧) */
static bool vol_backup_meta(const vol_geom_t *g, BYTE pdrv, BYTE *scratch)
{
    BYTE *buf = scratch;
    mkdir(VOL_META_DIR, 0777); /* 幂等; /cfg 已挂载 */

    const DWORD root_secs = (g->nroot * 32 + g->ssz - 1) / g->ssz;
    const struct {
        const char *path;
        LBA_t sec;
        DWORD n;
    } jobs[] = {
        {VOL_META_DIR "/bpb.bin", g->volbase, 1},
        {VOL_META_DIR "/fat.bin", g->fatbase, 1},
        {VOL_META_DIR "/root.bin", g->dirbase, root_secs},
    };
    bool ok = true;
    for (size_t j = 0; j < sizeof(jobs) / sizeof(jobs[0]) && ok; j++) {
        FILE *f = fopen(jobs[j].path, "wb");
        if (!f) {
            ESP_LOGE(TAG, "备份 %s 打不开 — 放弃修复", jobs[j].path);
            ok = false;
            break;
        }
        for (DWORD i = 0; i < jobs[j].n && ok; i++) {
            if (disk_read(pdrv, buf, jobs[j].sec + i, 1) != RES_OK ||
                fwrite(buf, 1, g->ssz, f) != g->ssz)
                ok = false;
        }
        fclose(f);
        if (ok) ESP_LOGI(TAG, "  备份 %s (%u 扇区)", jobs[j].path, (unsigned)jobs[j].n);
    }
    return ok;
}

/* 目录项名字 → 8.3 文本 (FatFS 把短名原样放在项里, LFN 在 0x0F 项, 这里只取短名) */
static void vol_ent_name(const BYTE *de, char *out, size_t n)
{
    char nm[9], ex[4];
    int a = 0, b = 0;
    for (int c = 0; c < 8 && de[c] != ' '; c++) nm[a++] = (char)de[c];
    nm[a] = 0;
    for (int c = 8; c < 11 && de[c] != ' '; c++) ex[b++] = (char)de[c];
    ex[b] = 0;
    if ((de[DE_ATTR] & AM_DIR) || b == 0) snprintf(out, n, "%.8s", nm);
    else snprintf(out, n, "%.8s.%.3s", nm, ex);
}

/* 扫一个子目录 (起始簇 start), 边走链边收集:
 *   - 文件项 → ents[] (起始簇同时记进 fact[])
 *   - 子目录 → dirs[] (待扫, 由调用方用同一个数组当下标递增的工作队列)
 *   - 自己的簇链 → out->chain[] (后面要照它写 FAT —— 只标 used 不写链的话,
 *     目录簇在新 FAT 里是 0 = 空闲, 会被后续分配直接覆盖掉)
 * 目录链前推: 某簇里出现 0x00 项 = 目录到此为止 (FatFS 删项写 0xE5,
 * 0x00 = 从未用过); 没出现就是链还长, 顺序前推到下一簇。
 * 撞上已被认领的簇 / 事实簇 / 越界 → 停, 只认前面一段 (截断, 同文件规则) */
static void vol_scan_dir(const vol_geom_t *g, BYTE pdrv, BYTE *scratch, DWORD start,
                         const char *dtag, bool *fact, bool *used, vol_ent_t *ents,
                         int *nent, int maxent, vol_dir_t *dirs, int *ndir, int maxdir,
                         vol_dir_t *out)
{
    if (out) out->nchain = 0;
    DWORD n = start;
    bool end = false;
    for (int k = 0; k < VOL_DIR_MAX_CLUSTERS && !end; k++) {
        if (n < 2 || n >= g->ncl) break;
        if (used[n]) break;              /* 已被认领 (交叉链接) → 不再猜 */
        if (k > 0 && fact[n]) break;     /* 延伸簇不许抢别的项的起始簇 */
        used[n] = true;
        if (out && out->nchain < VOL_DIR_MAX_CLUSTERS) out->chain[out->nchain++] = n;
        if (disk_read(pdrv, scratch, vol_cluster_sec(g, n), 1) != RES_OK) return;
        for (UINT i = 0; i < g->ssz / 32; i++) {
            const BYTE *de = scratch + (size_t)i * 32;
            BYTE a = de[DE_ATTR];
            if (de[0] == 0x00) { end = true; break; }
            /* '.' 开头的项是真目录项里的 "." / ".." — 不滤掉会被当成子目录,
             * 白吃 VOL_DIR_MAX 槽位 (FatFS 自己也这么滤, ff.c 里 b == '.') */
            if (de[0] == 0xE5 || de[0] == '.' || a == 0x0F || (a & DE_ATTR_VOL)) continue;
            DWORD cl = le16(de + DE_CLST);
            if (a & AM_DIR) {
                if (*ndir < maxdir) {
                    vol_dir_t *d = &dirs[(*ndir)++];
                    char nm[16];
                    vol_ent_name(de, nm, sizeof(nm));
                    d->start = cl;
                    d->nchain = 0;
                    snprintf(d->path, sizeof(d->path), "%.8s%.11s/", dtag, nm);
                } else {
                    ESP_LOGW(TAG, "  子目录数超 %d — 更深的目录不扫了 (其内容簇会被当空闲)",
                             maxdir);
                }
                if (cl >= 2 && cl < g->ncl) fact[cl] = true;
                continue;
            }
            if (*nent >= maxent) { end = true; break; }
            vol_ent_t *e = &ents[(*nent)++];
            e->cl = cl; /* FAT12/16 无高字 */
            e->size = le32(de + DE_SIZE);
            e->attr = a;
            snprintf(e->dtag, sizeof(e->dtag), "%.11s", dtag);
            vol_ent_name(de, e->name, sizeof(e->name));
            if (cl >= 2 && cl < g->ncl) fact[cl] = true; /* 起始簇是事实 */
        }
        if (end) break;
        n++;
    }
}

/* 重建 FAT 表并写回。返回是否**尝试并成功**写出。
 * scratch 由调用方给 —— 整个流程 (BPB 读 → 根目录扫 → 目录簇推测 → FAT
 * 构建) 是严格顺序的, 同一块 4KB 轮着用, 内部 RAM 占用恒为一块扇区 */
static bool vol_rebuild_fat(const vol_geom_t *g, BYTE pdrv, BYTE *scratch)
{
    const DWORD clbytes = (DWORD)g->csize * g->ssz;
    const DWORD root_secs = (g->nroot * 32 + g->ssz - 1) / g->ssz;

    /* FAT12 表可能跨扇区, 本实现只重建首扇区 —— 跨扇区时若只写首扇区,
     * 后续残留的 0xFF 仍是 EOC, 而重探会因为首扇区有 0 而**假通过**,
     * 于是报"修好了"却依旧坏。本项目 183 簇 = 275B 装得下一个 4KB,
     * 一旦不是这个几何就明说放弃, 不猜 */
    const DWORD fat_bytes = (g->ncl - 1) + ((g->ncl - 1) >> 1) + 2;
    if ((fat_bytes + g->ssz - 1) / g->ssz != 1) {
        ESP_LOGE(TAG, "FAT12 表需 %lu 字节 (跨扇区) — 本实现不处理, 不修",
                 (unsigned long)fat_bytes);
        return false;
    }

    vol_ent_t *ents = heap_caps_calloc(VOL_ENT_MAX, sizeof(vol_ent_t), MALLOC_CAP_SPIRAM);
    bool *fact = heap_caps_calloc(g->ncl, sizeof(bool), MALLOC_CAP_SPIRAM);
    bool *used = heap_caps_calloc(g->ncl, sizeof(bool), MALLOC_CAP_SPIRAM);
    vol_dir_t *dirs = heap_caps_calloc(VOL_DIR_MAX, sizeof(vol_dir_t), MALLOC_CAP_SPIRAM);
    bool ok = false;
    int nent = 0, nfile = 0, ndir = 0, ndir_ok = 0, ntrunc = 0, nbad = 0;

    if (!ents || !fact || !used || !dirs) {
        /* 分配失败 ≠ 卷坏 — 不能拿内存不足去判卷坏了 (会平白停写)。
         * PSRAM 十几 KB, 失败只可能是真枯竭, 下一轮再试 */
        ESP_LOGW(TAG, "修复所需 PSRAM 分配失败 — 本轮不判, 稍后重试");
        goto out;
    }

    /* 1. 媒介描述符只能取自 BPB — 坏 FAT 里那个已经没意义了 */
    if (disk_read(pdrv, scratch, g->volbase, 1) != RES_OK) goto out;
    BYTE media = scratch[BPB_MEDIA_OFF];
    if (media < 0xF0) {
        ESP_LOGE(TAG, "BPB 媒介字节 0x%02X 不像 FAT — 不猜, 放弃修复", media);
        goto out;
    }

    /* 2. 备份 (不成就别修) */
    if (!vol_backup_meta(g, pdrv, scratch)) goto out;

    /* 3. 扫根目录收集有效项; 0x00 空槽 = 根目录到此为止。
     * 根目录占固定扇区不占簇 → 不产生链 */
    {
        bool end = false;
        for (DWORD s = 0; s < root_secs && !end; s++) {
            if (disk_read(pdrv, scratch, g->dirbase + s, 1) != RES_OK) goto out;
            for (UINT i = 0; i < g->ssz / 32; i++) {
                const BYTE *de = scratch + (size_t)i * 32;
                BYTE a = de[DE_ATTR];
                if (de[0] == 0x00) { end = true; break; }
                if (de[0] == 0xE5 || de[0] == '.' || a == 0x0F || (a & DE_ATTR_VOL)) continue;
                if (nent >= VOL_ENT_MAX) { end = true; break; }
                DWORD cl = le16(de + DE_CLST); /* FAT12/16 无高字 */
                if (a & AM_DIR) {
                    if (ndir < VOL_DIR_MAX) {
                        vol_dir_t *d = &dirs[ndir++];
                        char nm[16];
                        vol_ent_name(de, nm, sizeof(nm));
                        d->start = cl;
                        d->nchain = 0;
                        snprintf(d->path, sizeof(d->path), "%.11s/", nm);
                    } else {
                        ESP_LOGW(TAG, "  子目录数超 %d — 更深的目录不扫了 "
                                      "(其内容簇会被当空闲)", VOL_DIR_MAX);
                    }
                    if (cl >= 2 && cl < g->ncl) fact[cl] = true;
                    continue;
                }
                vol_ent_t *e = &ents[nent++];
                e->cl = cl;
                e->size = le32(de + DE_SIZE);
                e->attr = a;
                e->dtag[0] = '\0';
                vol_ent_name(de, e->name, sizeof(e->name));
                if (cl >= 2 && cl < g->ncl) fact[cl] = true; /* 起始簇是事实 */
            }
        }
    }
    if (nent == 0 && ndir == 0) {
        ESP_LOGE(TAG, "根目录一个有效项都没有 — 目录也坏了, 不猜");
        goto out;
    }

    /* 4. 子目录逐个扫 (dirs[] 同时是工作队列: 扫 dirs[i] 时往里追加更深一层)。
     * 必须先把所有项收齐再排链 —— 延伸簇一律不许抢**任何**项的起始簇,
     * 而"任何"要等全扫完才知道。取名字多带一级目录, 日志里才分得清哪个文件 */
    for (int i = 0; i < ndir; i++) {
        if (dirs[i].start < 2 || dirs[i].start >= g->ncl) continue;
        vol_scan_dir(g, pdrv, scratch, dirs[i].start, dirs[i].path, fact, used, ents, &nent,
                     VOL_ENT_MAX, dirs, &ndir, VOL_DIR_MAX, &dirs[i]);
    }

    /* 5. 建全新 FAT: 簇0 = 媒介|0xF00, 簇1 = EOC, 其余先当空闲。
     * 目录链推导已结束 → scratch 腾出来当 FAT 缓冲 (同一块扇区轮用) */
    BYTE *fat = scratch;
    memset(fat, 0, g->ssz);
    fat12_set(fat, 0, (WORD)(0xF00 | media));
    fat12_set(fat, 1, FAT12_EOC);

    /* 5a. 目录链先写 —— 不写的话目录簇在 FAT 里是 0(空闲),
     * 后续分配会直接盖掉目录项本身 (曾经漏过, 见"只标 used 不写链"的教训) */
    for (int i = 0; i < ndir; i++) {
        const vol_dir_t *d = &dirs[i];
        for (int k = 0; k < d->nchain; k++)
            fat12_set(fat, d->chain[k],
                      (k + 1 == d->nchain) ? FAT12_EOC : (WORD)(d->chain[k] + 1));
        if (d->nchain) ndir_ok++;
    }

    /* 5b. 文件链 */
    for (int i = 0; i < nent; i++) {
        const vol_ent_t *e = &ents[i];
        if (e->attr & AM_DIR) continue;
        if (e->cl < 2 || e->cl >= g->ncl) {
            if (e->size) {
                ESP_LOGW(TAG, "  %s%s 起始簇 %lu 越界 — 无法恢复", e->dtag, e->name,
                         (unsigned long)e->cl);
                nbad++;
            }
            continue;
        }
        DWORD need = (DWORD)((e->size + clbytes - 1) / clbytes);
        if (need == 0) need = 1; /* 空文件也认一个簇 (目录项里就写着它) */
        DWORD got = 1;
        for (DWORD k = 1; k < need; k++) {
            DWORD cur = e->cl + k;
            if (cur >= g->ncl || used[cur] || fact[cur]) break; /* 事实簇不许抢 */
            used[cur] = true;
            got++;
        }
        for (DWORD k = 0; k < got; k++)
            fat12_set(fat, e->cl + k, (k + 1 == got) ? FAT12_EOC : (WORD)(e->cl + k + 1));
        used[e->cl] = true;
        nfile++;
        if (got < need) {
            ESP_LOGW(TAG, "  %s%s 需 %lu 簇只得 %lu — 截断 (原链已随 FAT 丢失, 只能顺序猜)",
                     e->dtag, e->name, (unsigned long)need, (unsigned long)got);
            ntrunc++;
        }
    }

    /* 6. 回写 FAT 扇区 */
    if (disk_write(pdrv, fat, g->fatbase, 1) != RES_OK) {
        ESP_LOGE(TAG, "FAT 扇区回写失败");
        goto out;
    }
    {
        DWORD free_cl = 0;
        for (DWORD n = 2; n < g->ncl; n++)
            if (!used[n]) free_cl++;
        ESP_LOGI(TAG, "FAT 重建完成: %d 文件 (%d 截断 / %d 不可恢复), %d/%d 子目录已挂链, "
                      "空闲 %lu 簇",
                 nfile, ntrunc, nbad, ndir_ok, ndir, (unsigned long)free_cl);
    }
    ok = true;

out:
    heap_caps_free(ents);
    heap_caps_free(fact);
    heap_caps_free(used);
    return ok;
}

/* 卷健康检查 + 必要时修复。返回修复后是否可用。
 * 幂等, 可在开机挂载后与运行期写失败累积时调用。
 * 健康 → 免修直接返回 true; 指纹命中 → 备份 + 重建 + 重挂重探 (硬验收) */
bool usb_storage_volume_repair(void)
{
    /* s_vol_bad 只在**判定明确**时改: 确认健康 → 清; 确认坏且修不好 → 置。
     * "判不出来"(探测失败 / 内存不够 / 拿不到锁) 一律不动它 —— 内存不足
     * 平白停写是比漏判更糟的失败模式 (停写后连诊断都没了) */
    if (!s_fatfs_fs) return false;
    wl_handle_t wl = sensor_logger_get_wl_handle();
    if (wl == WL_INVALID_HANDLE) return false;
    BYTE pdrv = ff_diskio_get_pdrv_wl(wl);
    if (pdrv == 0xff) return false;
    char dpath[3] = {(char)('0' + pdrv), ':', 0};

    /* 1. 探: 有空间就是好的 (本项目分区 724KB, 正常余量 500KB+) */
    DWORD fre = 0;
    FATFS *fs = NULL;
    if (f_getfree(dpath, &fre, &fs) != FR_OK || !fs) {
        ESP_LOGW(TAG, "卷探测失败 (f_getfree) — 无法判定, 本轮不判不修");
        return false;
    }
    if (fre > 0) {
        s_vol_bad = false;
        return true;
    }

    /* 2. 指纹确认: FAT12 头还在 → 真满盘, 不是坏卷 */
    vol_geom_t g = {
        .ssz = fs->ssize,
        .csize = fs->csize,
        .ncl = fs->n_fatent,
        .nroot = fs->n_rootdir,
        .volbase = fs->volbase,
        .fatbase = fs->fatbase,
        .dirbase = fs->dirbase,
        .database = fs->database,
    };
    BYTE *scratch = heap_caps_malloc(g.ssz, MALLOC_CAP_INTERNAL);
    if (!scratch) {
        ESP_LOGW(TAG, "修复缓冲分配失败 (4KB 内部 RAM) — 本轮不判不修");
        return false;
    }
    bool head_ok = disk_read(pdrv, scratch, g.fatbase, 1) == RES_OK &&
                   scratch[0] == 0xF8 && scratch[1] == 0xFF && scratch[2] == 0xFF;
    if (head_ok) {
        heap_caps_free(scratch);
        ESP_LOGW(TAG, "0 空闲簇但 FAT12 头完好 — 判为真满盘, 不做修复");
        s_vol_bad = false;
        return true;
    }

    ESP_LOGE(TAG, "卷损坏指纹命中: f_getfree 成功却 0 空闲簇 + FAT12 头丢失 "
                  "(%u 簇全成 EOC) — 尝试重建 FAT",
             (unsigned)g.ncl);

    /* 3. 修复窗口: 必须脱离挂载态 —— FatFS 的 win[] 可能持有脏扇区,
     * 直接 disk_write 会被它回写覆盖。同时把 data_mounted 置否,
     * 让 life_log/diary 这类写者在窗口内自己退避 */
    if (!s_fs_mutex ||
        xSemaphoreTake(s_fs_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        heap_caps_free(scratch);
        ESP_LOGE(TAG, "拿不到 FS 锁 — 本轮不判不修");
        return false;
    }
    s_repairing = true;
    f_mount(NULL, dpath, 1); /* 丢窗口, 防脏扇区反盖重建结果 */
    bool rebuilt = vol_rebuild_fat(&g, pdrv, scratch);
    FRESULT fr = f_mount(s_fatfs_fs, dpath, 1); /* 重挂: 从磁盘重读 FAT */
    s_repairing = false;
    xSemaphoreGive(s_fs_mutex);
    heap_caps_free(scratch);

    if (!rebuilt || fr != FR_OK) {
        s_vol_bad = true; /* 指纹已确认 + 修不好 = 明确的坏 */
        ESP_LOGE(TAG, "卷修复失败 (重建%s, 重挂 %d) — /data 停止写入, "
                      "原始元数据已留在 %s",
                 rebuilt ? "成功" : "失败", (int)fr, VOL_META_DIR);
        return false;
    }

    /* 4. 硬验收: 重探不过就当没修过 */
    DWORD fre2 = 0;
    FATFS *fs2 = NULL;
    if (f_getfree(dpath, &fre2, &fs2) != FR_OK || !fs2 || fre2 == 0) {
        s_vol_bad = true;
        ESP_LOGE(TAG, "修复后重探仍不正常 (空闲 %lu 簇) — /data 停止写入",
                 (unsigned long)fre2);
        return false;
    }
    ESP_LOGW(TAG, "卷修复完成: 空闲 %lu 簇 — /data 恢复写入 (原始元数据备份在 %s)",
             (unsigned long)fre2, VOL_META_DIR);
    s_vol_bad = false;
    return true;
}

bool usb_storage_volume_bad(void) { return s_vol_bad; }

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
        .target = USB_PHY_TARGET_INT,
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
    (void)h;
    (void)arg;
    /* : 组件不再管理 FatFS, FORMAT_REQUIRED 等事件不会再来 — 仅留日志 */
    ESP_LOGI(TAG, "MSC 事件 %d (mount=%d)", (int)ev->id, (int)ev->mount_point);
}

/* 全新分区判定: 前 4 扇区全 0xFF = 擦过的 flash / 新设备, 无数据可保护。
 * 只有这种情形 boot 才自动 f_mkfs — 有数据残留绝不自动销毁。
 * 缓冲 512B 分块读: 本路径在 main 任务 6144B 栈上下文, 栈占用必须小 */
static bool medium_blank(void)
{
    wl_handle_t wl = sensor_logger_get_wl_handle();
    if (wl == WL_INVALID_HANDLE) return false;
    uint8_t buf[512];
    for (int s = 0; s < 4; s++) {
        for (int c = 0; c < 4096 / (int)sizeof(buf); c++) {
            if (wl_read(wl, (size_t)s * 4096 + (size_t)c * sizeof(buf),
                        buf, sizeof(buf)) != ESP_OK) return false;
            for (size_t i = 0; i < sizeof(buf); i++)
                if (buf[i] != 0xFF) return false;
        }
    }
    return true;
}

esp_err_t usb_storage_init(void)
{
    if (s_handle) return ESP_OK;

    /* : boot 自愈 PHY — 上次会话若 U盘模式启动中途失败, D+/D- 可能卡在
     * OTG (RTCCNTL.usb_conf.sw_usb_phy_sel, RTC 域, esp_restart 不清,
     * 只有断电才复位) → COM 口永久消失。这里无条件把 mux 切回
     * USB-Serial-JTAG: 正常 boot (mux 已在 USJ) 时是无害往返 (句柄表
     * 已清零, 不报 IN_USE); 卡在 OTG 时则真正切回 — 软件重启即可恢复 */
    esp_err_t phy_err = usb_phy_restore_serial_jtag();
    if (phy_err != ESP_OK)
        ESP_LOGW(TAG, "boot PHY 自愈失败: %s — COM 口可能需断电恢复",
                 esp_err_to_name(phy_err));

    /* 诊断: boot 内存地图 — 验证 PSRAM 化 + ALWAYSINTERNAL 后的释放效果 */
    ESP_LOGI(TAG, "内存地图: 内部堆 free=%u max=%u | PSRAM free=%u max=%u",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    /* 重启后格式化: 必须赶在 FatFS 注册之前擦除 — 卷上残留有效 FAT 时
     * 挂载会成功, "格式化"就白做了。擦完后 boot 挂载报 FR_NO_FILESYSTEM
     * → medium_blank=true → 自动 f_mkfs 重建全新分区 */
    nvs_handle_t nvs;
    esp_err_t nvs_err = nvs_open(FMT_REQ_NS, NVS_READWRITE, &nvs);
    if (nvs_err == ESP_OK) {
        uint8_t fmt_req = 0;
        esp_err_t gerr = nvs_get_u8(nvs, FMT_REQ_KEY, &fmt_req);
        if (gerr == ESP_OK && fmt_req) {
            ESP_LOGW(TAG, "检测到格式化请求 — 擦除 data 分区");
            nvs_erase_key(nvs, FMT_REQ_KEY); /* 先清标志 — 擦除/重启失败不再重试 */
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
            /* 诊断: 区分 无标志 / 键不存在 / NVS 异常 — 曾遇
             * 格式化请求后重启仍显示旧 FAT (724K), 需确认标志链 */
            ESP_LOGD(TAG, "格式化标志: %s",
                     (gerr == ESP_OK) ? (fmt_req ? "有(但非1?!)" : "无")
                                      : ((gerr == ESP_ERR_NVS_NOT_FOUND)
                                             ? "键不存在"
                                             : "读取失败"));
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
    tinyusb_msc_driver_config_t drv_cfg = {.callback = msc_evt_cb};
    esp_err_t err = tinyusb_msc_install_driver(&drv_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MSC driver 安装失败: %s", esp_err_to_name(err));
        return err;
    }

    /* 存储实例: mount_point=USB — 起组件绝不尝试挂载 FatFS
     * (mount/unmount 已 patch 成纯状态切换), FatFS 由下面我们自己注册 */
    tinyusb_msc_storage_config_t cfg = {
        .medium = {.wl_handle = wl},
        .fat_fs = {
            .base_path = (char *)DATA_MOUNT,
            .config = {
                .format_if_mount_failed = false, /* 与 do_not_format 语义一致 */
                .max_files = 4,                  /* 设备同时打开 /data 文件 ≤ 2 (日记同步+life_log), 4 富余 */
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

    /* 兜底探测: 挂载报成功但卷不可用 → 从零重建 (自愈, 起) */
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
    /* 卷损坏识别 + 修复 (第 0 级)。开机是主场景 —— 掉电正好落在 FAT 扇区
     * 的"已擦未写"窗口 → 下次开机就是这里。健康则免修立即返回。
     * 必须在开机文件 (README/图标/卷标) 之前: 卷坏了就别再往上戳 */
    if (!usb_storage_volume_repair())
        ESP_LOGE(TAG, "/data 卷不可用 — 写入闸门关闭, 直到修复成功或格式化");

    if (usb_storage_volume_bad()) {
        /* 卷不可用 → 不写任何开机文件 (写也是白写, 还可能覆盖残存数据) */
    } else if (data_fs_probe()) {
        write_data_readme();
        write_usb_assets();
        ensure_volume_label();
    }

    /* 挂载健康检查: f_getfree 扫 FAT 表 — 卷损坏时 FRESULT≠OK;
     * 可用空间膨胀 = FAT 表与目录不一致 (孤儿簇), 此日志直接暴露 */
    {
        FRESULT fr;
        FATFS *fs = NULL;
        DWORD fre_clu = 0;
        char dpath[3] = {(char)('0' + usb_storage_get_drive()), ':', 0};
        fr = f_getfree(dpath, &fre_clu, &fs);
        if (fr == FR_OK && fs) {
            /* 卷字节 = 簇数 × csize × 扇区字节 (ssize, 本项目 4096) */
            DWORD total_kb = (DWORD)(fs->n_fatent - 2) * fs->csize * fs->ssize / 1024;
            DWORD free_kb = fre_clu * fs->csize * fs->ssize / 1024;
            ESP_LOGI(TAG, "卷健康: 总 %lu KB, 可用 %lu KB (%u%% 已用)",
                     (unsigned long)total_kb, (unsigned long)free_kb,
                     (unsigned)(100 - (total_kb ? free_kb * 100 / total_kb : 0)));
        } else {
            ESP_LOGW(TAG, "卷健康检查失败: f_getfree FRESULT=%d — FAT 表异常, "
                          "写盘可能静默失败, 建议设置页格式化存储",
                     (int)fr);
        }
    }

    ESP_LOGI(TAG, "USB 存储就绪 — /data 已挂载 (U盘模式: 设置页开启, 盘号 %d)",
             usb_storage_get_drive());
    return ESP_OK;
}

bool usb_storage_is_active(void) { return s_active; }

/* : FatFS 挂载生命周期归本模块 — 非 U盘模式即挂载态 (写者闸门) */
bool usb_storage_data_mounted(void)
{
    return s_handle != NULL && !s_active && !s_repairing;
}

esp_err_t usb_storage_enter(void)
{
    if (s_active) return ESP_OK;
    if (!s_handle) return ESP_ERR_INVALID_STATE;

    /* 1) 分离 FatFS — 介质交还 USB 主机独占。f_mount(NULL) 只清 FatFs[vol],
     * fs 对象与 VFS ctx 保持注册 (零分配, 不可能因堆失败) */
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
            .size = TINYUSB_TASK_STACK,
            .priority = 5,
            .xCoreID = 1,
        },
    };
    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        /* : install 内部 usb_new_phy(OTG) 可能已把 D+/D- 切给 USB
         * Wrap — 组件失败路径只释放句柄, 不切回 mux → 必须显式 restore,
         * 否则 COM 口永久消失 (RTC 域, 断电才复位)。错误日志必须在
         * restore 之后发 — install 失败时 PHY 已切 OTG (console 已死),
         * 先打日志全丢 */
        esp_err_t perr = usb_phy_restore_serial_jtag();
        if (perr != ESP_OK)
            ESP_LOGE(TAG, "PHY 回滚也失败: %s — COM 口需断电恢复", esp_err_to_name(perr));
        ESP_LOGE(TAG, "tinyusb 设备栈启动失败: %s — 已切回 Serial-JTAG, 恢复 /data 挂载 (内部堆 free=%u largest=%u)",
                 esp_err_to_name(err),
                 heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
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
        tinyusb_driver_uninstall();    /* 回滚: 释放 OTG */
        usb_phy_restore_serial_jtag(); /* 并把 PHY 切回串口口 */
        if (xSemaphoreTake(s_fs_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            fatfs_remount();
            xSemaphoreGive(s_fs_mutex);
        }
        return err;
    }
    s_active = true;
    /* : 禁轻睡 (USB 设备栈需连续时钟) + 探针免疫窗 (PHY/供电瞬态)。
     * 禁睡跟随充电状态 — enter 无条件持锁 (计数 1), 后续由
     * set_charging (main 电池块) 按电流翻转: 拔线 30s 宽限后 tick 释放,
     * 重插立即重新持锁 */
    portENTER_CRITICAL(&s_pm_mux);
    s_charging = true;
    s_release_grace_ms = 0;
    if (!s_pm_held) {
        usb_mode_lock_acquire();
        s_pm_held = true;
    }
    portEXIT_CRITICAL(&s_pm_mux);
    touch_fpc_note_usb_event();
    ESP_LOGI(TAG, "U盘模式开启 — 插上 USB 线访问磁盘; "
                  "设置页开关手动退出 (重启也回正常模式); "
                  "插线 (充电/满电) 禁睡, 拔线 30s 宽限后恢复轻睡");
    return ESP_OK;
}

esp_err_t usb_storage_exit(void)
{
    if (!s_active) return ESP_OK;
    if (!s_handle) {
        s_active = false;
        return ESP_ERR_INVALID_STATE;
    }
    s_active = false; /* 先清标志 — 退出期间 tick (2s 节拍, 另一任务) 检测到
                       * "已回 APP" 会再次进入本函数, 防并发重入 */

    /* : 先放禁睡锁再切 PHY — 卸载/切换期间的供电瞬态探针不判唤醒。
     * s_pm_mux 串行化 + s_pm_held 保证 release 恰一次 — 即使宽限期
     * 已释放过, exit 也不会重复 release */
    portENTER_CRITICAL(&s_pm_mux);
    if (s_pm_held) {
        s_pm_held = false;
        usb_mode_lock_release();
    }
    s_charging = true; /* 复位 — 下次 enter 自建计数 */
    s_release_grace_ms = 0;
    portEXIT_CRITICAL(&s_pm_mux);
    touch_fpc_note_usb_event();

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
     * 零分配, 不可能因堆失败 */
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
                      "请到设置页执行\"格式化存储\"",
                 (int)fr);
    }

    write_data_readme(); /* 主机可能删过引导文件 → 恢复 (重挂失败时静默跳过) */
    write_usb_assets();
    return ESP_OK;
}

/* FatFS 卷可用性探测: 只探 WL 注册盘号 — IDF ff_disk_initialize 对未注册
 * 槽无守卫 (diskio.c:88 直接解引用), 注销盘号后全盘扫描必崩 */
static bool data_fs_probe(void)
{
    wl_handle_t wl = sensor_logger_get_wl_handle();
    if (wl == WL_INVALID_HANDLE) return false;
    BYTE pdrv = ff_diskio_get_pdrv_wl(wl);
    if (pdrv == 0xff) return false; /* 盘号未注册 → 无卷可探 */
    char dpath[12];
    snprintf(dpath, sizeof(dpath), "%d:", pdrv);
    DWORD fre_clu = 0;
    FATFS *fs = NULL;
    if (f_getfree(dpath, &fre_clu, &fs) != FR_OK || !fs || !fs->csize)
        return false;
    return true;
}

/* 轻量重挂 : 分离 + 用开机注册的 fs 重挂 — 纯 FatFS 操作,
 * 零分配。fs 与 VFS ctx 运行期从不释放, 故不可能悬垂/双注册/槽位耗尽。
 * 调用方必须已持有 s_fs_mutex */
static FRESULT fatfs_remount(void)
{
    wl_handle_t wl = sensor_logger_get_wl_handle();
    if (wl == WL_INVALID_HANDLE || !s_fatfs_fs) return FR_INT_ERR;
    BYTE pdrv = ff_diskio_get_pdrv_wl(wl);
    if (pdrv == 0xff) return FR_INT_ERR;
    char drv[3] = {(char)('0' + pdrv), ':', 0};
    f_mount(NULL, drv, 1);              /* 空槽时为无害 no-op */
    return f_mount(s_fatfs_fs, drv, 1); /* 重挂 — 从磁盘重读 FAT */
}

/* 从零重建 /data 挂载 (开机一次性注册 + 运行期自愈兜底)。
 * 全量清理: 分离所有卷 → 清 VFS ctx → 清全部盘号注册 → 注册 WL 盘号 →
 * VFS ctx → f_mount, 每步检查错误, 失败回滚不留残留。系统只有唯一 WL
 * 卷, 全量清理不会误伤其他卷。allow_format=true 时 (仅开机调用):
 * f_mount 失败且 medium_blank → f_mkfs 建全新 FAT (FM_ANY 自动选类型) */
static esp_err_t repair_data_mount_locked(bool allow_format)
{
    wl_handle_t wl = sensor_logger_get_wl_handle();
    if (wl == WL_INVALID_HANDLE) return ESP_ERR_INVALID_STATE;

    /* 先分离所有卷再释放 ctx — f_mount 挂载失败后 FatFs[drv] 仍指向 fs
     * (ff.c 先注册后挂载, 失败无回滚), 若直接 unregister_path 释放 ctx,
     * FatFs[drv] 变悬垂, 此后任意 f_getfree/f_mount 走进已释放内存 →
     * LoadProhibited 崩溃。fs 还活着时 f_mount(NULL) 分离是唯一安全清理 */
    for (BYTE drv = 0; drv < FF_VOLUMES; drv++) {
        char d[3] = {(char)('0' + drv), ':', 0};
        f_mount(NULL, d, 1);
    }
    esp_vfs_fat_unregister_path(DATA_MOUNT); /* 无 ctx 时返回 NOT_FOUND, 无害 */
    for (BYTE drv = 0; drv < FF_VOLUMES; drv++)
        ff_diskio_unregister(drv); /* 无注册的槽为无害 no-op */
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
        MKFS_PARM mkfs = {.fmt = FM_ANY, .au_size = 0}; /* 自动选 FAT 类型/簇 (与出厂路径一致) */
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
    s_fatfs_fs = fs; /* : 此后运行期只重挂, 不再释放 */
    /* 诊断: 卷几何 — 曾见格式化重建后仍显示 724K (512B 几何),
     * 需确认 f_mkfs 建出的是 4Kn (ssize=4096) 还是 512B 卷 */
    ESP_LOGI(TAG, "卷几何: ssize=%u csize=%u n_fatent=%u fat=%s",
             (unsigned)fs->ssize, (unsigned)fs->csize, (unsigned)fs->n_fatent,
             fs->fs_type == FS_FAT12 ? "FAT12" : fs->fs_type == FS_FAT16 ? "FAT16"
                                             : fs->fs_type == FS_FAT32   ? "FAT32"
                                                                         : "exFAT");
    return ESP_OK;
}

/* 手动重建 /data 挂载 — 持锁执行, 探测 (LVGL 任务) 在此期间阻塞等待,
 * 不会看到"盘号已注册卷未挂"的中间态 (FRESULT=12) */
static esp_err_t repair_data_mount(bool allow_format)
{
    if (!s_fs_mutex) return ESP_ERR_INVALID_STATE;
    esp_err_t ret;
    if (xSemaphoreTake(s_fs_mutex, pdMS_TO_TICKS(2000)) != pdTRUE)
        return ESP_ERR_TIMEOUT; /* 探测卡死不应阻塞重建 */
    ret = repair_data_mount_locked(allow_format);
    xSemaphoreGive(s_fs_mutex);
    return ret;
}

/* 设置页数据分区探测 — 与 repair/挂载切换互斥。f_getfree 在切换窗口
 * 会看到 FatFs[vol]==NULL → FRESULT=12 (FR_NOT_ENABLED), 曾误报"-" */
bool usb_storage_probe_data(uint32_t *total_kb, uint32_t *free_kb)
{
    if (total_kb) *total_kb = 0;
    if (free_kb) *free_kb = 0;
    if (!s_fs_mutex) return false;
    wl_handle_t wl = sensor_logger_get_wl_handle();
    if (wl == WL_INVALID_HANDLE) return false;

    bool ok = false;
    if (xSemaphoreTake(s_fs_mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
        return false;                      /* 挂载切换/重建进行中 — 探测让路 */
    BYTE pdrv = ff_diskio_get_pdrv_wl(wl); /* 只探 WL 注册盘号 */
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
            /* 诊断: 区分 盘号未注册 / 卷未挂 (FRESULT=12) / 卷数据异常 */
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

/* 非 U盘模式下的卷自愈: 探测失败 → 轻量重挂 (零分配) → 全重建
 * (无泄漏源) → 60s 降频重试。持续失败 = FAT 被主机改动或损坏,
 * 走设置页"格式化存储" */
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
                          "持续失败请设置页\"格式化存储\"",
                     esp_err_to_name(rerr));
        }
        return;
    }

    tinyusb_msc_mount_point_t mp;
    if (tinyusb_msc_get_storage_mount_point(s_handle, &mp) != ESP_OK) return;

    /* : 拔线宽限到期 → 释放禁睡锁, 恢复轻睡。宽限防换电脑/换线短期
     * 重插掉盘; 重插 → set_charging(true) 立即重新持锁, 宽限作废。
     * 睡着时重插 (设备未醒) 会掉盘 — 触摸唤醒后充电检测自动重新持锁,
     * 主机重枚举恢复 */
    bool released = false;
    uint32_t now_ms2 = (uint32_t)(esp_timer_get_time() / 1000);
    portENTER_CRITICAL(&s_pm_mux);
    if (!s_charging && s_pm_held && s_release_grace_ms &&
        (int32_t)(now_ms2 - s_release_grace_ms) >= 0) {
        s_release_grace_ms = 0;
        s_pm_held = false;
        usb_mode_lock_release();
        released = true;
    }
    portEXIT_CRITICAL(&s_pm_mux);
    if (released)
        ESP_LOGI(TAG, "拔线超过 %d s 宽限 — 释放 usb 禁睡锁, 恢复轻睡 "
                      "(重插线自动重新禁睡)",
                 USB_PLUG_GRACE_MS / 1000);

    /* Windows"安全弹出" (SCSI EJECT): 组件已把挂载权自动回 APP — 不
     * 退出模式, 重新武装 USB 挂载权, 重插即恢复磁盘 (真 U盘行为) */
    if (mp == TINYUSB_MSC_STORAGE_MOUNT_APP) {
        ESP_LOGI(TAG, "主机已弹出磁盘 — 保持 U盘模式, 重插自动恢复");
        tinyusb_msc_set_storage_mount_point(s_handle, TINYUSB_MSC_STORAGE_MOUNT_USB);
    }
}

/* 设置页"格式化存储"确认 → 请求重启后格式化。运行时不碰 FatFS —
 * 运行数小时后内部堆仅剩 ~10KB, VFS ctx 注册/f_mkfs 会 NO_MEM 失败或
 * 留悬垂指针崩设置页。改为: 写 NVS 标志 → esp_restart → boot 整区
 * 擦除 → 自动建 FAT (出厂流程) */
esp_err_t usb_storage_request_format(void)
{
    if (s_active) return ESP_ERR_INVALID_STATE; /* USB 主机正持有, 不可格式化 */

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(FMT_REQ_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "格式化请求: NVS 打开失败 (%s)", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_u8(nvs, FMT_REQ_KEY, 1);
    if (err == ESP_OK) err = nvs_commit(nvs); /* 必须落盘 — esp_restart 不等 NVS 后台写 */
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "格式化请求: NVS 写标志失败 (%s)", esp_err_to_name(err));
        return err;
    }
    ESP_LOGW(TAG, "格式化请求已记录 — 3 秒后重启并擦除 data 分区");
    vTaskDelay(pdMS_TO_TICKS(3000)); /* 日志可见后再重启 */
    esp_restart();
    return ESP_OK; /* 不返回 (esp_restart 不返回) */
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
