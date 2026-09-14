"""Token secrecy and hourly maintenance job tests."""

from __future__ import annotations

import io
import logging
from datetime import UTC, datetime, timedelta

import pytest
from app import jobs
from app.cli import create_user_token
from app.config import get_settings
from app.db.models import (
    IdempotencyKey,
    PendingUpload,
    SaveUnit,
    SaveVersion,
    User,
)
from app.db.session import get_session_factory
from app.logging import JsonFormatter, TokenRedactionFilter, configure_logging
from app.security import hash_token, is_valid_token_format
from conftest import Identity
from sqlalchemy import select


async def test_new_token_stores_only_sha256_digest() -> None:
    """The administrative flow never persists the plaintext invitation."""

    token = await create_user_token("proprietaire")

    assert is_valid_token_format(token)
    async with get_session_factory()() as db:
        user = await db.scalar(select(User).where(User.label == "proprietaire"))
    assert user is not None
    assert user.invite_token_hash == hash_token(token)
    assert token.encode() not in user.invite_token_hash


def test_logging_filter_removes_complete_token() -> None:
    """No formatter output may retain an invitation token."""

    token = "rsc_" + "a" * 40
    stream = io.StringIO()
    handler = logging.StreamHandler(stream)
    handler.addFilter(TokenRedactionFilter())
    handler.setFormatter(JsonFormatter())
    logger = logging.getLogger("retrosave-token-redaction-test")
    logger.handlers = [handler]
    logger.propagate = False
    logger.setLevel(logging.INFO)

    logger.info("received %s", token)

    output = stream.getvalue()
    assert token not in output
    assert "[REDACTED_TOKEN]" in output


def test_logging_configuration_keeps_framework_logs_json_safe() -> None:
    """Uvicorn propagates through JSON while verbose S3 internals stay hidden."""

    configure_logging("DEBUG")

    assert logging.getLogger("uvicorn.access").handlers == []
    assert logging.getLogger("uvicorn.access").propagate is True
    assert logging.getLogger("botocore").level == logging.WARNING


async def test_gc_deletes_only_unreferenced_expired_upload(
    identity: Identity,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """GC preserves referenced blobs and never scans archive contents."""

    old = datetime.now(UTC) - timedelta(hours=get_settings().pending_upload_ttl_h + 1)
    referenced_key = "u/referenced.tar.zst"
    orphan_key = "u/orphan.tar.zst"
    async with get_session_factory()() as db:
        unit = SaveUnit(
            user_id=identity.user.id,
            emulator="folder",
            unit_key="gc",
            unit_type="dir",
            game_key="gc",
            game_label="GC",
            head_version=1,
        )
        db.add(unit)
        await db.flush()
        db.add(
            SaveVersion(
                unit_id=unit.id,
                number=1,
                content_sha256=bytes.fromhex("11" * 32),
                archive_sha256=bytes.fromhex("aa" * 32),
                size_bytes=10,
                archive_bytes=8,
                object_key=referenced_key,
                origin_device=identity.device.id,
                env={},
            )
        )
        db.add_all(
            [
                PendingUpload(
                    object_key=referenced_key,
                    unit_id=unit.id,
                    content_sha256=bytes.fromhex("11" * 32),
                    archive_sha256=bytes.fromhex("aa" * 32),
                    size_bytes=10,
                    archive_bytes=8,
                    created_at=old,
                ),
                PendingUpload(
                    object_key=orphan_key,
                    unit_id=unit.id,
                    content_sha256=bytes.fromhex("22" * 32),
                    archive_sha256=bytes.fromhex("bb" * 32),
                    size_bytes=10,
                    archive_bytes=8,
                    created_at=old,
                ),
            ]
        )
        await db.commit()

    deleted: list[str] = []

    async def fake_delete(object_key: str) -> None:
        deleted.append(object_key)

    monkeypatch.setattr(jobs, "delete_object", fake_delete)
    await jobs.gc_pending_uploads()

    assert deleted == [orphan_key]
    async with get_session_factory()() as db:
        remaining = (await db.scalars(select(PendingUpload.object_key))).all()
    assert remaining == []


async def test_idempotency_purge_uses_configured_ttl(
    identity: Identity,
) -> None:
    """Only expired cached responses are purged by the hourly job."""

    old = datetime.now(UTC) - timedelta(hours=get_settings().idempotency_ttl_h + 1)
    async with get_session_factory()() as db:
        db.add_all(
            [
                IdempotencyKey(
                    key="expired",
                    user_id=identity.user.id,
                    status_code=200,
                    response={"ok": True},
                    created_at=old,
                ),
                IdempotencyKey(
                    key="fresh",
                    user_id=identity.user.id,
                    status_code=200,
                    response={"ok": True},
                ),
            ]
        )
        await db.commit()

    await jobs.purge_idempotency_keys()

    async with get_session_factory()() as db:
        keys = (await db.scalars(select(IdempotencyKey.key))).all()
    assert keys == ["fresh"]
