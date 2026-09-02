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
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "webp/decode.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

static const char *TAG = "avatar";

#define FW 240
#define FH 240
#define FRAME_SIZE (FW * FH * 2)
#define IDLE_FRAMES 5
#define MAX_ANIM 20
#define PLAY_FRAMES 6 /* 播放期续载帧数上限 — PSRAM 撞顶防护 */
#define PLAY_LOOPS 3 /* 播放循环次数后回到 idle */

/* anims.bin 打包格式 — magic 校验失败即整体拒绝 (优雅降级: 宠物不显示,
 * 系统照常), 不会按错误帧表解出垃圾帧.
 * size_flags: bit31=原始 RGB565 未压缩; bits30-28=codec (0=RLE,
 * 1=WebP 无损); 低 28 位 = 帧数据字节数 */
#define PACK_MAGIC_V2    0x32494E41u /* "ANI2" */
#define PACK_VERSION     2
#define PACK_MAX_FRAMES  256
#define PACK_FLAG_RAW    0x80000000u
#define PACK_SIZE_MASK   0x0FFFFFFFu
#define PACK_CODEC_SHIFT 28
#define CODEC_RLE  0
#define CODEC_WEBP 1

typedef struct {
    const char *prefix;
    uint8_t max_frames;
} anim_asset_t;

static const anim_asset_t s_assets[] = {
    [PET_ANIM_IDLE] = {"zhanli", 5},
    [PET_ANIM_HAPPY] = {"happy", 6},
    [PET_ANIM_SAD] = {"zhanli", 5},
    [PET_ANIM_EXCITED] = {"talk", 10},
    [PET_ANIM_SLEEPY] = {"sleep", 13},
    [PET_ANIM_EATING] = {"eating", 19},
    [PET_ANIM_SURPRISED] = {"squat", 2},
    [PET_ANIM_BLUSH] = {"blush", 14},
    [PET_ANIM_PATHEAD] = {"pathead", 7},
    [PET_ANIM_SCRATCH] = {"scratch", 2},
    [PET_ANIM_POINTSELF] = {"pointself", 5},
};

typedef struct {
    uint8_t *data;
    lv_image_dsc_t dsc;
} frame_t;

static frame_t s_idle[IDLE_FRAMES];
static uint8_t s_idle_count = 0;

static frame_t s_dynamic[MAX_ANIM];
static pet_anim_t s_loaded_anim = PET_ANIM_IDLE;
/* s_dynamic_count 声明见下方共享状态块 (volatile, 跨核) */

/* ══════ anims.bin 动画包 (SPIFFS 单文件 + 帧表) ══════
 * 单文件 + 帧表使 open 毫秒级; 播放 = lseek+read 块读 + 锁外解压, fd 常开。
 * 头部 {magic,version,total} + total×{anim_id,off,size_flags} + 帧数据平铺
 * (off 相对数据区起点); 帧数由文件决定, 与枚举数解耦 (缺素材如 SAD 不出条目);
 * 素材演进只重打包重烧 assets, 分区表不动。 */
static int s_pack_fd = -1;
static uint32_t s_pack_data_off;             /* 数据区绝对文件偏移 (头部之后) */
static uint16_t s_pack_total;                /* 帧表条目数 */
/* 帧表 (静态 .bss, 不占运行期堆): 12B/条目 × PACK_MAX_FRAMES */
static uint8_t s_pack_tab[PACK_MAX_FRAMES * 12];
static uint8_t s_pack_first[PET_ANIM_COUNT]; /* 每动画首帧在帧表的序号 */
static uint8_t s_pack_count[PET_ANIM_COUNT]; /* 每动画帧数 */

