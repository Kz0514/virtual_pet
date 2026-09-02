/**
 * @file sim_spiffs.c
 * @brief 路径重定向层: 把设备路径 (/assets/, /data/) 映射到项目本地目录
 *
 * 链接选项: -Wl,--wrap=fopen --wrap=open --wrap=stat64i32 --wrap=opendir
 *
 * 新 UI 代码的文件 IO 已全走 POSIX fd (open/read/close), fopen 只留历史兼容:
 *   - /assets/xxx → ../assets_fs/xxx   (实机资源镜像源目录, 与固件资源 100% 一致)
 *   - /data/xxx   → ../simdata/xxx  (日记等数据, 首次访问自动建 simdata/diary)
 *
 * 注意 (MinGW 坑): stat() 在 sys/stat.h 被 __MINGW_ASM_CALL(stat64i32) 编译期
 * 重定向到 stat64i32 符号, 所以 wrap 的是 stat64i32 而不是 stat。
 * read/lseek/close 的 fd 来自 __real_open (真实 CRT fd), 无需 wrap;
 * opendir wrap 返回真实 DIR*, readdir/closedir 走真实符号即可。
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <direct.h>

/* ── 真实函数声明 (由 linker --wrap 提供) ── */
extern FILE *__real_fopen(const char *path, const char *mode);
extern int __real_open(const char *path, int flags, ...);
extern int __real_stat64i32(const char *path, struct stat *st);
extern DIR *__real_opendir(const char *path);

/* 设备目录映射 (相对 simulator/ 运行目录) */
#ifndef SIM_SPIFFS_LOCAL
#define SIM_SPIFFS_LOCAL "../simulator/spiffs"   /* 模拟器专属资源 (动画包等) */
#endif
#ifndef SIM_SPIFFS_DIR
#define SIM_SPIFFS_DIR "../assets_fs"            /* 实机资源 (字体 zh.bin 回落) */
#endif
#ifndef SIM_DATA_DIR
#define SIM_DATA_DIR "../simdata"
#endif

/* ── 路径翻译: 命中设备路径返回重定向后的本地路径; 否则返回 NULL ── */
static const char *translate_path(const char *path, char *out, size_t out_sz) {
    if (!path) return NULL;

    /* 环境变量 SIM_ZH_FONT: 覆盖 zh.bin 字体路径 (用于字体测试) */
    if (strncmp(path, "/assets/", 8) == 0) {
        if (strcmp(path + 8, "zh.bin") == 0) {
            const char *override = getenv("SIM_ZH_FONT");
            if (override && override[0] != '\0') return override;
        }
        /* 模拟器资源独立: simulator/spiffs/ 优先 (动画包 anims.bin 等),
         * 缺失回落实机资源 ../spiffs/ (zh.bin 字体共用, 不复制 2.7MB) */
        snprintf(out, out_sz, SIM_SPIFFS_LOCAL "/%s", path + 8);
        struct stat st;
        if (__real_stat64i32(out, &st) == 0) return out;
        snprintf(out, out_sz, SIM_SPIFFS_DIR "/%s", path + 8);
        return out;
    }
    if (strncmp(path, "/data/", 6) == 0) {
        snprintf(out, out_sz, SIM_DATA_DIR "/%s", path + 6);
        return out;
    }
    return NULL;
}

/* 惰性建目录: /data/ 首次访问时保证 ../simdata/diary 存在 (日记屏测试用) */
static void ensure_data_dirs(void) {
    static int done = 0;
    if (done) return;
    _mkdir(SIM_DATA_DIR);              /* 已存在时返回 EEXIST, 忽略 */
    char diary_dir[512];
    snprintf(diary_dir, sizeof(diary_dir), SIM_DATA_DIR "/diary");
    _mkdir(diary_dir);
    done = 1;
}

/* ── fopen (历史兼容, 新代码已全走 fd) ── */
FILE *__wrap_fopen(const char *path, const char *mode) {
    char local[1024];
    const char *tr = translate_path(path, local, sizeof(local));
    return __real_fopen(tr ? tr : path, mode);
}

/* ── open: varargs 透传 mode (仅 O_CREAT 时有第三个参数) ── */
int __wrap_open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    if (strncmp(path, "/data/", 6) == 0) ensure_data_dirs();
    char local[1024];
    const char *tr = translate_path(path, local, sizeof(local));
    /* MinGW 坑: 固件代码的 open() 从不带 O_BINARY (POSIX 无此概念), Windows
     * 默认文本模式下 read() 遇 0x1A 当 EOF 截断 → 二进制资源 (字体/动画帧) 读不全.
     * wrap 层强制二进制模式, 行为与实机一致. */
    flags |= O_BINARY;
    return __real_open(tr ? tr : path, flags, mode);
}

/* ── stat: MinGW 编译期重定向到 stat64i32 ── */
int __wrap_stat64i32(const char *path, struct stat *st) {
    char local[1024];
    const char *tr = translate_path(path, local, sizeof(local));
    return __real_stat64i32(tr ? tr : path, st);
}

/* ── opendir: 返回真实 DIR* (readdir/closedir 无需 wrap) ── */
DIR *__wrap_opendir(const char *path) {
    if (strncmp(path, "/data/", 6) == 0) ensure_data_dirs();
    char local[1024];
    const char *tr = translate_path(path, local, sizeof(local));
    return __real_opendir(tr ? tr : path);
}
