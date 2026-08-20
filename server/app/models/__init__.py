"""
SQLAlchemy ORM models — Virtualpet database schema.
"""

import uuid
from datetime import datetime, timezone
from sqlalchemy import (
    Column, String, Integer, SmallInteger, BigInteger, Boolean,
    Float, Text, Date, DateTime, ForeignKey, UniqueConstraint, Index, JSON,
)
from sqlalchemy.dialects.postgresql import UUID, JSONB
from sqlalchemy.orm import relationship
from app.core.database import Base


def _uuid() -> str:
    return str(uuid.uuid4())


def _now() -> datetime:
    return datetime.now(timezone.utc)


class Device(Base):
    __tablename__ = "devices"

    id               = Column(UUID, primary_key=True, default=_uuid)
    mac_address      = Column(String(17), unique=True, nullable=False, index=True)
    device_name      = Column(String(64), default="萝莉丝")
    firmware_version = Column(String(16), default="1.0.0")
    hardware_rev     = Column(String(8), default="A")
    certificate_fp   = Column(String(256), nullable=True)
    is_active        = Column(Boolean, default=True)
    created_at       = Column(DateTime(timezone=True), default=_now)
    last_seen_at     = Column(DateTime(timezone=True), default=_now, onupdate=_now)

    pet              = relationship("Pet", back_populates="device", uselist=False,
                                     cascade="all, delete-orphan")
    conversations    = relationship("Conversation", back_populates="device",
                                     cascade="all, delete-orphan")
    sensor_events    = relationship("SensorEvent", back_populates="device",
                                     cascade="all, delete-orphan")
    diary_entries    = relationship("DiaryEntry", back_populates="device",
                                     cascade="all, delete-orphan")
    memory           = relationship("DeviceMemory", back_populates="device",
                                     uselist=False, cascade="all, delete-orphan")


class Pet(Base):
    __tablename__ = "pets"

    id           = Column(UUID, primary_key=True, default=_uuid)
    device_id    = Column(UUID, ForeignKey("devices.id"), unique=True, nullable=False)
    name         = Column(String(32), default="萝莉丝")
    mood         = Column(SmallInteger, default=50)   # 0–100
    energy       = Column(SmallInteger, default=80)   # 0–100
    intimacy     = Column(SmallInteger, default=0)    # 0–100
    level        = Column(SmallInteger, default=1)
    personality  = Column(JSONB, default=lambda: {
        "traits": ["活泼", "善良"],
        "speaking_style": "可爱",
    })
    state        = Column(JSONB, default=dict)
    updated_at   = Column(DateTime(timezone=True), default=_now, onupdate=_now)

    device       = relationship("Device", back_populates="pet")


class Conversation(Base):
    __tablename__ = "conversations"

    id           = Column(BigInteger, primary_key=True, autoincrement=True)
    device_id    = Column(UUID, ForeignKey("devices.id"), nullable=False, index=True)
    role         = Column(String(16), nullable=False)  # user / assistant / system / sensor
    content      = Column(Text, nullable=False)
    content_type = Column(String(32), default="text")  # text / audio_transcript / sensor_event
    mood_at_time = Column(String(32), nullable=True)
    tokens_used  = Column(Integer, nullable=True)
    extra_data   = Column(JSONB, default=dict)  # SQLAlchemy reserved 'metadata'
    created_at   = Column(DateTime(timezone=True), default=_now)

    device       = relationship("Device", back_populates="conversations")

    __table_args__ = (
        Index("idx_conv_device_time", "device_id", created_at.desc()),
    )


class SensorEvent(Base):
    __tablename__ = "sensor_events"

    id           = Column(BigInteger, primary_key=True, autoincrement=True)
    device_id    = Column(UUID, ForeignKey("devices.id"), nullable=False, index=True)
    event_type   = Column(String(32), nullable=False)  # shake / petting / light_change / noise
    event_data   = Column(JSONB, default=dict)
    created_at   = Column(DateTime(timezone=True), default=_now)

    device       = relationship("Device", back_populates="sensor_events")


class DiaryEntry(Base):
    __tablename__ = "diary_entries"

    id                = Column(UUID, primary_key=True, default=_uuid)
    device_id         = Column(UUID, ForeignKey("devices.id"), nullable=False)
    entry_date        = Column(Date, nullable=False)
    title             = Column(String(128), nullable=True)
    content           = Column(Text, nullable=False)
    doodle_url        = Column(String(512), nullable=True)
    interaction_count = Column(Integer, default=0)
    mood_summary      = Column(String(32), nullable=True)
    created_at        = Column(DateTime(timezone=True), default=_now)

    device            = relationship("Device", back_populates="diary_entries")

    __table_args__ = (
        UniqueConstraint("device_id", "entry_date"),
    )


class DeviceMemory(Base):
    """设备端记忆压缩结果 (重要历史) — 服务端落库, 供日记生成作素材.

    每次设备端记忆超限压缩后 upsert (device_id 主键, 覆盖为最新摘要);
    未压缩过的设备无此行 — 此时近三天对话已由 conversations 表覆盖。
    """
    __tablename__ = "device_memories"

    device_id  = Column(UUID, ForeignKey("devices.id"), primary_key=True)
    content    = Column(Text, nullable=False)
    updated_at = Column(DateTime(timezone=True), default=_now, onupdate=_now)

    device     = relationship("Device", back_populates="memory")


class Firmware(Base):
    __tablename__ = "firmwares"

    id               = Column(Integer, primary_key=True, autoincrement=True)
    version          = Column(String(16), unique=True, nullable=False)
    changelog        = Column(Text, nullable=True)
    file_url         = Column(String(512), nullable=False)
    file_size        = Column(BigInteger, nullable=True)
    sha256_hash      = Column(String(64), nullable=False)
    min_hardware_rev = Column(String(8), nullable=True)
    is_active        = Column(Boolean, default=False)
    created_at       = Column(DateTime(timezone=True), default=_now)


