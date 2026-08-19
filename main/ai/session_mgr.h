/** @file session_mgr.h @brief 连续会话模式 — VAD 只录人声段, 半双工多轮语音对话 */
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化会话管理器 (常驻任务 + PSRAM 缓冲). 需 es8311/tts_client/ws_client 已就绪 */
void session_mgr_init(void);

/** 进入会话; 会话进行中再调用无效果 (打断机制已按用户要求移除) */
void session_mgr_enter(void);

/** 会话是否进行中 */
bool session_mgr_is_active(void);

/** 是否在等回复/播放中 (此时敲击 = 打断语义) */
bool session_mgr_is_playing(void);

/** 麦克风是否被会话占用 (噪音检测器需避让) */
bool session_mgr_is_capturing(void);

/** 打印 sess 任务栈水位 (内存探查用) */
void session_mgr_log_stack(void);

#ifdef __cplusplus
}
#endif
