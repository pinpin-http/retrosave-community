"""PostgreSQL concurrency tests for the M1 CAS transaction."""

from __future__ import annotations

import asyncio

import pytest
from app.api.errors import ApiError
from app.core.sync import CasConflict, ConfirmBody, Confirmed, confirm_version
from app.db.models import Conflict, PendingUpload, SaveUnit, SaveVersion
from app.db.session import get_session_factory
from conftest import Identity
from sqlalchemy import func, select


@pytest.mark.parametrize("preload_head", [False, True])
async def test_two_confirms_on_same_base_create_one_head_and_one_conflict(
    identity: Identity,
    preload_head: bool,
) -> None:
    """Pessimistic locking preserves both concurrent archives exactly once."""

    async with get_session_factory()() as db:
        unit = SaveUnit(
            user_id=identity.user.id,
            emulator="folder",
            unit_key="concurrent",
            unit_type="dir",
            game_key="concurrent",
            game_label="Concurrent",
        )
        db.add(unit)
        await db.flush()
        bodies = [
            ConfirmBody(
                object_key=f"u/{identity.user.id}/{unit.id}/{'11' * 32}.tar.zst",
                content_sha256=bytes.fromhex("11" * 32),
                archive_sha256=bytes.fromhex("aa" * 32),
                base_version=0,
                device_id=identity.device.id,
                env={"os": "windows", "app": "0.1.0"},
                client_mtime=None,
            ),
            ConfirmBody(
                object_key=f"u/{identity.user.id}/{unit.id}/{'22' * 32}.tar.zst",
                content_sha256=bytes.fromhex("22" * 32),
                archive_sha256=bytes.fromhex("bb" * 32),
                base_version=0,
                device_id=identity.device.id,
                env={"os": "windows", "app": "0.1.0"},
                client_mtime=None,
            ),
        ]
        db.add_all(
            [
                PendingUpload(
                    object_key=body.object_key,
                    unit_id=unit.id,
                    content_sha256=body.content_sha256,
                    archive_sha256=body.archive_sha256,
                    size_bytes=100 + index * 20,
                    archive_bytes=80 + index * 10,
                )
                for index, body in enumerate(bodies)
            ]
        )
        await db.commit()

    ready = asyncio.Barrier(2)

    async def run_confirm(body: ConfirmBody) -> Confirmed | CasConflict:
        async with get_session_factory()() as db:
            user = await db.get(type(identity.user), identity.user.id)
            assert user is not None
            # La route HTTP charge l'unité avant le HEAD S3. Garder une référence
            # forte reproduit son cache ORM avec expire_on_commit=False.
            cached = await db.get(SaveUnit, unit.id) if preload_head else None
            if preload_head:
                assert cached is not None and cached.head_version == 0
            await db.commit()
            await ready.wait()
            result = await confirm_version(db, user, unit.id, body)
            if cached is not None:
                assert cached.head_version == 1
            return result

    results = await asyncio.gather(*(run_confirm(body) for body in bodies))

    assert sum(isinstance(result, Confirmed) for result in results) == 1
    assert sum(isinstance(result, CasConflict) for result in results) == 1
    conflict_result = next(result for result in results if isinstance(result, CasConflict))
    assert conflict_result.head.number == 1
    assert conflict_result.yours.number == 2

    async with get_session_factory()() as db:
        refreshed = await db.get(SaveUnit, unit.id)
        versions = (
            await db.scalars(select(SaveVersion).where(SaveVersion.unit_id == unit.id))
        ).all()
        conflict_count = await db.scalar(
            select(func.count())
            .select_from(Conflict)
            .where(
                Conflict.unit_id == unit.id,
                Conflict.status == "open",
            )
        )
        pending_count = await db.scalar(
            select(func.count()).select_from(PendingUpload).where(PendingUpload.unit_id == unit.id)
        )

    assert refreshed is not None
    assert refreshed.head_version == 1
    assert sorted((version.number, version.kind) for version in versions) == [
        (1, "normal"),
        (2, "conflict_branch"),
    ]
    assert conflict_count == 1
    assert pending_count == 0


async def test_cached_pending_cannot_be_reused_after_concurrent_confirm(
    identity: Identity,
) -> None:
    """The Q10 same-key race creates one version, never a cached ghost row."""

    content_a = bytes.fromhex("11" * 32)
    content_b = bytes.fromhex("22" * 32)
    archive_b = bytes.fromhex("bb" * 32)
    async with get_session_factory()() as db:
        unit = SaveUnit(
            user_id=identity.user.id,
            emulator="folder",
            unit_key="same-content",
            unit_type="dir",
            game_key="same-content",
            game_label="Same content",
            head_version=1,
        )
        db.add(unit)
        await db.flush()
        db.add(
            SaveVersion(
                unit_id=unit.id,
                number=1,
                content_sha256=content_a,
                archive_sha256=bytes.fromhex("aa" * 32),
                size_bytes=100,
                archive_bytes=80,
                object_key=f"u/{identity.user.id}/{unit.id}/{content_a.hex()}.tar.zst",
                origin_device=identity.device.id,
                env={},
            )
        )
        object_key = f"u/{identity.user.id}/{unit.id}/{content_b.hex()}.tar.zst"
        db.add(
            PendingUpload(
                object_key=object_key,
                unit_id=unit.id,
                content_sha256=content_b,
                archive_sha256=archive_b,
                size_bytes=120,
                archive_bytes=90,
            )
        )
        await db.commit()

    body = ConfirmBody(
        object_key=object_key,
        content_sha256=content_b,
        archive_sha256=archive_b,
        base_version=1,
        device_id=identity.device.id,
        env={"os": "test"},
        client_mtime=None,
    )
    factory = get_session_factory()
    async with factory() as first_db, factory() as second_db:
        first_user = await first_db.get(type(identity.user), identity.user.id)
        second_user = await second_db.get(type(identity.user), identity.user.id)
        assert first_user is not None and second_user is not None
        assert await first_db.get(PendingUpload, object_key) is not None
        assert await second_db.get(PendingUpload, object_key) is not None
        await first_db.commit()
        await second_db.commit()

        results = await asyncio.gather(
            confirm_version(first_db, first_user, unit.id, body),
            confirm_version(second_db, second_user, unit.id, body),
            return_exceptions=True,
        )

    assert sum(isinstance(result, Confirmed) for result in results) == 1
    errors = [result for result in results if isinstance(result, ApiError)]
    assert len(errors) == 1
    assert errors[0].code == "unknown_upload"
    async with factory() as db:
        versions = (
            await db.scalars(select(SaveVersion).where(SaveVersion.unit_id == unit.id))
        ).all()
    assert sorted(version.number for version in versions) == [1, 2]
