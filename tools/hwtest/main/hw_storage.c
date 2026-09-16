/**
 * @file hw_storage.c
 * @brief H 段 —— NVS 统计, /cfg(LittleFS 512KB), /data(FAT 1MB), /assets(LittleFS 只读)
 *
 * 只验"能不能挂载 + 能不能写读删 + 容量对不对", 不动 assets 里的任何数据。
 */
#include "hw_storage.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hw_expect.h"
#include "hw_report.h"
#include "nvs_flash.h"

static const char *TAG = "hwtest";

/* 分区前 4KB 是否全 0xFF (全新未格式化) —— 只有这种才允许格式化 */
static bool partition_blank(const esp_partition_t *p)
{
    if (!p) return false;
    uint8_t buf[256];
    size_t off = 0;
    size_t limit = p->size < 4096 ? p->size : 4096;
    while (off < limit) {
        size_t n = limit - off;
        if (n > sizeof(buf)) n = sizeof(buf);
        if (esp_partition_read(p, off, buf, n) != ESP_OK) return false;
        for (size_t i = 0; i < n; i++)
            if (buf[i] != 0xFF) return false;
        off += n;
    }
    return true;
}

/* 512 字节图案写-读-删往返 */
static bool rw_roundtrip(const char *path, char *err, size_t err_len)
{
    const int N = 512;
    uint8_t w[N], r[N];
    for (int i = 0; i < N; i++) w[i] = (uint8_t)(i * 7 + 3);

    errno = 0; /* 必须清零: 失败路径可能不设 errno, 不清就会把上一笔的陈值报出来 */
    FILE *f = fopen(path, "wb");
    if (!f) {
        int e0 = errno;
        snprintf(err, err_len, "写打不开 errno=%d", e0);
        return false;
    }
    size_t wn = fwrite(w, 1, N, f);
    int we = errno;
    fclose(f);
    if (wn != N) {
        snprintf(err, err_len, "写 %u/%d 字节 errno=%d", (unsigned)wn, N, we);
        remove(path);
        return false;
    }

    errno = 0;
    f = fopen(path, "rb");
    if (!f) {
        snprintf(err, err_len, "读打不开 errno=%d", errno);
        remove(path);
        return false;
    }
    errno = 0;
    size_t rn = fread(r, 1, N, f);
    int re = errno;
    fclose(f);
    bool same = (rn == N) && (memcmp(w, r, N) == 0);
    int del = remove(path);
    if (!same) {
        snprintf(err, err_len, "读回 %u 字节%s errno=%d 删=%d", (unsigned)rn,
                 rn == N ? "但内容不符" : "", re, del);
        return false;
    }
    return true;
}

/* ── LittleFS 挂载 (默认不格式化; 全空白才允许) ── */
static esp_err_t lfs_mount(const char *label, const char *base, bool read_only, bool *formatted,
                           char *err, size_t err_len)
{
    esp_vfs_littlefs_conf_t cfg = {
        .base_path = base,
        .partition_label = label,
        .format_if_mount_failed = false,
        .read_only = read_only,
    };
    *formatted = false;
    esp_err_t e = esp_vfs_littlefs_register(&cfg);
    if (e == ESP_OK) return ESP_OK;

    if (read_only) {
        snprintf(err, err_len, "只读挂载失败: %s", esp_err_to_name(e));
        return e;
    }
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                       ESP_PARTITION_SUBTYPE_ANY, label);
    if (!partition_blank(p)) {
        snprintf(err, err_len, "挂载失败 (%s) 且分区有数据 → 拒绝自动格式化", esp_err_to_name(e));
        return e;
    }
    ESP_LOGW(TAG, "%s 分区全空白 → 格式化", label);
    esp_err_t fe = esp_littlefs_format(label);
    if (fe != ESP_OK) {
        snprintf(err, err_len, "格式化失败: %s", esp_err_to_name(fe));
        return fe;
    }
    *formatted = true;
    e = esp_vfs_littlefs_register(&cfg);
    if (e != ESP_OK) snprintf(err, err_len, "格式化后仍挂载失败: %s", esp_err_to_name(e));
    return e;
}

/* ══════════════════════════════════════════════════════════════════════
 * NVS
 * ══════════════════════════════════════════════════════════════════════ */
