/**
 * @file pet_avatar.c
 * @brief LLM 控制的多动画引擎 — idle常驻, 其余按需加载, 播完3轮回idle
 *
 * 素材: zhanli(idle/sad) happy gaoxingjiangjie / talk baoxiongshuohua
 *        sleep shuijiao / eating e / squat dunzhe / blush miantianxiao
 *        pathead motou / scratch naotou / pointself zhizheziji
 */
#include "pet_avatar.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "avatar";

#define FW           240
#define FH           240
#define FRAME_SIZE   (FW * FH * 2)
#define IDLE_FRAMES  5
#define MAX_ANIM     20
#define PLAY_LOOPS    3     /* 播放循环次数后回到 idle */

typedef struct {
    const char *prefix;
    uint8_t     max_frames;
} anim_asset_t;

static const anim_asset_t s_assets[] = {
    [PET_ANIM_IDLE]      = {"zhanli",  5},
    [PET_ANIM_HAPPY]     = {"happy",   6},
    [PET_ANIM_SAD]       = {"zhanli",  5},
    [PET_ANIM_EXCITED]   = {"talk",   10},
    [PET_ANIM_SLEEPY]    = {"sleep",  13},
    [PET_ANIM_EATING]    = {"eating", 19},
    [PET_ANIM_SURPRISED] = {"squat",   2},
    [PET_ANIM_BLUSH]     = {"blush",  14},
    [PET_ANIM_PATHEAD]   = {"pathead", 7},
    [PET_ANIM_SCRATCH]   = {"scratch", 2},
    [PET_ANIM_POINTSELF] = {"pointself", 5},
};

typedef struct {
    uint8_t        *data;
    lv_image_dsc_t  dsc;
} frame_t;

static frame_t s_idle[IDLE_FRAMES];
static uint8_t s_idle_count = 0;

static frame_t s_dynamic[MAX_ANIM];
static uint8_t s_dynamic_count = 0;
static pet_anim_t s_loaded_anim = PET_ANIM_IDLE;

typedef struct {
    const avatar_frame_t *frames;
    frame_t              *pool;
    uint8_t  count;
    uint8_t  loop;
    uint8_t  default_fps;
} anim_seq_t;

static anim_seq_t  s_anims[PET_ANIM_COUNT];
static lv_obj_t   *s_img;
static lv_timer_t *s_frame_timer;
static pet_anim_t  s_current_anim;
static uint8_t     s_seq_pos;
static uint16_t    s_override_ms;
static uint8_t     s_loop_count;
static bool        s_loop_active;
static uint8_t     s_loop_remaining;
static bool        s_hold;           /* 保持模式: 按住期间循环不回 idle */

/* 延迟请求 — WS回调只存请求, LVGL定时器执行(栈更大) */
static volatile int s_pending_anim = -1;
static uint32_t s_pending_at = 0;  /* 请求时间戳 — 延迟800ms避开TTS下载抢SPI总线 */
static uint8_t s_play_loops = 3;   /* 动画总播放轮数 */

/* ══════ SPIFFS ══════ */

static bool load_one_frame(const char *path, frame_t *f) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    f->data = heap_caps_malloc(FRAME_SIZE, MALLOC_CAP_SPIRAM);
    if (!f->data) { fclose(fp); return false; }
    size_t rd = fread(f->data, 1, FRAME_SIZE, fp);
    fclose(fp);
    if (rd != FRAME_SIZE) { heap_caps_free(f->data); f->data = NULL; return false; }
    f->dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    f->dsc.header.w      = FW;
    f->dsc.header.h      = FH;
    f->dsc.header.stride = FW * 2;
    f->dsc.data          = f->data;
    f->dsc.data_size     = FRAME_SIZE;
    return true;
}

static int load_frames(frame_t *pool, int pool_max,
                       const char *prefix, int max_frames) {
    char path[48];
    int loaded = 0, n = (max_frames < pool_max) ? max_frames : pool_max;
    for (int i = 0; i < n; i++) {
        snprintf(path, sizeof(path), "/spiffs/%s_%02d.bin", prefix, i);
        if (!load_one_frame(path, &pool[i])) break;
        loaded++;
    }
    return loaded;
}

static void unload_frames(frame_t *pool, int count) {
    for (int i = 0; i < count; i++) {
        if (pool[i].data) { heap_caps_free(pool[i].data); pool[i].data = NULL; }
    }
}

/* 渐进加载: 快速加载前3帧立即开播, 剩余帧在播放中逐帧后台加载 */
#define QUICK_START_FRAMES 3
static int s_bg_next = 0;            /* 下一个待加载帧索引 */
static int s_bg_total = 0;           /* 总帧数上限 */
static const char *s_bg_prefix;      /* 当前加载前缀 */
static pet_anim_t s_cached_anim = PET_ANIM_IDLE;  /* 动态池缓存的动画 (切回idle不卸载) */

