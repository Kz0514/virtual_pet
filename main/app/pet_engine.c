/**
 * @file pet_engine.c
 * @brief 萝莉丝宠物状态机 — 等级/经验/心情/饥饿
 *
 * 等级: exp 累积 → 升级, exp<0 → 降级, 无等级上限
 * 心情: 0-100, 不主动增加, >=65 时降至 64 (活跃时段大幅减速)
 * 饥饿: 0-100, 归零时扣减 exp
 * 经验: 基础 exp × (1 + mood/100) 取整
 */
#include "pet_engine.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "tts_client.h"
#include "memory_store.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <math.h>

static const char *TAG = "pet";

/* ── 内部状态 ── */
static pet_state_t s_pet = {
    .level = 1, .exp = 0, .exp_to_next = 100,
    .mood = 60, .hunger = 80, .energy = 80,
    .personality = 0,
    .face = PET_FACE_NEUTRAL, .age_seconds = 0,
};
static uint32_t s_tick_acc_ms = 0;
static uint32_t s_last_interact_tick = 0;   /* 最后互动的时间戳 */
static pet_face_cb_t s_face_cb = NULL;

/* ── 等级计算 ── */

/* 计算升至下一级所需经验: round(100 * 1.2^(level-1) / 10) * 10 */
static uint32_t calc_exp_to_next(uint8_t level) {
    if (level < 1) level = 1;
    float xp = 100.0f;
    for (uint8_t i = 1; i < level; i++) xp *= 1.2f;
    return ((uint32_t)(xp + 5.0f) / 10) * 10;  /* round to nearest 10 */
}

static void check_level_up(void) {
    while (s_pet.exp >= s_pet.exp_to_next) {
        s_pet.exp -= s_pet.exp_to_next;
        s_pet.level++;
        s_pet.exp_to_next = calc_exp_to_next(s_pet.level);
        ESP_LOGI(TAG, "🎉 升级! Lv.%u (下级需 %lu exp)",
                 s_pet.level, s_pet.exp_to_next);
    }
}

static void check_level_down(void) {
    while ((int32_t)s_pet.exp < 0) {
        if (s_pet.level <= 1) {
            s_pet.exp = 0;  /* Lv.1 最低 */
            return;
        }
        s_pet.level--;
        s_pet.exp_to_next = calc_exp_to_next(s_pet.level);
        s_pet.exp += s_pet.exp_to_next;  /* 回退 */
        ESP_LOGI(TAG, "⬇ 降级! Lv.%u (下级需 %lu exp)",
                 s_pet.level, s_pet.exp_to_next);
    }
}

/* ── 表情判定 ── */
static pet_face_t compute_face(void) {
    if (s_pet.energy < 15)  return PET_FACE_SLEEPY;
    if (s_pet.mood >= 90)   return PET_FACE_VERY_HAPPY;
    if (s_pet.mood >= 65)   return PET_FACE_HAPPY;
    if (s_pet.mood <= 9)    return PET_FACE_SAD;
    return PET_FACE_NEUTRAL;
}

static void set_face(pet_face_t f) {
    if (s_pet.face != f) {
        s_pet.face = f;
        if (s_face_cb) s_face_cb(f);
    }
}

/* ── 存档 (/data/pet.json — 不再写 NVS, 减轻 24KB NVS 磨损) ── */
#define SAVE_FILE "/data/pet.json"
#define SAVE_TMP  "/data/pet.tmp"
#define NVS_NS    "pet"      /* 仅用于旧存档一次性迁移 */
#define NVS_KEY   "state"

void pet_engine_save(void) {
    /* TTS 播放期间跳过 — flash 写冻结双核卡音频, 下个节拍再写.
     * 低电量不跳过: 宠物状态是关键数据且原子写保证旧存档安全 */
    if (tts_client_is_playing()) return;
    /* 原子写: 先写 pet.tmp 再 rename — 断电最坏丢新存档, 不毁旧存档 */
    FILE *f = fopen(SAVE_TMP, "w");
    if (!f) { ESP_LOGW(TAG, "打开存档文件失败"); return; }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddNumberToObject(root, "level", s_pet.level);
    cJSON_AddNumberToObject(root, "exp", (double)s_pet.exp);
    cJSON_AddNumberToObject(root, "exp_to_next", (double)s_pet.exp_to_next);
    cJSON_AddNumberToObject(root, "mood", s_pet.mood);
    cJSON_AddNumberToObject(root, "hunger", s_pet.hunger);
    cJSON_AddNumberToObject(root, "energy", s_pet.energy);
    cJSON_AddNumberToObject(root, "age_seconds", (double)s_pet.age_seconds);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json) {
        fwrite(json, 1, strlen(json), f);
    }
    fclose(f);
    remove(SAVE_FILE);
    if (rename(SAVE_TMP, SAVE_FILE) != 0) {
        /* rename 失败兜底: 直写目标文件 */
        ESP_LOGW(TAG, "rename 失败 (errno=%d), 直写兜底", errno);
        FILE *g = fopen(SAVE_FILE, "w");
        if (g) {
            if (json) fwrite(json, 1, strlen(json), g);
            fclose(g);
        }
    }
    if (json) cJSON_free(json);
}