static bool pack_ensure_open(void)
{
    if (s_pack_fd >= 0) return true;
    s_pack_fd = open("/assets/anims.bin", O_RDONLY);
    if (s_pack_fd < 0) return false;
    uint8_t hdr[12];
    if (read(s_pack_fd, hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) goto fail;
    uint32_t magic, version, total;
    memcpy(&magic, hdr, 4);
    memcpy(&version, hdr + 4, 4);
    memcpy(&total, hdr + 8, 4);
    if (magic != PACK_MAGIC_V2 || version != PACK_VERSION ||
        total == 0 || total > PACK_MAX_FRAMES) {
        ESP_LOGE(TAG, "anims.bin 头部无效 (magic=%08x version=%u frames=%u)",
                 magic, version, total);
        goto fail;
    }
    /* 一次性读全帧表 (防 12B 小读) */
    size_t tab_bytes = (size_t)total * 12;
    if (read(s_pack_fd, s_pack_tab, tab_bytes) != (ssize_t)tab_bytes) goto fail;
    off_t file_end = lseek(s_pack_fd, 0, SEEK_END);
    if (file_end < 0) goto fail;
    s_pack_data_off = (uint32_t)(12 + tab_bytes);
    s_pack_total = (uint16_t)total;
    if (file_end < (off_t)(s_pack_data_off + 4)) goto fail;
    uint32_t data_len = (uint32_t)file_end - s_pack_data_off;

    memset(s_pack_first, 0xFF, sizeof(s_pack_first));
    memset(s_pack_count, 0, sizeof(s_pack_count));
    for (int i = 0; i < (int)total; i++) {
        const uint8_t *e = &s_pack_tab[i * 12];
        uint32_t anim_id, off, size_flags;
        memcpy(&anim_id, e, 4);
        memcpy(&off, e + 4, 4);
        memcpy(&size_flags, e + 8, 4);
        if (anim_id >= PET_ANIM_COUNT) continue;      /* 缺素材枚举不出条目 */
        uint32_t size = size_flags & PACK_SIZE_MASK;
        if ((size_flags & PACK_FLAG_RAW) ? (size != FRAME_SIZE)
                                         : (size < 4 || size > FRAME_SIZE))
            continue;                                 /* 条目损坏, 跳过 */
        if (off > data_len || size > data_len - off) continue;
        if (s_pack_count[anim_id] == 0) s_pack_first[anim_id] = (uint8_t)i;
        s_pack_count[anim_id]++;
    }
    ESP_LOGI(TAG, "anims.bin v2 打开: %u 帧, fd=%d", total, s_pack_fd);
    return true;
fail:
    close(s_pack_fd);
    s_pack_fd = -1;
    return false;
}

typedef struct {
    const avatar_frame_t *frames;
    frame_t *pool;
    volatile uint8_t count; /* 跨核: anim_load 任务扩展帧数 (锁内写) */
    uint8_t loop;
    uint8_t default_fps;
} anim_seq_t;

static anim_seq_t s_anims[PET_ANIM_COUNT];
static lv_obj_t *s_img;
static lv_timer_t *s_frame_timer;
static pet_anim_t s_current_anim;
static uint8_t s_seq_pos;
static uint16_t s_override_ms;
static uint8_t s_loop_count;
static bool s_loop_active;
static uint8_t s_loop_remaining;
static bool s_hold; /* 保持模式: 按住期间循环不回 idle */

/* 延迟请求 — WS回调只存请求, LVGL定时器执行(栈更大) */
static volatile int s_pending_anim = -1;
static uint32_t s_pending_at = 0; /* 请求时间戳 (play_fast 置 0 标记立即执行) */
static uint8_t s_play_loops = 3;  /* 动画总播放轮数 */

/* ══════ SPIFFS ══════ */

/* ── 帧解码器 (纯 C 无平台依赖, 模拟器共享同一文件) ── */

/* pair-RLE 解码: {u16 run_len, u16 color565} 序列 → RGB565 目标.
 * 输出必须恰好 out_cap_px 像素; 行程长 0 或越界判损坏.
 * u32 打包对写 (颜色|颜色<<16), 先补齐到 4 像素对齐 (基地址 4 对齐) —
 * Xtensa 非对齐 u32 写走慢路径, 且 (out_px&1) 判断在 4k+2 偏移不触发 */
static bool anim_rle_decode(const uint8_t *src, size_t src_len,
                            uint8_t *dst, size_t out_cap_px)
{
    size_t out_px = 0, off = 0;
    while (off + 4 <= src_len) {
        uint16_t run = src[off] | ((uint16_t)src[off + 1] << 8);
        uint16_t color = src[off + 2] | ((uint16_t)src[off + 3] << 8);
        off += 4;
        if (run == 0 || run > out_cap_px - out_px) return false;
        if (out_px & 1) {                 /* 补到偶数像素 */
            ((uint16_t *)dst)[out_px++] = color;
            run--;
        }
        if ((out_px & 2) && run >= 2) {   /* 补到 4 像素对齐 */
            ((uint16_t *)dst)[out_px++] = color;
            ((uint16_t *)dst)[out_px++] = color;
            run -= 2;
        }
        uint32_t pair = color | ((uint32_t)color << 16);
        uint32_t *dst32 = (uint32_t *)(dst + out_px * 2);
        while (run >= 2) {
            *dst32++ = pair;
            out_px += 2;
            run -= 2;
        }
        if (run) ((uint16_t *)dst)[out_px++] = color;
    }
    return out_px == out_cap_px;
}

/* WebP 无损帧: WebPDecodeBGRAInto (BGRA = ARGB8888 字节序, 与 diary_screen
 * 涂鸦同一 API) → ARGB 暂存 → 转 RGB565 入目标. 解码器内部 malloc 已由
 * vendored libwebp 补丁切到 PSRAM (utils.c WebPMalloc/WebPFree). */
static bool anim_webp_decode(const uint8_t *src, size_t src_len,
                             uint8_t *dst, uint8_t *argb)
{
    int w = 0, h = 0;
    if (!WebPGetInfo(src, src_len, &w, &h)) return false;
    if (w != FW || h != FH) return false;
    if (!WebPDecodeBGRAInto(src, src_len, argb, (size_t)FW * FH * 4, FW * 4))
        return false;
    uint16_t *dst16 = (uint16_t *)dst;
    for (int i = 0; i < FW * FH; i++) {
        uint8_t b = argb[i * 4], g = argb[i * 4 + 1], r = argb[i * 4 + 2];
        dst16[i] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
    return true;
}

/* 后台路径解码暂存 (anim_load 任务独占, 惰性分配, 常驻不释放):
 * comp = 压缩数据 (≤FRAME_SIZE, 压缩更大时工具存原始), argb = WebP 输出 */
static uint8_t *s_scratch_comp;
static uint8_t *s_scratch_argb;

/* 从包读一帧 — lseek+read 块读压缩数据到暂存, 锁外解码 (flash 读冻结窗口
 * 外 cache 恢复, 解码不冻结)。read 失败自动重开 fd 一次 (SPIFFS 写盘 GC
 * 后 fd 失效兜底)。scratch_static: 后台路径用常驻缓冲 (零分配);
 * 同步路径 (LVGL 定时器/init) malloc-per-call 用后 free — 两路隔离防
 * 跨核踩踏 (同步路径持锁读帧时后台任务可能在读同一暂存). */
static bool load_one_frame_off(pet_anim_t anim, int frame_idx, frame_t *f,
                               bool scratch_static)
{
    if (!pack_ensure_open()) return false;
    if (anim >= PET_ANIM_COUNT || frame_idx >= s_pack_count[anim]) return false;

    const uint8_t *e = &s_pack_tab[((size_t)s_pack_first[anim] + frame_idx) * 12];
    uint32_t off, size_flags;
    memcpy(&off, e + 4, 4);
    memcpy(&size_flags, e + 8, 4);
    bool raw = (size_flags & PACK_FLAG_RAW) != 0;
    uint32_t size = size_flags & PACK_SIZE_MASK;
    int codec = (size_flags >> PACK_CODEC_SHIFT) & 7;

    f->data = heap_caps_malloc(FRAME_SIZE, MALLOC_CAP_SPIRAM);
    if (!f->data) return false;

    /* 后台路径常驻暂存惰性分配 (anim_load 任务独占, 无需锁) */
    if (!raw && scratch_static) {
        if (!s_scratch_comp) {
            s_scratch_comp = heap_caps_malloc(FRAME_SIZE, MALLOC_CAP_SPIRAM);
            if (!s_scratch_comp) goto fail;
        }
        if (codec == CODEC_WEBP && !s_scratch_argb) {
            s_scratch_argb = heap_caps_malloc((size_t)FW * FH * 4, MALLOC_CAP_SPIRAM);
            if (!s_scratch_argb) goto fail;
        }
    }

    /* raw 帧直读目标缓冲; 压缩帧读入暂存 */
    uint8_t *buf = raw ? f->data
                       : (scratch_static ? s_scratch_comp
                                         : heap_caps_malloc(size, MALLOC_CAP_SPIRAM));
    if (!buf) goto fail;
    bool scratch_mine = !raw && !scratch_static;
    uint32_t rlen = raw ? FRAME_SIZE : size;

    if (lseek(s_pack_fd, s_pack_data_off + off, SEEK_SET) < 0 ||
        read(s_pack_fd, buf, rlen) != (ssize_t)rlen) {
        close(s_pack_fd);
        s_pack_fd = -1;
        if (!pack_ensure_open() ||
            lseek(s_pack_fd, s_pack_data_off + off, SEEK_SET) < 0 ||
            read(s_pack_fd, buf, rlen) != (ssize_t)rlen) {
            if (scratch_mine) heap_caps_free(buf);
            goto fail;
        }
    }

    if (!raw) {
        bool ok;
        if (codec == CODEC_WEBP) {
            uint8_t *argb = scratch_static ? s_scratch_argb
                                           : heap_caps_malloc((size_t)FW * FH * 4,
                                                              MALLOC_CAP_SPIRAM);
            if (!argb) {
                if (scratch_mine) heap_caps_free(buf);
                goto fail;
            }
            ok = anim_webp_decode(buf, size, f->data, argb);
            if (!scratch_static) heap_caps_free(argb);
        } else {
            ok = anim_rle_decode(buf, size, f->data, FW * FH);
        }
        if (scratch_mine) heap_caps_free(buf);
        if (!ok) goto fail;
    }

    f->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    f->dsc.header.w = FW;
    f->dsc.header.h = FH;
    f->dsc.header.stride = FW * 2;
    f->dsc.data = f->data;
    f->dsc.data_size = FRAME_SIZE;
    return true;
fail:
    heap_caps_free(f->data);
    f->data = NULL;
    return false;
}

static int load_frames_anim(frame_t *pool, int pool_max,
                            pet_anim_t anim, int max_frames,
                            bool scratch_static)
{
    int loaded = 0, n = (max_frames < pool_max) ? max_frames : pool_max;
    if (s_pack_count[anim] < n) n = s_pack_count[anim];
    for (int i = 0; i < n; i++) {
        if (!load_one_frame_off(anim, i, &pool[i], scratch_static)) break;
        loaded++;
    }
    return loaded;
}

static void unload_frames(frame_t *pool, int count)
{
    for (int i = 0; i < count; i++) {
        if (pool[i].data) {
            heap_caps_free(pool[i].data);
            pool[i].data = NULL;
        }
    }
}

/* 帧读取全部在 anim_load 后台任务, 定时器回调零 flash IO —
 * 切换一律异步备帧 (不在 LVGL 定时器内同步读帧) */

/* ══ 跨核共享状态 (LVGL 定时器 ↔ anim_load 后台任务) ══
 * LVGL 任务不绑核 (affinity=-1), 池所有权转移 (unload/发布/重置) 全部
 * 在 s_load_mux 临界区内; 后台任务锁外做 35ms flash 读, 锁内只转移指针,
 * LVGL 侧最坏等 ~1us。读侧安全: show_frame 的 pool[idx].data==NULL
 * 检查是兜底 — 任务先写帧再发 count (锁内赋值, 解锁自带 WMB), 读侧
 * 要么全旧 (NULL 跳过) 要么全新。unload 保留在临界区内 (heap free
 * 不持其它锁, 单次 ≤190us, 切换动画时一次) */
static portMUX_TYPE s_load_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint8_t s_dynamic_count = 0;     /* 动态池已加载帧数 */
static volatile pet_anim_t s_cached_anim = PET_ANIM_IDLE; /* 动态池缓存的动画 (切回idle不卸载) */
static volatile int s_bg_next = 0;               /* 下一个待加载帧索引 */
static volatile int s_bg_total = 0;              /* 总帧数上限 */
static frame_t s_load_buf;                       /* 后台加载暂存槽 */
/* 异步切换备帧: 目标动画首帧由后台任务读入 s_load_buf, 定时器回调零 flash IO */
static volatile int s_stage_anim = -1;           /* 备帧请求 (>=0=有请求) */
static volatile bool s_stage_ready = false;      /* 首帧已备好在 s_load_buf */

/* 异步切换: 目标动画帧不在动态池时, 由后台任务备好首帧后定时器再完成
 * 切换 — 定时器回调内零 flash IO */
static bool switch_to_anim(pet_anim_t anim)
{
    if (anim == s_loaded_anim) return true;
    if (anim == PET_ANIM_IDLE) {
        /* 不卸载 — 保留缓存, 同一动画再次触发零延迟; 后台续载继续 */
        s_loaded_anim = PET_ANIM_IDLE;
        return true;
    }
    /* 缓存命中: 帧还在动态池里 (锁内读一致性快照) */
    bool hit;
    portENTER_CRITICAL(&s_load_mux);
    hit = (anim == s_cached_anim && s_dynamic_count > 0);
    if (hit) {
        s_anims[anim].pool = s_dynamic;
        s_anims[anim].count = s_assets[anim].max_frames;
        if (s_anims[anim].count > s_dynamic_count) s_anims[anim].count = s_dynamic_count;
        s_bg_next = s_dynamic_count;
        s_bg_total = (s_assets[anim].max_frames < MAX_ANIM) ? s_assets[anim].max_frames : MAX_ANIM;
    }
    portEXIT_CRITICAL(&s_load_mux);
    if (hit) {
        ESP_LOGI(TAG, "anim %d 缓存命中 (%d 帧), 零延迟", anim, s_dynamic_count);
        s_loaded_anim = anim;
        return true;
    }

    /* 后台备帧已就绪 (首帧在 s_load_buf) — 锁内完成切换: 卸旧池 → 收首帧
     * → 发布续载. unload 在临界区内 (heap free 无嵌套锁, 单次 ≤190us) */
    portENTER_CRITICAL(&s_load_mux);
    if (s_stage_anim == anim && s_stage_ready) {
        unload_frames(s_dynamic, s_dynamic_count);
        s_dynamic[0] = s_load_buf;
        s_load_buf.data = NULL; /* 所有权 → s_dynamic[0] */
        s_dynamic_count = 1;
        s_cached_anim = anim;
        s_bg_next = 1;
        s_bg_total = (s_assets[anim].max_frames < MAX_ANIM) ? s_assets[anim].max_frames : MAX_ANIM;
        s_stage_anim = -1;
        s_stage_ready = false;
        s_anims[anim].pool = s_dynamic;
        s_anims[anim].count = 1;
        s_loaded_anim = anim;
        portEXIT_CRITICAL(&s_load_mux);
        ESP_LOGI(TAG, "anim %d (%s): 异步快速开播, 剩余 %d 帧后台加载",
                 anim, s_assets[anim].prefix, s_bg_total - 1);
        return true;
    }
    /* 首帧未就绪 — 请求后台备帧, 继续播当前动画 (零 IO) */
    if (s_stage_anim != anim) {
        if (s_stage_ready) unload_frames(&s_load_buf, 1); /* 丢弃旧备帧 */
        s_stage_anim = anim;
        s_stage_ready = false;
    }
    portEXIT_CRITICAL(&s_load_mux);
    return false;
}

/* 后台逐帧加载任务 — 独立于 LVGL 定时器, 锁外做 35ms flash 读,
 * 锁内只做指针转移 + count 发布。播放期间 (TTS) 暂停加载: 播放对
 * PSRAM 读时序敏感, 抢总线会卡音; 流式下载 (96KB/s 细流) 期间正常
 * 加载, 不冻结动画。 */
static void anim_load_task(void *arg)
{
    for (;;) {
        extern bool tts_client_is_playing(void);
        pet_anim_t anim;
        int idx;
        int stage;
        bool stage_ready;

        portENTER_CRITICAL(&s_load_mux);
        anim = s_cached_anim;
        idx = s_bg_next;
        stage = s_stage_anim;
        stage_ready = s_stage_ready;
        portEXIT_CRITICAL(&s_load_mux);

        /* ══ 阶段 1: 异步切换备帧 — 读目标动画首帧入 s_load_buf (定时器
         * 零 flash IO). 绕开 TTS 暂停: 单帧 32KB 读 + ~1ms 解码,
         * 不构成音频卡顿 ══ */
        if (stage >= 0 && stage < PET_ANIM_COUNT && !stage_ready) {
            if (load_one_frame_off((pet_anim_t)stage, 0, &s_load_buf, true)) {
                portENTER_CRITICAL(&s_load_mux);
                if (s_stage_anim == stage && !s_stage_ready) {
                    s_stage_ready = true;      /* 首帧备好, 等定时器切换 */
                } else {
                    unload_frames(&s_load_buf, 1); /* 请求被覆盖/已消费 */
                }
                portEXIT_CRITICAL(&s_load_mux);
            } else {
                /* 读失败 — 放弃备帧 (保持当前动画, 不阻塞 UI) */
                portENTER_CRITICAL(&s_load_mux);
                if (s_stage_anim == stage) s_stage_anim = -1;
                portEXIT_CRITICAL(&s_load_mux);
                ESP_LOGW(TAG, "异步备帧失败: anim %d 首帧不可读", stage);
            }
        }

        /* ══ 阶段 2: 正常续载 — 备帧期间暂停 (s_load_buf 被备帧独占);
         * 播放期 (TTS) 限载 6 帧防 PSRAM 撞顶 (分配失败 → SPI DMA 描述符
         * 被踩 → flush polling 死循环 → 任务看门狗重启), 播完后再补满;
         * 每帧 40ms 延时摊开, 不抢音频时序 ══ */
        bool tts_playing = tts_client_is_playing();
        int load_limit = tts_playing ? PLAY_FRAMES : MAX_ANIM;
        if (idx < s_bg_total && idx < load_limit && s_stage_anim < 0) {
            bool discard = false;
            /* 锁外 flash 读 + 解码 (malloc 也在锁外, 不挡 LVGL) */
            if (load_one_frame_off(anim, idx, &s_load_buf, true)) {
                portENTER_CRITICAL(&s_load_mux);
                /* 锁内校验: 期间动画被切换/池被重置 → 丢弃 */
                if (anim == s_cached_anim && idx == s_bg_next && idx < s_bg_total) {
                    s_dynamic[idx] = s_load_buf;
                    s_dynamic_count = idx + 1;
                    /* 扩展缓存动画的序列 — 新帧加入循环 */
                    if (s_cached_anim != PET_ANIM_IDLE &&
                        s_anims[s_cached_anim].count < idx + 1 &&
                        s_anims[s_cached_anim].count < s_assets[s_cached_anim].max_frames)
                        s_anims[s_cached_anim].count++;
                    s_bg_next = idx + 1;
                } else {
                    discard = true;
                }
                portEXIT_CRITICAL(&s_load_mux);
                if (discard) unload_frames(&s_load_buf, 1); /* 锁外释放 */
            } else {
                /* 读失败 — 停止续载 (截断到已加载帧) */
                portENTER_CRITICAL(&s_load_mux);
                if (anim == s_cached_anim && idx < s_bg_total)
                    s_bg_total = idx;
                portEXIT_CRITICAL(&s_load_mux);
            }
        }
        vTaskDelay(tts_playing ? pdMS_TO_TICKS(40) : pdMS_TO_TICKS(15));
    }
}

/* boot 预加载摸头 — 内部堆充足期 (起机早期) 把摸头 7 帧读进 PSRAM
 * 动态池, 首次播放缓存命中零延迟。播放由 s_loaded_anim 驱动, 预加载
 * 只填缓存 (s_cached_anim), 不干扰显示; 用户抢在完成前触发 →
 * 备帧路径先切换, 预加载锁内校验失败自动中止 */
static void anim_preload_task(void *arg)
{
    pet_anim_t anim = (pet_anim_t)(intptr_t)arg;
    int maxf = (s_assets[anim].max_frames < MAX_ANIM) ? s_assets[anim].max_frames : MAX_ANIM;
    bool done = false;
    for (int i = 0; i < maxf; i++) {
        frame_t fb;
        bool took = false;
        if (!load_one_frame_off(anim, i, &fb, true)) break;
        portENTER_CRITICAL(&s_load_mux);
        if (s_dynamic_count == i && s_stage_anim < 0) {
            s_dynamic[i] = fb;
            s_dynamic_count = i + 1;
            took = true;
        }
        portEXIT_CRITICAL(&s_load_mux);
        if (!took) { unload_frames(&fb, 1); break; } /* 被打断 — 用户已在播 */
        if (i == maxf - 1) done = true;
    }
    if (done) {
        bool pub = false;
        portENTER_CRITICAL(&s_load_mux);
        if (s_dynamic_count == maxf && s_stage_anim < 0) {
            s_cached_anim = anim;
            s_anims[anim].pool = s_dynamic;
            s_anims[anim].count = s_dynamic_count;
            s_bg_next = s_dynamic_count;
            s_bg_total = maxf;
            pub = true;
        }
        portEXIT_CRITICAL(&s_load_mux);
        /* 日志必须在临界区外 — 临界区内 printf 的 newlib 锁获取会被
         * xPortCanYield (查 INTLEVEL) 误判为 ISR 上下文 → abort;
         * 内存余量判断仅作诊断 */
        if (pub && heap_caps_get_free_size(MALLOC_CAP_INTERNAL) > 8192) {
            ESP_LOGI(TAG, "boot 预加载完成: anim %d (%d 帧), 首次播放零延迟",
                     anim, s_dynamic_count);
        }
    }
    vTaskDelete(NULL);
}

/* 预加载入口 — init 后调用 (main.c); 低优先级后台, 不阻塞 UI */
void pet_avatar_preload(void)
{
    if (s_dynamic_count != 0) return; /* 池已被使用 (重启/复用场景) */
    xTaskCreate(anim_preload_task, "anim_pre", 4096, (void *)PET_ANIM_PATHEAD,
                1, NULL);
}

/* ══════ 帧序表 (子循环定义来自各动画 manifest.c) ══════ */

static void build_seq(avatar_frame_t *dst, int count, uint16_t dur, uint8_t fps)
{
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

static void show_frame(uint8_t idx, frame_t *pool)
{
    if (pool && pool[idx].data) lv_image_set_src(s_img, &pool[idx].dsc);
}

static void do_play(pet_anim_t anim)
{
    if (!switch_to_anim(anim)) return; /* 异步备帧中 — 继续播当前动画 */
    s_current_anim = anim;
    s_seq_pos = 0;
    s_loop_count = 0;
    s_loop_active = false;
    s_loop_remaining = 0;
    anim_seq_t *seq = &s_anims[anim];
    /* ⚠ 动画资源缺失时 switch_to_anim 失败路径不设置 frames/count
     * (s_anims[anim] 全零), 空指针读 frames[0] 会崩 — 保护后仅无帧显示,
     * 定时器按 150ms 兜底, 画面停在上一帧, 资源齐后自动恢复 */
    uint16_t dur = 150;
    if (seq->count && seq->frames && seq->pool) {
        show_frame(seq->frames[0].frame_idx, seq->pool);
        dur = seq->frames[0].duration_ms;
        if (dur == 0)
            dur = s_override_ms ? s_override_ms
                                : 1000 / (seq->default_fps ? seq->default_fps : 15);
    }
    lv_timer_set_period(s_frame_timer, dur);
}

/* 线程安全入口: 只记录请求 */
void pet_avatar_play(pet_anim_t anim)
{
    if (anim < PET_ANIM_COUNT) {
        s_pending_anim = anim;
        s_pending_at = xTaskGetTickCount();
    }
}

void pet_avatar_play_fast(pet_anim_t anim)
{
    if (anim < PET_ANIM_COUNT) {
        s_pending_anim = anim;
        s_pending_at = 0; /* 时间戳0 → 最小延迟检查立即通过 */
    }
}

pet_anim_t pet_avatar_get_current(void) { return s_current_anim; }

/* 息屏降载: 暂停/恢复帧定时器 (power_manager_screen_off/on 调用) */
void pet_avatar_pause(void)
{
    if (s_frame_timer) lv_timer_pause(s_frame_timer);
}

void pet_avatar_resume(void)
{
    if (s_frame_timer) lv_timer_resume(s_frame_timer);
}

void pet_avatar_set_fps(uint8_t fps) { s_override_ms = fps ? 1000 / fps : 0; }

void pet_avatar_set_hold(bool on) { s_hold = on; }

void pet_avatar_set_sequence(const avatar_frame_t *frames, uint8_t count, uint8_t loop)
{
    s_anims[s_current_anim].frames = frames;
    s_anims[s_current_anim].count = count;
    s_anims[s_current_anim].loop = loop;
}

/* ── 帧推进 + 子循环/整组循环 (LVGL定时器上下文执行) ── */
static void frame_timer_cb(lv_timer_t *t)
{
    /* 异步切换完成检查: 后台任务已备好目标动画首帧 → 锁内完成切换
     * (零 flash IO). pending==-1 表示无更新请求, 完成已请求的切换 */
    int stage;
    bool stage_ready;
    portENTER_CRITICAL(&s_load_mux);
    stage = s_stage_anim;
    stage_ready = s_stage_ready;
    portEXIT_CRITICAL(&s_load_mux);
    if (stage_ready && stage >= 0 && stage < PET_ANIM_COUNT &&
        (s_pending_anim == stage || s_pending_anim == -1)) {
        if (s_pending_anim == stage) s_pending_anim = -1;
        do_play((pet_anim_t)stage);
        return;
    }

    /* 处理动画请求 — 立即开播 (流式 TTS 下载为匀速细流, 不构成 SPI 突发争用) */
    if (s_pending_anim >= 0 && s_pending_anim < PET_ANIM_COUNT) {
        pet_anim_t req = (pet_anim_t)s_pending_anim;
        s_pending_anim = -1;
        do_play(req);
        return;
    }

    anim_seq_t *seq = &s_anims[s_current_anim];
    if (seq->count < 2 || !seq->pool) return;

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

    /* 剩余帧由 anim_load 后台任务续载 — 定时器零 flash IO */

    /* ── 前进到下一帧 ── */
    s_seq_pos++;
    if (s_seq_pos >= seq->count) {
        s_seq_pos = 0;
        s_loop_count++;
        bool has_subloop = false;
        for (int i = 0; i < seq->count; i++)
            if (seq->frames[i].loop_back > 0) {
                has_subloop = true;
                break;
            }
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

esp_err_t pet_avatar_init(void)
{
    build_seq(s_seq_idle, 5, 300, 8);
    s_seq_idle[2].duration_ms = 800;
    s_seq_idle[3].duration_ms = 800;
    build_seq(s_seq_happy, 6, 300, 8);
    build_seq(s_seq_talk, 10, 200, 10);
    /* baoxiongshuohua: 帧4起点, 帧5回跳1步 → 4-5子循环 */
    s_seq_talk[5].loop_back = 1;
    build_seq(s_seq_sleep, 13, 500, 6);
    /* shuijiao: 帧4起点, 帧5回跳1步 → 4-5子循环 */
    s_seq_sleep[5].loop_back = 1;
    build_seq(s_seq_eating, 19, 200, 10);
    /* dunzhe: 帧0=1500ms, 帧1=300ms */
    build_seq(s_seq_squat, 2, 300, 8);
    s_seq_squat[0].duration_ms = 1500;
    build_seq(s_seq_blush, 14, 200, 10);
    /* motou: 300ms每帧, 帧3子循环起点, 帧6回跳3步 → 3-6子循环 */
    build_seq(s_seq_pathead, 7, 300, 10);
    s_seq_pathead[6].loop_back = 3;
    /* naotou: 帧0=1500ms, 帧1=300ms */
    build_seq(s_seq_scratch, 2, 300, 8);
    s_seq_scratch[0].duration_ms = 1500;
    build_seq(s_seq_pointself, 5, 300, 8);

    /* 打开动画包并读偏移表 — 之后播放零 open */
    if (!pack_ensure_open()) {
        ESP_LOGE(TAG, "anims.bin 打开失败 — 动画不可用");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "加载 idle (zhanli)...");
    s_idle_count = load_frames_anim(s_idle, IDLE_FRAMES, PET_ANIM_IDLE, 5, false);
    if (s_idle_count < 5) {
        ESP_LOGE(TAG, "idle 帧不足");
        return ESP_FAIL;
    }

    s_anims[PET_ANIM_IDLE] = (anim_seq_t){s_seq_idle, s_idle, 5, 1, 8};
    s_anims[PET_ANIM_HAPPY] = (anim_seq_t){s_seq_happy, NULL, 6, 1, 8};
    s_anims[PET_ANIM_SAD] = (anim_seq_t){s_seq_idle, s_idle, 5, 1, 8};
    s_anims[PET_ANIM_EXCITED] = (anim_seq_t){s_seq_talk, NULL, 10, 1, 10};
    s_anims[PET_ANIM_SLEEPY] = (anim_seq_t){s_seq_sleep, NULL, 13, 1, 6};
    s_anims[PET_ANIM_EATING] = (anim_seq_t){s_seq_eating, NULL, 19, 1, 10};
    s_anims[PET_ANIM_SURPRISED] = (anim_seq_t){s_seq_squat, NULL, 2, 1, 8};
    s_anims[PET_ANIM_BLUSH] = (anim_seq_t){s_seq_blush, NULL, 14, 1, 10};
    s_anims[PET_ANIM_PATHEAD] = (anim_seq_t){s_seq_pathead, NULL, 7, 1, 10};
    s_anims[PET_ANIM_SCRATCH] = (anim_seq_t){s_seq_scratch, NULL, 2, 1, 8};
    s_anims[PET_ANIM_POINTSELF] = (anim_seq_t){s_seq_pointself, NULL, 5, 1, 8};

    s_img = lv_image_create(lv_screen_active());
    lv_obj_set_size(s_img, FW, FH);
    lv_obj_set_pos(s_img, 0, 0);
    lv_obj_set_style_pad_all(s_img, 0, 0);
    lv_obj_set_style_border_width(s_img, 0, 0);

    s_current_anim = PET_ANIM_IDLE;
    s_loaded_anim = PET_ANIM_IDLE;
    s_seq_pos = 0;
    s_loop_count = 0;
    s_pending_anim = -1;
    show_frame(0, s_idle);

    s_frame_timer = lv_timer_create(frame_timer_cb, 300, NULL);
    lv_timer_set_repeat_count(s_frame_timer, -1);

    /* 后台帧加载任务 — 独立于 LVGL 定时器; 内部 RAM 栈 (高频访问),
     * 低优先级: 只在 LVGL 空闲时续载, 播放期自暂停 (TTS 检查)。
     * 栈 4096: load_one_frame_off 的 ESP_LOGI + malloc + VFS read
     * 链栈深较大, 2048 不够 */
    if (xTaskCreate(anim_load_task, "anim_load", 4096, NULL, 1, NULL) != pdPASS)
        ESP_LOGE(TAG, "anim_load 任务创建失败 — 动画只能播首帧");

    ESP_LOGI(TAG, "就绪: idle=%d帧 动态池=%d帧 默认%d轮回idle",
             s_idle_count, MAX_ANIM, s_play_loops);
    return ESP_OK;
}
