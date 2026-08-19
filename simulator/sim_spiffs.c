/**
 * @file sim_spiffs.c
 * @brief fopen wrapper: redirect /spiffs/ paths → project's spiffs/ directory.
 *
 * Link with -Wl,--wrap=fopen so all fopen() calls go through __wrap_fopen.
 * Only intercepts /spiffs/ prefixed paths; others pass through to __real_fopen.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Forward declaration: the real fopen, provided by the linker wrap */
extern FILE *__real_fopen(const char *path, const char *mode);

/* 设备 SPIFFS 镜像源目录 (与 CMakeLists.txt 中 spiffs_create_partition_image 一致) */
#ifndef SIM_SPIFFS_DIR
#define SIM_SPIFFS_DIR "../spiffs"
#endif

FILE *__wrap_fopen(const char *path, const char *mode) {
    if (path && strncmp(path, "/spiffs/", 8) == 0) {
        /* 环境变量 SIM_ZH_FONT: 覆盖 zh.bin 字体路径 (用于字体测试) */
        if (strcmp(path + 8, "zh.bin") == 0) {
            const char *override = getenv("SIM_ZH_FONT");
            if (override && override[0] != '\0') {
                return __real_fopen(override, mode);
            }
        }
        /* Translate: /spiffs/xxx_00.bin → ../spiffs/xxx_00.bin */
        char local[1024];
        snprintf(local, sizeof(local), SIM_SPIFFS_DIR "/%s", path + 8);
        return __real_fopen(local, mode);
    }
    return __real_fopen(path, mode);
}
