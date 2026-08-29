"""
FastAPI application entry point.
Creates the app, registers middleware and routers, sets up lifespan events.
"""
import asyncio
import os
from contextlib import asynccontextmanager
from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware
from fastapi.staticfiles import StaticFiles
from app.config import get_settings
from app.api.v1.router import router as v1_router
from app.api.ws.router import router as ws_router

settings = get_settings()

# 单用户部署: 老库不兼容, 首次部署前直接重建数据库 (drop 后由 upgrade head
# 建全量结构)。alembic 同步 API 放 executor, 不阻塞启动事件循环;
# 迁移失败 = 表结构不对 = 中止启动 (不静默, 后续请求全会崩)
BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ALEMBIC_INI = os.path.join(BASE_DIR, "alembic.ini")


def _run_migrations() -> None:
    from alembic import command
    from alembic.config import Config
    command.upgrade(Config(ALEMBIC_INI), "head")


@asynccontextmanager
async def lifespan(app: FastAPI):
    """Application startup / shutdown events."""
    import logging
    logging.getLogger("startup").info("alembic upgrade head…")
    await asyncio.get_running_loop().run_in_executor(None, _run_migrations)
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