static void load_from_file(void) {
    FILE *f = fopen(SAVE_FILE, "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 4096) { fclose(f); return; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) { ESP_LOGW(TAG, "存档 JSON 解析失败, 用默认值"); return; }
    cJSON *j;
    if ((j = cJSON_GetObjectItem(root, "level")) && cJSON_IsNumber(j))
        s_pet.level = j->valueint >= 1 ? (uint8_t)j->valueint : 1;
    if ((j = cJSON_GetObjectItem(root, "exp")) && cJSON_IsNumber(j))
        s_pet.exp = j->valueint >= 0 ? (uint32_t)j->valueint : 0;
    if ((j = cJSON_GetObjectItem(root, "mood")) && cJSON_IsNumber(j))
        s_pet.mood = (uint8_t)(j->valueint > 100 ? 100 : (j->valueint < 0 ? 0 : j->valueint));
    if ((j = cJSON_GetObjectItem(root, "hunger")) && cJSON_IsNumber(j))
        s_pet.hunger = (uint8_t)(j->valueint > 100 ? 100 : (j->valueint < 0 ? 0 : j->valueint));
    if ((j = cJSON_GetObjectItem(root, "energy")) && cJSON_IsNumber(j))
        s_pet.energy = (uint8_t)(j->valueint > 100 ? 100 : (j->valueint < 0 ? 0 : j->valueint));
    if ((j = cJSON_GetObjectItem(root, "age_seconds")) && cJSON_IsNumber(j))
        s_pet.age_seconds = j->valueint >= 0 ? (uint32_t)j->valueint : 0;
    s_pet.exp_to_next = calc_exp_to_next(s_pet.level);  /* 公式为准, 不信任存档 */
    cJSON_Delete(root);
}

/* 旧 NVS 存档 → /data/pet.json 一次性迁移 */
static void migrate_from_nvs(void) {
    nvs_handle_t nvs;
    pet_state_t saved;
    size_t len = sizeof(saved);
    if (nvs_open(NVS_NS, NVS_READONLY, &nvs) != ESP_OK) return;
    if (nvs_get_blob(nvs, NVS_KEY, &saved, &len) == ESP_OK && len == sizeof(saved)) {
        s_pet = saved;
        s_pet.exp_to_next = calc_exp_to_next(s_pet.level);
        nvs_close(nvs);
        ESP_LOGI(TAG, "NVS 旧存档迁移到 %s", SAVE_FILE);
        pet_engine_save();
        nvs_handle_t rw;
        if (nvs_open(NVS_NS, NVS_READWRITE, &rw) == ESP_OK) {
            nvs_erase_key(rw, NVS_KEY);
            nvs_commit(rw);
            nvs_close(rw);
        }
    } else {
        nvs_close(nvs);
    }
}

/* ── 心情标签 ── */
const char* pet_get_mood_label(void) {
    if (s_pet.mood >= 90) return "兴奋";
    if (s_pet.mood >= 80) return "开心";
    if (s_pet.mood >= 65) return "开朗";
    if (s_pet.mood >= 40) return "平静";
    if (s_pet.mood >= 20) return "无聊";
    if (s_pet.mood >= 10) return "寂寞";
    if (s_pet.mood >= 5)  return "有点难过";
    return "难过";
}

/* ── 常用时段 ── */
bool pet_is_active_hours(void) {
    /* 预留: 后续由 time_manager / 用户行为推断 */
    return true;
}

/* ════════════════════════════════════════════════════════════════ */

esp_err_t pet_engine_init(void) {
    /* 1) /data/pet.json 优先 (data 分区由 sensor_logger_init 先行挂载);
     * 主文件缺失但 .tmp 存在 → 上次 rename 被打断, 用 .tmp 恢复 */
    bool loaded = false;
    {
        FILE *probe = fopen(SAVE_FILE, "r");
        if (!probe) {
            FILE *t = fopen(SAVE_TMP, "r");
            if (t) { fclose(t); rename(SAVE_TMP, SAVE_FILE); }
            probe = fopen(SAVE_FILE, "r");
        }
        if (probe) { fclose(probe); loaded = true; load_from_file(); }
    }
    /* 2) 无文件 → 旧 NVS 存档一次性迁移 */
    if (!loaded) migrate_from_nvs();
    ESP_LOGI(TAG, "存档恢复: Lv.%u exp=%lu/%lu mood=%d hunger=%d",
             s_pet.level, s_pet.exp, s_pet.exp_to_next,
             s_pet.mood, s_pet.hunger);

    s_last_interact_tick = xTaskGetTickCount();
    s_pet.face = compute_face();
    if (s_face_cb) s_face_cb(s_pet.face);
    ESP_LOGI(TAG, "状态机就绪 face=%d", s_pet.face);
    return ESP_OK;
}

/* ════════════════════════════════════════════════════════════════ */

void pet_engine_tick(uint32_t tick_ms) {
    s_tick_acc_ms += tick_ms;
    s_pet.age_seconds += tick_ms / 1000;

    if (s_tick_acc_ms < 2000) return;
    uint32_t sec = s_tick_acc_ms / 1000;
    s_tick_acc_ms %= 1000;

    bool active = pet_is_active_hours();
    uint32_t idle_sec = (xTaskGetTickCount() - s_last_interact_tick)
                        * portTICK_PERIOD_MS / 1000;

    /* ── 心情衰减 ── */
    if (s_pet.mood >= 65) {
        /* 心情越高降速越快: 90→降3点/10s, 80→降2点, 70→降1点 */
        int rate = ((int)s_pet.mood - 64) / 10;  /* 0~3 */
        if (rate < 1) rate = 1;  /* 最低 1 点/10s */
        if (active) rate = (rate + 1) / 2;  /* 活跃时段减半，最低仍为 1 */
        if (rate < 1) rate = 1;
        static uint8_t mood_decay_timer = 0;
        mood_decay_timer += sec;
        if (mood_decay_timer >= (uint8_t)(10 / rate)) {
            mood_decay_timer = 0;
            s_pet.mood--;
        }
    }

    /* 长期无互动: 每 300s (5分钟) 扣 1 点心情 */
    static uint32_t idle_penalty_timer = 0;
    idle_penalty_timer += sec;
    if (idle_penalty_timer >= 300) {
        idle_penalty_timer = 0;
        if (idle_sec > 300 && s_pet.mood > 0) {
            int penalty = active ? 1 : 2;
            s_pet.mood = s_pet.mood > penalty ? s_pet.mood - penalty : 0;
        }
    }

    /* ── 饥饿衰减 ── */
    {
        /* 基础: 1点/60s, 活跃时段: 1点/30s */
        int hunger_rate = active ? 30 : 60;
        static uint8_t hunger_timer = 0;
        hunger_timer += sec;
        if (hunger_timer >= hunger_rate) {
            hunger_timer = 0;
            if (s_pet.hunger > 0) s_pet.hunger--;
        }
    }

    /* ── 饥饿归零惩罚 ── */
    if (s_pet.hunger == 0) {
        static uint8_t starve_timer = 0;
        starve_timer += sec;
        if (starve_timer >= 10) {  /* 每秒扣 1 exp */
            starve_timer = 0;
            pet_remove_exp(1);
        }
    }

    /* ── 精力衰减 ── */
    {
        static uint16_t energy_timer = 0;
        energy_timer += sec;
        if (energy_timer >= 200) {  /* 1点/200s */
            energy_timer = 0;
            if (s_pet.energy > 0) s_pet.energy--;
        }
    }

    set_face(compute_face());

    static uint16_t save_timer = 0;
    save_timer += sec;
    if (save_timer >= 300) { save_timer = 0; pet_engine_save(); }  /* 5min 存档, 减磨损 */
}

/* ════════════════════════════════════════════════════════════════ */

void pet_engine_trigger(pet_event_t event) {
    int mood_d = 0, energy_d = 0, base_exp = 0;
    pet_face_t tmp_face = PET_FACE_NEUTRAL;

    s_last_interact_tick = xTaskGetTickCount();

    switch (event) {
    case PET_EVENT_TOUCH:
        mood_d = 3; base_exp = 5; tmp_face = PET_FACE_HAPPY;
        break;
    case PET_EVENT_DOUBLE_TOUCH:
        mood_d = 6; base_exp = 10; tmp_face = PET_FACE_VERY_HAPPY;
        break;
    case PET_EVENT_SHAKE:
        mood_d = 2; base_exp = 3; tmp_face = PET_FACE_EXCITED;
        break;
    case PET_EVENT_HARD_SHAKE:
        mood_d = 4; base_exp = 5; tmp_face = PET_FACE_EXCITED;
        break;
    case PET_EVENT_VOICE:
        /* mood/exp 由 LLM 通过 pet_process_chat 决定 */
        mood_d = 0; base_exp = 0;
        tmp_face = PET_FACE_HAPPY;
        break;
    case PET_EVENT_FEED:
        mood_d = 5; energy_d = 20; base_exp = 15;
        s_pet.hunger = s_pet.hunger + 30 > 100 ? 100 : s_pet.hunger + 30;
        tmp_face = PET_FACE_EATING;
        break;
    case PET_EVENT_PRAISE:
        mood_d = 8; base_exp = 12; tmp_face = PET_FACE_VERY_HAPPY;
        break;
    case PET_EVENT_SCOLD:
        mood_d = -12; base_exp = -5; tmp_face = PET_FACE_SAD;
        break;
    case PET_EVENT_LONG_IDLE:
        mood_d = -5; base_exp = 0; tmp_face = PET_FACE_SLEEPY;
        break;
    default: break;
    }

    /* 应用心情 */
    pet_add_mood(mood_d);

    /* 应用经验 */
    if (base_exp > 0) pet_add_exp(base_exp);
    else if (base_exp < 0) pet_remove_exp(-base_exp);

    /* 精力 */
    s_pet.energy = (int)s_pet.energy + energy_d > 100 ? 100
                 : (int)s_pet.energy + energy_d < 0 ? 0
                 : s_pet.energy + energy_d;

    if (tmp_face != PET_FACE_NEUTRAL) set_face(tmp_face);

    ESP_LOGI(TAG, "事件 %d → mood%+d exp%+d energy%+d",
             event, mood_d, base_exp, energy_d);
}

/* ════════════════════════════════════════════════════════════════ */

void pet_add_exp(int32_t base_exp) {
    if (base_exp <= 0) return;
    /* 心情倍率: (1 + mood/100) × base_exp */
    int32_t final_exp = (base_exp * (100 + s_pet.mood)) / 100;
    if (final_exp < 1) final_exp = 1;
    s_pet.exp += final_exp;
    ESP_LOGI(TAG, "exp+%ld (base=%ld × %.2f)", final_exp, base_exp,
             1.0f + s_pet.mood / 100.0f);
    check_level_up();
    s_last_interact_tick = xTaskGetTickCount();
}

void pet_remove_exp(int32_t exp) {
    if (exp <= 0) return;
    s_pet.exp -= exp;
    ESP_LOGI(TAG, "exp-%ld", exp);
    check_level_down();
}

void pet_add_mood(int8_t delta) {
    int16_t v = (int16_t)s_pet.mood + delta;
    if (v > 100) v = 100;
    if (v < 0)   v = 0;
    s_pet.mood = (uint8_t)v;
}

void pet_add_hunger(int8_t delta) {
    int16_t v = (int16_t)s_pet.hunger + delta;
    if (v > 100) v = 100;
    if (v < 0)   v = 0;
    s_pet.hunger = (uint8_t)v;
}

void pet_process_chat(int8_t mood_d, int8_t exp_d) {
    /* 彩蛋检测: 特别满意时 +20 心情 +99 经验 */
    bool easter = (mood_d >= 10 && exp_d >= 5);
    if (easter) {
        ESP_LOGI(TAG, "🌟 彩蛋触发! mood+20 exp+99");
        pet_add_mood(20);
        pet_add_exp(99);
    } else {
        pet_add_mood(mood_d);
        if (exp_d > 0) pet_add_exp(exp_d);
        else if (exp_d < 0) pet_remove_exp(-exp_d);
    }
    s_last_interact_tick = xTaskGetTickCount();
}

uint32_t pet_get_exp_to_next(void) { return s_pet.exp_to_next; }

pet_state_t pet_engine_get_state(void) { return s_pet; }

void pet_engine_on_face_change(pet_face_cb_t cb) { s_face_cb = cb; }
