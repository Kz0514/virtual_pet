"""User habit analysis batch job."""
from app.core.celery_app import celery_app

@celery_app.task
def analyze_user_habits():
    pass