static void switch_to_anim(pet_anim_t anim) {
    if (anim == s_loaded_anim) return;
    if (anim == PET_ANIM_IDLE) {
        /* 不卸载 — 保留缓存, 同一动画再次触发零延迟; 后台续载继续 */
        s_loaded_anim = PET_ANIM_IDLE;
        return;
    }
    /* 缓存命中: 帧还在动态池里 */
    if (anim == s_cached_anim && s_dynamic_count > 0) {
        s_loaded_anim = anim;
        s_anims[anim].pool = s_dynamic;
        s_anims[anim].count = s_assets[anim].max_frames;
        if (s_anims[anim].count > s_dynamic_count) s_anims[anim].count = s_dynamic_count;
        s_bg_next = s_dynamic_count;
        s_bg_total = (s_assets[anim].max_frames < MAX_ANIM) ? s_assets[anim].max_frames : MAX_ANIM;
        s_bg_prefix = s_assets[anim].prefix;
        ESP_LOGI(TAG, "anim %d 缓存命中 (%d 帧), 零延迟", anim, s_dynamic_count);
        return;
    }

    unload_frames(s_dynamic, s_dynamic_count);
    s_dynamic_count = 0;
    s_loaded_anim = PET_ANIM_IDLE;
    s_bg_total = 0;
    s_cached_anim = PET_ANIM_IDLE;

    const anim_asset_t *a = &s_assets[anim];
    /* 快速开播: 只同步加载前3帧 (~300ms), 不卡UI */
    int n = load_frames(s_dynamic, QUICK_START_FRAMES, a->prefix, QUICK_START_FRAMES);
    if (n == 0) {
        ESP_LOGW(TAG, "无法加载 anim %d (%s)", anim, a->prefix);
        return;
    }
    s_dynamic_count = n;
    s_loaded_anim = anim;
    s_cached_anim = anim;
    s_bg_prefix = a->prefix;
    s_bg_next = n;
    s_bg_total = (a->max_frames < MAX_ANIM) ? a->max_frames : MAX_ANIM;
    ESP_LOGI(TAG, "anim %d (%s): 快速加载 %d 帧, 剩余 %d 帧后台加载",
             anim, a->prefix, n, s_bg_total - n);

    s_anims[anim].pool = s_dynamic;
    /* 恢复完整序列长度, 再截断到已加载帧数 */
    s_anims[anim].count = s_assets[anim].max_frames;
    if (s_anims[anim].count > n) s_anims[anim].count = n;
}

/* 后台逐帧加载 — 每tick调一次, 分摊SPI读取避免卡顿 */
static void bg_load_tick(void) {
    if (s_bg_next >= s_bg_total) return;
    /* 播放期间暂停帧加载 — 播放对 PSRAM 读时序敏感, 抢总线会卡音;
     * 流式下载 (96KB/s 细流) 期间正常加载, 不冻结动画 */
    extern bool tts_client_is_playing(void);
    if (tts_client_is_playing()) return;
    char path[48];
    snprintf(path, sizeof(path), "/spiffs/%s_%02d.bin", s_bg_prefix, s_bg_next);
    if (load_one_frame(path, &s_dynamic[s_bg_next])) {
        s_dynamic_count = s_bg_next + 1;
        /* 扩展缓存动画的序列 — 新帧加入循环 */
        if (s_cached_anim != PET_ANIM_IDLE &&
            s_anims[s_cached_anim].count < s_bg_next + 1 &&
            s_anims[s_cached_anim].count < s_assets[s_cached_anim].max_frames)
            s_anims[s_cached_anim].count++;
    } else {
        s_bg_total = s_bg_next;   /* 加载失败 — 停止继续 */
    }
    s_bg_next++;
}

/* ══════ 帧序表 (子循环定义来自各动画 manifest.c) ══════ */

static void build_seq(avatar_frame_t *dst, int count, uint16_t dur, uint8_t fps) {
    for (int i = 0; i < count; i++)
        dst[i] = (avatar_frame_t){i, dur, 0, 0};
}

static avatar_frame_t s_seq_idle[5];
static avatar_frame_t s_seq_happy[6];
static avatar_frame_t s_seq_talk[10];
static avatar_frame_t s_seq_sleep[13];
static avatar_frame_t s_seq_eating[19];
static avatar_frame_t s_seq_squat[2];
static avatar_frame_t s_seq_blush[14];
static avatar_frame_t s_seq_pathead[7];
static avatar_frame_t s_seq_scratch[2];
static avatar_frame_t s_seq_pointself[5];