static void test_nvs(void)
{
    hw_item_t *it = hw_begin("store.nvs", "NVS 分区");
    esp_err_t e = nvs_flash_init(); /* 幂等: main 已初始化则直接 OK */
    if (e != ESP_OK) {
        hw_set(it, "nvs_flash_init=%s", esp_err_to_name(e));
        hw_note(it, e == ESP_ERR_NVS_NO_FREE_PAGES
                        ? "NVS 满 → 需人工决定是否擦除 (自检不会替你删凭据)"
                        : "初始化失败 (版本不符/分区损坏)");
        hw_end(it, HW_ST_FAIL);
        return;
    }
    nvs_stats_t st;
    if (nvs_get_stats(NULL, &st) != ESP_OK) { /* NULL = 默认 "nvs" 分区 */
        hw_set(it, "初始化 OK, 统计读取失败");
        hw_end(it, HW_ST_WARN);
        return;
    }
    unsigned pct = st.total_entries ? (unsigned)(st.free_entries * 100 / st.total_entries) : 0;
    hw_set(it, "%u/%u 项空闲 (%u%%), 命名空间 %u", (unsigned)st.free_entries,
           (unsigned)st.total_entries, pct, (unsigned)st.namespace_count);
    if (st.total_entries == 0) {
        hw_note(it, "总项数 0 → 分区没起来");
        hw_end(it, HW_ST_FAIL);
    } else if (pct < HW_EXP_NVS_FREE_MIN_PCT) {
        hw_note(it, "空闲低于 %d%% — 长期写入会把 NVS 撑满", HW_EXP_NVS_FREE_MIN_PCT);
        hw_end(it, HW_ST_WARN);
    } else {
        hw_end(it, HW_ST_PASS);
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * /cfg — LittleFS (config 分区 512KB), 可写
 * ══════════════════════════════════════════════════════════════════════ */
static void test_cfg(void)
{
    hw_item_t *it = hw_begin("store.cfg", "/cfg LittleFS");
    char err[80] = "";
    bool fmt = false;
    esp_err_t e = lfs_mount("config", "/cfg", false, &fmt, err, sizeof(err));
    if (e != ESP_OK) {
        hw_set(it, "挂载失败");
        hw_note(it, "%s", err);
        hw_end(it, HW_ST_FAIL);
        return;
    }
    size_t total = 0, used = 0;
    esp_err_t ie = esp_littlefs_info("config", &total, &used);
    char rt[128] = "";
    bool rw_ok = rw_roundtrip("/cfg/hwtest.tmp", rt, sizeof(rt));
    esp_vfs_littlefs_unregister("config");

    hw_set(it, "%s总 %uKB 用 %uKB, 写读删 %s", fmt ? "格式化后 " : "", (unsigned)(total / 1024),
           (unsigned)(used / 1024), rw_ok ? "OK" : "失败");
    if (ie != ESP_OK) {
        hw_note(it, "容量读取失败");
        hw_end(it, HW_ST_FAIL);
    } else if (total < HW_EXP_CFG_SIZE - 64 * 1024) {
        hw_note(it, "总容量 %uB 远小于分区 %uB", (unsigned)total, HW_EXP_CFG_SIZE);
        hw_end(it, HW_ST_FAIL);
    } else if (!rw_ok) {
        hw_note(it, "%s", rt);
        hw_end(it, HW_ST_FAIL);
    } else {
        hw_end(it, HW_ST_PASS);
    }
}

/* 目录条目数 (-1 = 目录打不开) */
static int dir_count(const char *path)
{
    DIR *d = opendir(path);
    if (!d) return -1;
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) n++;
    closedir(d);
    return n;
}

/* ══════════════════════════════════════════════════════════════════════
 * /data — FAT (data 分区 1MB), 可写
 * ══════════════════════════════════════════════════════════════════════ */
static void test_data(void)
{
    hw_item_t *it = hw_begin("store.data", "/data FATFS");
    esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = false, /* 有数据残留绝不格式化 (usb_storage.c:30-32 红线) */
        .max_files = 4,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
    };
    wl_handle_t wl = WL_INVALID_HANDLE;
    esp_err_t e = esp_vfs_fat_spiflash_mount_rw_wl("/data", "data", &cfg, &wl);
    bool fmt = false;
    if (e != ESP_OK) {
        const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                           ESP_PARTITION_SUBTYPE_ANY, "data");
        if (!partition_blank(p)) {
            hw_set(it, "挂载失败");
            hw_note(it, "%s 且分区有数据 → 拒绝自动格式化", esp_err_to_name(e));
            hw_end(it, HW_ST_FAIL);
            return;
        }
        ESP_LOGW(TAG, "data 分区全空白 → 格式化");
        esp_err_t fe = esp_vfs_fat_spiflash_format_rw_wl("/data", "data");
        if (fe != ESP_OK) {
            hw_set(it, "格式化失败: %s", esp_err_to_name(fe));
            hw_end(it, HW_ST_FAIL);
            return;
        }
        fmt = true;
        e = esp_vfs_fat_spiflash_mount_rw_wl("/data", "data", &cfg, &wl);
        if (e != ESP_OK) {
            hw_set(it, "格式化后仍挂载失败: %s", esp_err_to_name(e));
            hw_end(it, HW_ST_FAIL);
            return;
        }
    }

    uint64_t total = 0, freeb = 0;
    esp_err_t ie = esp_vfs_fat_info("/data", &total, &freeb);
    int nent = dir_count("/data"); /* FAT12/16 根目录是固定表, 上限 512 项 */
    char rt[128] = "";
    bool rw_ok = rw_roundtrip("/data/hwtest.tmp", rt, sizeof(rt));
    esp_vfs_fat_spiflash_unmount_rw_wl("/data", wl);

    hw_set(it, "%s总 %uKB 空 %uKB, 根目录 %d 项, 写读删 %s", fmt ? "格式化后 " : "",
           (unsigned)(total / 1024), (unsigned)(freeb / 1024), nent, rw_ok ? "OK" : "失败");
    /* FAT 可见容量天生小于分区 (WL + FAT 表开销) → 只卡缩水下限 */
    if (ie != ESP_OK) {
        hw_note(it, "容量读取失败");
        hw_end(it, HW_ST_FAIL);
    } else if (!rw_ok) {
        hw_note(it, "%s", rt);
        if (nent >= HW_EXP_FAT_ROOT_MAX - 32)
            hw_note(it, "根目录表已无空位 → 卷内容坏(非硬件): chkdsk 或格式化 data 分区");
        hw_end(it, HW_ST_FAIL);
    } else if (total < HW_EXP_DATA_MIN_BYTES) {
        hw_note(it, "容量 %uB 低于下限 %uB → 分区/WL 缩水", (unsigned)total,
                (unsigned)HW_EXP_DATA_MIN_BYTES);
        hw_end(it, HW_ST_WARN);
    } else {
        hw_end(it, HW_ST_PASS);
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * /assets — LittleFS 只读 (未烧 assets → SKIP, 不算错)
 * ══════════════════════════════════════════════════════════════════════ */
static void test_assets(void)
{
    hw_item_t *it = hw_begin("store.assets", "/assets LittleFS");
    char err[80] = "";
    bool fmt = false;
    esp_err_t e = lfs_mount("assets", "/assets", true, &fmt, err, sizeof(err));
    if (e != ESP_OK) {
        hw_set(it, "只读挂载失败");
        hw_note(it, "%s (新板未烧 assets 属正常: 跑一次主工程烧录)", err);
        hw_end(it, HW_ST_SKIP);
        return;
    }
    size_t total = 0, used = 0;
    esp_err_t ie = esp_littlefs_info("assets", &total, &used);

    /* 目录枚举 + 真读一个已知文件的前 16 字节 */
    int nfiles = 0;
    DIR *d = opendir("/assets");
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.') continue;
            nfiles++;
        }
        closedir(d);
    }
    uint8_t head[16] = {0};
    size_t got = 0;
    FILE *f = fopen("/assets/anims.bin", "rb");
    if (f) {
        got = fread(head, 1, sizeof(head), f);
        fclose(f);
    }
    esp_vfs_littlefs_unregister("assets");

    hw_set(it, "用 %u/%uKB, %d 个文件, anims.bin 头 %uB", (unsigned)(used / 1024),
           (unsigned)(total / 1024), nfiles, (unsigned)got);
    if (ie != ESP_OK || total == 0) {
        hw_note(it, "容量读取失败");
        hw_end(it, HW_ST_FAIL);
    } else if (used == 0) {
        hw_note(it, "分区是空的 → 未烧 assets (先跑主工程烧一次)");
        hw_end(it, HW_ST_SKIP);
    } else if (nfiles == 0) {
        hw_note(it, "有占用但目录枚举为空 → 文件系统结构异常");
        hw_end(it, HW_ST_FAIL);
    } else if (got == 0) {
        hw_note(it, "anims.bin 打不开/读不出 → assets 内容不可用");
        hw_end(it, HW_ST_FAIL);
    } else {
        hw_end(it, HW_ST_PASS);
    }
}

void hw_storage_run(void)
{
    ESP_LOGI(TAG, "──── H 段: 存储 ────");
    test_nvs();
    test_cfg();
    test_data();
    test_assets();
}
