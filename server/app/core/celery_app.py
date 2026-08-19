"""
Celery application configuration for async tasks.
"""

from celery import Celery
from app.config import get_settings

settings = get_settings()

celery_app = Celery(
    "virtualpet",
    broker=settings.redis_url,
    backend=settings.redis_url,
    include=[
        "app.tasks.diary_tasks",
        "app.tasks.habit_tasks",
        "app.tasks.cleanup_tasks",
    ],
)

celery_app.conf.update(
    task_serializer="json",
    accept_content=["json"],
    result_serializer="json",
    timezone="Asia/Shanghai",
    enable_utc=True,
    beat_schedule={
        "generate-daily-diaries": {
            "task": "app.tasks.diary_tasks.generate_daily_diaries",
            "schedule": 3600.0,  # Every hour, check if any device needs diary generation
        },
        "analyze-user-habits": {
            "task": "app.tasks.habit_tasks.analyze_user_habits",
            "schedule": 86400.0,  # Daily at midnight
        },
        "cleanup-expired-sessions": {
            "task": "app.tasks.cleanup_tasks.cleanup_expired_sessions",
            "schedule": 3600.0,
        },
    },
)


