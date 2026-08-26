"""
FastAPI application entry point.
Creates the app, registers middleware and routers, sets up lifespan events.
"""
import os
from contextlib import asynccontextmanager
from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware
from fastapi.staticfiles import StaticFiles
from app.config import get_settings
from app.api.v1.router import router as v1_router
from app.api.ws.router import router as ws_router

settings = get_settings()


@asynccontextmanager
async def lifespan(app: FastAPI):
    """Application startup / shutdown events."""
    import logging
    from sqlalchemy import text
    from app.core.database import engine, Base
    async with engine.begin() as conn:
        await conn.run_sync(Base.metadata.create_all)
        # create_all 只建新表不补旧表列 — 幂等补齐老库缺失列 (如 pets.owner_name)
        try:
            await conn.execute(
                text("ALTER TABLE pets ADD COLUMN owner_name VARCHAR(32) DEFAULT '主人'")
            )
            logging.getLogger("startup").warning("迁移: pets.owner_name 列已新增")
        except Exception as e:  # 已存在 (Duplicate column) 或其它
            if "Duplicate column" not in str(e) and "already exists" not in str(e):
                logging.getLogger("startup").error(f"迁移检查异常: {e}")
    yield


app = FastAPI(
    title=settings.app_name,
    version=settings.app_version,
    docs_url="/docs",
    redoc_url="/redoc",
    lifespan=lifespan,
)

# CORS (allow ESP32 provisioning requests from any origin)
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# Uploads directory for ASR audio files (DashScope needs public URL)
UPLOAD_DIR = os.path.join(os.path.dirname(__file__), "..", "uploads")
os.makedirs(UPLOAD_DIR, exist_ok=True)
app.mount("/uploads", StaticFiles(directory=UPLOAD_DIR), name="uploads")

# Doodles directory for diary drawings (Qwen-Image 涂鸦)
DOODLE_DIR = os.path.join(os.path.dirname(__file__), "..", "doodles")
os.makedirs(DOODLE_DIR, exist_ok=True)
app.mount("/doodles", StaticFiles(directory=DOODLE_DIR), name="doodles")

# Register API routers
app.include_router(v1_router, prefix="/api/v1")
app.include_router(ws_router, prefix="/ws")


@app.get("/")
async def root():
    return {"name": settings.app_name, "version": settings.app_version, "status": "ok"}


@app.get("/health")
async def health():
    return {"status": "healthy"}


