/** @file chat_bubble.h @brief 底部半透明对话框 (LLM 回复) */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 创建对话框 (叠加在宠物上方, 底部) */
void chat_bubble_init(void);

/** 显示文本, auto_hide_ms 后自动隐藏 (0=不自动隐藏) */
void chat_bubble_show(const char *text, uint32_t auto_hide_ms);

/** 同步显示: 等 TTS 播放开始后逐字显示 (与语音同步) */
void chat_bubble_show_sync(const char *text);

/** 立即隐藏 */
void chat_bubble_hide(void);

#ifdef __cplusplus
}
#endif
