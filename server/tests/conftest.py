"""PostgreSQL-backed fixtures for frozen API contract tests."""

from __future__ import annotations

import os
from collections.abc import AsyncIterator
from dataclasses import dataclass

import pytest
from app.config import Settings, get_settings
from app.db.models import (
    Conflict,
    Device,
    IdempotencyKey,
    PendingUpload,
    SaveUnit,
    SaveVersion,
    User,
)
from app.db.session import get_session_factory
from app.main import app
from app.security import generate_invite_token
from httpx import ASGITransport, AsyncClient
from sqlalchemy import delete


@dataclass(frozen=True)
class Identity:
    """Authenticated user and its registered device."""

    token: str
    user: User
    device: Device

    @property
    def headers(self) -> dict[str, str]:
        return {
            "Authorization": f"Bearer {self.token}",
            "X-Device-Id": str(self.device.id),
        }


def pytest_configure(config: pytest.Config) -> None:
    """Refuse to run against a database nobody declared disposable.

    `clean_database` empties the seven application tables of whatever
    `DATABASE_URL` points at. Pointed at a development database — which is what
    happens when the CI environment block is copied onto a workstation — it
    destroys it silently, since deleting rows is exactly its job. The opt-in has
    to be explicit and separate from `DATABASE_URL` itself: a database name
    tells us nothing, dev and CI both use `retrosave`.
    """

    del config
    # Un .env de développement ne doit pas réactiver un fournisseur JWT dans les tests.
    Settings.model_config["env_file"] = None
    get_settings.cache_clear()
    if os.environ.get("RETROSAVE_TEST_DB") != "1":
        raise pytest.UsageError(
            "server/tests efface toutes les tables de DATABASE_URL. "
            "Poser RETROSAVE_TEST_DB=1 pour confirmer que cette base est jetable "
            "(voir docs/guides/RUNBOOK.md)."
        )


@pytest.fixture(autouse=True)
async def clean_database() -> AsyncIterator[None]:
    """Isolate tests while preserving the migration-owned schema."""

    async with get_session_factory()() as db:
        for model in (
            IdempotencyKey,
            PendingUpload,
            Conflict,
            SaveVersion,
            SaveUnit,
            Device,
            User,
        ):
            await db.execute(delete(model))
        await db.commit()
    yield


@pytest.fixture
async def client() -> AsyncIterator[AsyncClient]:
    """Create an in-process API client without scheduler startup."""

    async with AsyncClient(
        transport=ASGITransport(app=app, raise_app_exceptions=False),
        base_url="http://test",
    ) as test_client:
        yield test_client


@pytest.fixture
async def identity() -> Identity:
    """Create one token-backed user and registered Windows device."""

    token, token_hash = generate_invite_token()
    async with get_session_factory()() as db:
        user = User(invite_token_hash=token_hash, label="testeur")
        db.add(user)
        await db.flush()
        device = Device(
            user_id=user.id,
            name="PC bureau",
            os="windows",
            app_version="0.1.0",
        )
        db.add(device)
        await db.commit()
        return Identity(token=token, user=user, device=device)