/* ══════ 播放 ══════ */

static void show_frame(uint8_t idx, frame_t *pool) {
    if (pool && pool[idx].data) lv_image_set_src(s_img, &pool[idx].dsc);
}

static void do_play(pet_anim_t anim) {
    switch_to_anim(anim);
    s_current_anim = anim;
    s_seq_pos = 0;
    s_loop_count = 0;
    s_loop_active = false;
    s_loop_remaining = 0;
    anim_seq_t *seq = &s_anims[anim];
    if (seq->count && seq->frames && seq->pool)
        show_frame(seq->frames[0].frame_idx, seq->pool);
    uint16_t dur = seq->frames[0].duration_ms;
    if (dur == 0) dur = s_override_ms ? s_override_ms : 1000 / seq->default_fps;
    lv_timer_set_period(s_frame_timer, dur > 0 ? dur : 150);
}

/* 线程安全入口: 只记录请求 */
void pet_avatar_play(pet_anim_t anim) {
    if (anim < PET_ANIM_COUNT) {
        s_pending_anim = anim;
        s_pending_at = xTaskGetTickCount();
    }
}

void pet_avatar_play_fast(pet_anim_t anim) {
    if (anim < PET_ANIM_COUNT) {
        s_pending_anim = anim;
        s_pending_at = 0;   /* 时间戳0 → 最小延迟检查立即通过 */
    }
}

pet_anim_t pet_avatar_get_current(void) { return s_current_anim; }

void pet_avatar_set_fps(uint8_t fps) { s_override_ms = fps ? 1000 / fps : 0; }

void pet_avatar_set_hold(bool on) { s_hold = on; }

void pet_avatar_set_sequence(const avatar_frame_t *frames, uint8_t count, uint8_t loop) {
    s_anims[s_current_anim].frames = frames;
    s_anims[s_current_anim].count  = count;
    s_anims[s_current_anim].loop   = loop;
}

/* ── 帧推进 + 子循环/整组循环 (LVGL定时器上下文执行) ── */
static void frame_timer_cb(lv_timer_t *t) {
    /* 处理动画请求 — 流式 TTS 下载是 96KB/s 匀速细流,
     * 不再构成 SPI 突发争用, 立即开播 (原 800ms 最小延迟与下载等待一并移除) */
    if (s_pending_anim >= 0 && s_pending_anim < PET_ANIM_COUNT) {
        pet_anim_t req = (pet_anim_t)s_pending_anim;
        s_pending_anim = -1;
        do_play(req);
        return;
    }

    anim_seq_t *seq = &s_anims[s_current_anim];
    if (seq->count < 2 || !seq->pool) return;

    /* 后台渐进加载剩余帧 — 每tick一帧, 分摊SPI读取 */
    bg_load_tick();

    /* ── 子循环处理 (loop_back > 0 的帧) ── */
    const avatar_frame_t *f = &seq->frames[s_seq_pos];
    if (f->loop_back > 0) {
        if (!s_loop_active) {
            s_loop_active = true;
            /* 子循环总次数 = s_play_loops (已播1次, 剩余 loops-1) */
            s_loop_remaining = (s_play_loops > 0) ? s_play_loops - 1 : 0;
        }
        /* 保持模式下子循环无限重复、不消耗计数 (按住期间循环播放) */
        if (s_hold || s_loop_remaining > 0) {
            if (!s_hold) s_loop_remaining--;
            s_seq_pos -= f->loop_back;
            f = &seq->frames[s_seq_pos];
            show_frame(f->frame_idx, seq->pool);
            uint16_t dur = f->duration_ms;
            if (dur == 0) dur = s_override_ms;
            if (dur == 0) dur = 1000 / seq->default_fps;
            lv_timer_set_period(t, dur > 0 ? dur : 150);
            return;
        }
        /* 子循环耗尽 — 继续前进 */
        s_loop_active = false;
    }

    /* ── 前进到下一帧 ── */
    s_seq_pos++;
    if (s_seq_pos >= seq->count) {
        s_seq_pos = 0;
        s_loop_count++;
        bool has_subloop = false;
        for (int i = 0; i < seq->count; i++)
            if (seq->frames[i].loop_back > 0) { has_subloop = true; break; }
        if (s_current_anim != PET_ANIM_IDLE && !s_hold) {
            /* 有子循环: 序列播完一遍即回idle; 无子循环: 整组循环 loops 次 */
            uint8_t target = has_subloop ? 1 : s_play_loops;
            if (s_loop_count >= target) {
                ESP_LOGI(TAG, "anim %d 完成 (%d次), 回 idle", s_current_anim, s_loop_count);
                do_play(PET_ANIM_IDLE);
                return;
            }
        }
    }

    f = &seq->frames[s_seq_pos];
    show_frame(f->frame_idx, seq->pool);
    uint16_t dur = f->duration_ms;
    if (dur == 0) dur = s_override_ms;
    if (dur == 0) dur = 1000 / seq->default_fps;
    lv_timer_set_period(t, dur > 0 ? dur : 150);
}

