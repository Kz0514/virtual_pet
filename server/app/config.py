"""
Virtualpet Server Configuration
Priority: env vars > api.json > defaults
"""
import json, os
from pydantic_settings import BaseSettings
from functools import lru_cache

# ── API Keys from api.json ──
_api_file = os.path.join(os.path.dirname(__file__), "..", "api.json")
_api_keys: dict = {}
try:
    with open(_api_file, "r", encoding="utf-8") as f:
        _api_keys = json.load(f)
except (FileNotFoundError, json.JSONDecodeError):
    pass

_deepseek = _api_keys.get("deepseek", {})
_dashscope = _api_keys.get("dashscope", {})
_tencent = _api_keys.get("tencent", {})


class Settings(BaseSettings):
    # ── Application ──
    app_name: str = "Virtualpet Server"
    app_version: str = "0.1.0"
    debug: bool = False
    log_level: str = "INFO"

    # ── Database ──
    database_url: str = (
        "postgresql+asyncpg://virtualpet:virtualpet@localhost:5432/virtualpet"
    )
    database_pool_size: int = 10
    database_max_overflow: int = 20

    # ── Redis ──
    redis_url: str = "redis://localhost:6379/0"

    # ── Security ──
    secret_key: str = "dev-secret-change-in-production-must-be-64-chars-min"
    jwt_algorithm: str = "HS256"
    jwt_expire_minutes: int = 10080
    device_token_refresh_days: int = 7

    # ── DeepSeek (from api.json or env var) ──
    deepseek_api_key: str = _deepseek.get("api_key", "")
    deepseek_base_url: str = _deepseek.get("base_url", "https://api.deepseek.com/v1")
    deepseek_model: str = _deepseek.get("model", "deepseek-chat")

    # ── DashScope (ASR + TTS) ──
    dashscope_api_key: str = _dashscope.get("api_key", "")
    dashscope_base_url: str = _dashscope.get("base_url", "https://dashscope.aliyuncs.com/api/v1")

    # ── Tencent Map (from api.json or env var) ──
    tencent_api_key: str = _tencent.get("api_key", "")

    # ── TTS ──
    dashscope_voice_id: str = "qwen-audio-3.0-tts-flash"  # DASHSCOPE_VOICE_ID 环境变量可覆盖
    tts_default_voice: str = "zh_female_cute"
    tts_default_speed: float = 1.0

    # ── Conversation ──
    conversation_window_size: int = 20
    conversation_cache_ttl_sec: int = 3600

    # ── Device Memory (设备端对话记忆) ──
    memory_max_bytes: int = 102400          # 上限 100KB, 超限触发 LLM 压缩
    memory_recent_keep_turns: int = 3       # 压缩时保留最近 N 轮原文
    memory_summary_max_tokens: int = 4000   # 压缩 LLM 输出上限

    # ── Diary ──
    diary_min_interactions: int = 10
    diary_generate_hour: int = 23

    # ── OTA ──
    ota_bucket_url: str = ""

    model_config = {"env_file": ".env", "env_file_encoding": "utf-8", "extra": "ignore"}


@lru_cache()
def get_settings() -> Settings:
    return Settings()


