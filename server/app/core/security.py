"""
JWT token creation / verification and device authentication.
"""

from datetime import datetime, timedelta, timezone
from jose import jwt, JWTError
from passlib.context import CryptContext
from app.config import get_settings

settings = get_settings()
pwd_context = CryptContext(schemes=["bcrypt"], deprecated="auto")


def create_access_token(data: dict, expires_delta: timedelta | None = None) -> str:
    """Create a JWT access token for device authentication."""
    to_encode = data.copy()
    expire = datetime.now(timezone.utc) + (
        expires_delta or timedelta(minutes=settings.jwt_expire_minutes)
    )
    to_encode.update({"exp": expire, "iat": datetime.now(timezone.utc)})
    return jwt.encode(to_encode, settings.secret_key, algorithm=settings.jwt_algorithm)


def verify_access_token(token: str) -> dict | None:
    """Verify a JWT token; returns payload or None if invalid/expired."""
    try:
        payload = jwt.decode(
            token, settings.secret_key, algorithms=[settings.jwt_algorithm],
            options={"verify_exp": True}
        )
        return payload
    except JWTError:
        return None


def hash_api_key(key: str) -> str:
    """Hash an API key for storage."""
    return pwd_context.hash(key)


def verify_api_key(plain: str, hashed: str) -> bool:
    """Verify a plain API key against its hash."""
    return pwd_context.verify(plain, hashed)


