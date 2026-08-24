/** @file mock/esp_app_desc.h — Mock esp_app_desc (设置页"关于"显示版本号) */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 只保留设置页用到的字段 (version 在真实结构体中也排最前) */
typedef struct {
    char version[32];
    char project_name[32];
    char idf_ver[32];
} esp_app_desc_t;

/* 实现见 mock_impl.c */
const esp_app_desc_t *esp_app_get_description(void);

#ifdef __cplusplus
}
#endif
