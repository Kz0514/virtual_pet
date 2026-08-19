"""
API v1 route aggregation.
"""

from fastapi import APIRouter
from app.api.v1 import auth, chat, diary, ota, weather, settings, asr, assets, tts, location

router = APIRouter()

router.include_router(auth.router, prefix="/devices", tags=["auth"])
router.include_router(chat.router, prefix="/chat", tags=["chat"])
router.include_router(diary.router, prefix="/diary", tags=["diary"])
router.include_router(ota.router, prefix="/ota", tags=["ota"])
router.include_router(weather.router, prefix="/weather", tags=["weather"])
router.include_router(settings.router, prefix="/settings", tags=["settings"])
router.include_router(asr.router, prefix="/asr", tags=["asr"])
router.include_router(assets.router, prefix="/assets", tags=["assets"])
router.include_router(tts.router, prefix="/tts", tags=["tts"])
router.include_router(location.router, prefix="/location", tags=["location"])