/* ══════ 初始化 ══════ */

esp_err_t pet_avatar_init(void) {
    build_seq(s_seq_idle,  5, 300, 8);
    s_seq_idle[2].duration_ms = 800;
    s_seq_idle[3].duration_ms = 800;
    build_seq(s_seq_happy, 6, 300, 8);
    build_seq(s_seq_talk, 10, 200, 10);
    /* baoxiongshuohua: 帧4起点, 帧5回跳1步 → 4-5子循环 */
    s_seq_talk[5].loop_back = 1;
    build_seq(s_seq_sleep,13, 500, 6);
    /* shuijiao: 帧4起点, 帧5回跳1步 → 4-5子循环 */
    s_seq_sleep[5].loop_back = 1;
    build_seq(s_seq_eating,19,200, 10);
    /* dunzhe: 帧0=1500ms, 帧1=300ms */
    build_seq(s_seq_squat, 2, 300, 8);
    s_seq_squat[0].duration_ms = 1500;
    build_seq(s_seq_blush,14, 200, 10);
    /* motou: 300ms每帧, 帧3子循环起点, 帧6回跳3步 → 3-6子循环 */
    build_seq(s_seq_pathead,7,300, 10);
    s_seq_pathead[6].loop_back = 3;
    /* naotou: 帧0=1500ms, 帧1=300ms */
    build_seq(s_seq_scratch,2,300, 8);
    s_seq_scratch[0].duration_ms = 1500;
    build_seq(s_seq_pointself,5,300, 8);

    ESP_LOGI(TAG, "加载 idle (zhanli)...");
    s_idle_count = load_frames(s_idle, IDLE_FRAMES, "zhanli", 5);
    if (s_idle_count < 5) { ESP_LOGE(TAG, "idle 帧不足"); return ESP_FAIL; }

    s_anims[PET_ANIM_IDLE]      = (anim_seq_t){s_seq_idle,      s_idle, 5, 1, 8};
    s_anims[PET_ANIM_HAPPY]     = (anim_seq_t){s_seq_happy,     NULL,   6, 1, 8};
    s_anims[PET_ANIM_SAD]       = (anim_seq_t){s_seq_idle,      s_idle, 5, 1, 8};
    s_anims[PET_ANIM_EXCITED]   = (anim_seq_t){s_seq_talk,      NULL,  10, 1, 10};
    s_anims[PET_ANIM_SLEEPY]    = (anim_seq_t){s_seq_sleep,     NULL,  13, 1, 6};
    s_anims[PET_ANIM_EATING]    = (anim_seq_t){s_seq_eating,    NULL,  19, 1, 10};
    s_anims[PET_ANIM_SURPRISED] = (anim_seq_t){s_seq_squat,     NULL,   2, 1, 8};
    s_anims[PET_ANIM_BLUSH]     = (anim_seq_t){s_seq_blush,     NULL,  14, 1, 10};
    s_anims[PET_ANIM_PATHEAD]   = (anim_seq_t){s_seq_pathead,   NULL,   7, 1, 10};
    s_anims[PET_ANIM_SCRATCH]   = (anim_seq_t){s_seq_scratch,   NULL,   2, 1, 8};
    s_anims[PET_ANIM_POINTSELF] = (anim_seq_t){s_seq_pointself, NULL,   5, 1, 8};

    s_img = lv_image_create(lv_screen_active());
    lv_obj_set_size(s_img, FW, FH);
    lv_obj_set_pos(s_img, 0, 0);
    lv_obj_set_style_pad_all(s_img, 0, 0);
    lv_obj_set_style_border_width(s_img, 0, 0);

    s_current_anim = PET_ANIM_IDLE;
    s_loaded_anim  = PET_ANIM_IDLE;
    s_seq_pos = 0;
    s_loop_count = 0;
    s_pending_anim = -1;
    show_frame(0, s_idle);

    s_frame_timer = lv_timer_create(frame_timer_cb, 300, NULL);
    lv_timer_set_repeat_count(s_frame_timer, -1);

    ESP_LOGI(TAG, "就绪: idle=%d帧 动态池=%d帧 默认%d轮回idle",
             s_idle_count, MAX_ANIM, s_play_loops);
    return ESP_OK;
}
