"""Nightly diary generation tasks."""
from app.core.celery_app import celery_app

@celery_app.task
def generate_daily_diaries():
    pass


