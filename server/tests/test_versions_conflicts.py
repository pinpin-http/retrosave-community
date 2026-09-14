"""Prepare, history, restore, and conflict-resolution contract tests."""

from __future__ import annotations

from datetime import UTC, datetime, timedelta

import pytest
from app.api import versions as versions_api
from app.config import get_settings
from app.db.models import Conflict, Device, PendingUpload, SaveUnit, SaveVersion
from app.db.session import get_session_factory
from conftest import Identity
from httpx import AsyncClient
from sqlalchemy import func, select

CONTENT_A = bytes.fromhex("11" * 32)
CONTENT_B = bytes.fromhex("22" * 32)
CONTENT_C = bytes.fromhex("33" * 32)
CONTENT_D = bytes.fromhex("44" * 32)
ARCHIVE_A = bytes.fromhex("aa" * 32)
ARCHIVE_B = bytes.fromhex("bb" * 32)
ARCHIVE_C = bytes.fromhex("cc" * 32)
ARCHIVE_D = bytes.fromhex("dd" * 32)


async def create_versioned_unit(
    identity: Identity,
    *,
    with_conflict: bool = False,
) -> tuple[SaveUnit, Conflict | None]:
    """Seed immutable metadata without touching any archive content."""

    async with get_session_factory()() as db:
        unit = SaveUnit(
            user_id=identity.user.id,
            emulator="folder",
            unit_key="fixture",
            unit_type="dir",
            game_key="fixture",
            game_label="Fixture",
            head_version=1,
        )
        db.add(unit)
        await db.flush()
        db.add(
            SaveVersion(
                unit_id=unit.id,
                number=1,
                parent_number=None,
                content_sha256=CONTENT_A,
                archive_sha256=ARCHIVE_A,
                size_bytes=100,
                archive_bytes=80,
                object_key=f"u/{identity.user.id}/{unit.id}/{CONTENT_A.hex()}.tar.zst",
                origin_device=identity.device.id,
                env={"os": "windows", "app": "0.1.0"},
                kind="normal",
            )
        )
        conflict = None
        if with_conflict:
            db.add(
                SaveVersion(
                    unit_id=unit.id,
                    number=2,
                    parent_number=1,
                    content_sha256=CONTENT_B,
                    archive_sha256=ARCHIVE_B,
                    size_bytes=120,
                    archive_bytes=90,
                    object_key=(f"u/{identity.user.id}/{unit.id}/{CONTENT_B.hex()}.tar.zst"),
                    origin_device=identity.device.id,
                    env={"os": "windows", "app": "0.1.0"},
                    kind="conflict_branch",
                )
            )
            conflict = Conflict(
                unit_id=unit.id,
                version_a=1,
                version_b=2,
            )
            db.add(conflict)
        await db.commit()
        return unit, conflict


async def test_prepare_rejects_oversized_unit(
    client: AsyncClient,
    identity: Identity,
) -> None:
    """Logical content size is bounded before any upload URL is issued."""

    unit, _ = await create_versioned_unit(identity)
    response = await client.post(
        f"/v0/units/{unit.id}/versions:prepare",
        headers=identity.headers,
        json={
            "base_version": 1,
            "content_sha256": CONTENT_B.hex(),
            "archive_sha256": ARCHIVE_B.hex(),
            "size": get_settings().max_unit_bytes + 1,
            "archive_bytes": 90,
        },
    )

    assert response.status_code == 413
    assert response.json()["error"]["code"] == "payload_too_large"


async def test_prepare_deduplicates_only_current_head(
    client: AsyncClient,
    identity: Identity,
) -> None:
    """Matching the head avoids upload and returns the current number."""

    unit, _ = await create_versioned_unit(identity)
    response = await client.post(
        f"/v0/units/{unit.id}/versions:prepare",
        headers=identity.headers,
        json={
            "base_version": 1,
            "content_sha256": CONTENT_A.hex(),
            "archive_sha256": ARCHIVE_A.hex(),
            "size": 100,
            "archive_bytes": 80,
        },
    )

    assert response.status_code == 200
    assert response.json() == {"duplicate": True, "version": 1}


async def test_prepare_reserves_frozen_object_key(
    client: AsyncClient,
    identity: Identity,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """Non-duplicate content is namespaced by user, unit, and content hash."""

    unit, _ = await create_versioned_unit(identity)
    expires_at = datetime.now(UTC) + timedelta(minutes=15)

    async def fake_presign(object_key: str) -> tuple[str, datetime]:
        return f"https://upload.invalid/{object_key}", expires_at

    monkeypatch.setattr(versions_api, "presign_put", fake_presign)
    response = await client.post(
        f"/v0/units/{unit.id}/versions:prepare",
        headers=identity.headers,
        json={
            "base_version": 1,
            "content_sha256": CONTENT_B.hex(),
            "archive_sha256": ARCHIVE_B.hex(),
            "size": 120,
            "archive_bytes": 90,
        },
    )

    assert response.status_code == 200
    object_key = response.json()["upload"]["object_key"]
    assert object_key == (f"u/{identity.user.id}/{unit.id}/{CONTENT_B.hex()}.tar.zst")
    async with get_session_factory()() as db:
        pending = await db.get(PendingUpload, object_key)
    assert pending is not None
    assert pending.unit_id == unit.id
    assert pending.content_sha256 == CONTENT_B
    assert pending.archive_sha256 == ARCHIVE_B
    assert pending.size_bytes == 120
    assert pending.archive_bytes == 90


async def test_confirm_uses_prepared_sizes_and_replays_exact_response(
    client: AsyncClient,
    identity: Identity,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """Confirm keeps its public body unchanged and consumes frozen metadata."""

    unit, _ = await create_versioned_unit(identity)
    expires_at = datetime.now(UTC) + timedelta(minutes=15)
    verified: list[tuple[str, int]] = []

    async def fake_presign(object_key: str) -> tuple[str, datetime]:
        return f"https://upload.invalid/{object_key}", expires_at

    async def fake_verify(object_key: str, archive_bytes: int) -> None:
        verified.append((object_key, archive_bytes))

    monkeypatch.setattr(versions_api, "presign_put", fake_presign)
    monkeypatch.setattr(versions_api, "verify_uploaded_object", fake_verify)
    prepared = await client.post(
        f"/v0/units/{unit.id}/versions:prepare",
        headers=identity.headers,
        json={
            "base_version": 1,
            "content_sha256": CONTENT_B.hex(),
            "archive_sha256": ARCHIVE_B.hex(),
            "size": 120,
            "archive_bytes": 90,
        },
    )
    object_key = prepared.json()["upload"]["object_key"]
    headers = {**identity.headers, "Idempotency-Key": "confirm-frozen-metadata"}
    body = {
        "object_key": object_key,
        "content_sha256": CONTENT_B.hex(),
        "archive_sha256": ARCHIVE_B.hex(),
        "base_version": 1,
        "env": {"os": "windows", "app": "0.1.0"},
        "client_mtime": None,
    }

    confirmed = await client.post(
        f"/v0/units/{unit.id}/versions:confirm",
        headers=headers,
        json=body,
    )
    replayed = await client.post(
        f"/v0/units/{unit.id}/versions:confirm",
        headers=headers,
        json=body,
    )

    assert confirmed.status_code == replayed.status_code == 201
    assert confirmed.json() == replayed.json() == {"version": 2}
    assert verified == [(object_key, 90)]
    async with get_session_factory()() as db:
        version = await db.scalar(
            select(SaveVersion).where(
                SaveVersion.unit_id == unit.id,
                SaveVersion.number == 2,
            )
        )
        pending = await db.get(PendingUpload, object_key)
    assert version is not None
    assert version.size_bytes == 120
    assert version.archive_bytes == 90
    assert version.archive_sha256 == ARCHIVE_B
    assert pending is None


async def test_latest_prepare_archive_hash_wins_same_content_race(
    client: AsyncClient,
    identity: Identity,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """A stale uploader cannot confirm metadata after another PUT won the key."""

    unit, _ = await create_versioned_unit(identity)
    expires_at = datetime.now(UTC) + timedelta(minutes=15)

    async def fake_presign(object_key: str) -> tuple[str, datetime]:
        return f"https://upload.invalid/{object_key}", expires_at

    async def fake_verify(_object_key: str, _archive_bytes: int) -> None:
        return None

    monkeypatch.setattr(versions_api, "presign_put", fake_presign)
    monkeypatch.setattr(versions_api, "verify_uploaded_object", fake_verify)
    prepare_body = {
        "base_version": 1,
        "content_sha256": CONTENT_B.hex(),
        "size": 120,
        "archive_bytes": 90,
    }
    first = await client.post(
        f"/v0/units/{unit.id}/versions:prepare",
        headers=identity.headers,
        json={**prepare_body, "archive_sha256": ARCHIVE_B.hex()},
    )
    second = await client.post(
        f"/v0/units/{unit.id}/versions:prepare",
        headers=identity.headers,
        json={**prepare_body, "archive_sha256": ARCHIVE_C.hex()},
    )
    object_key = second.json()["upload"]["object_key"]
    confirm_body = {
        "object_key": object_key,
        "content_sha256": CONTENT_B.hex(),
        "base_version": 1,
        "env": {"os": "windows", "app": "0.1.0"},
        "client_mtime": None,
    }

    stale = await client.post(
        f"/v0/units/{unit.id}/versions:confirm",
        headers=identity.headers,
        json={**confirm_body, "archive_sha256": ARCHIVE_B.hex()},
    )
    winner = await client.post(
        f"/v0/units/{unit.id}/versions:confirm",
        headers=identity.headers,
        json={**confirm_body, "archive_sha256": ARCHIVE_C.hex()},
    )

    assert first.status_code == second.status_code == 200
    assert stale.status_code == 422
    assert stale.json()["error"]["code"] == "checksum_mismatch"
    assert winner.status_code == 201
    async with get_session_factory()() as db:
        version = await db.scalar(
            select(SaveVersion).where(
                SaveVersion.unit_id == unit.id,
                SaveVersion.number == 2,
            )
        )
    assert version is not None
    assert version.archive_sha256 == ARCHIVE_C


async def test_confirm_without_pending_returns_unknown_upload_before_head(
    client: AsyncClient,
    identity: Identity,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """An expired or consumed reservation forces a bounded client re-prepare."""

    unit, _ = await create_versioned_unit(identity)

    async def unexpected_verify(_object_key: str, _archive_bytes: int) -> None:
        raise AssertionError("HEAD must not run without a pending upload")

    monkeypatch.setattr(
        versions_api,
        "verify_uploaded_object",
        unexpected_verify,
    )
    object_key = f"u/{identity.user.id}/{unit.id}/{CONTENT_B.hex()}.tar.zst"
    response = await client.post(
        f"/v0/units/{unit.id}/versions:confirm",
        headers=identity.headers,
        json={
            "object_key": object_key,
            "content_sha256": CONTENT_B.hex(),
            "archive_sha256": ARCHIVE_B.hex(),
            "base_version": 1,
            "env": {"os": "windows", "app": "0.1.0"},
            "client_mtime": None,
        },
    )

    assert response.status_code == 422
    assert response.json()["error"]["code"] == "unknown_upload"


async def test_confirm_rejects_content_different_from_pending(
    client: AsyncClient,
    identity: Identity,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """The prepared server-side hash wins over a changed confirm body."""

    unit, _ = await create_versioned_unit(identity)
    expires_at = datetime.now(UTC) + timedelta(minutes=15)

    async def fake_presign(object_key: str) -> tuple[str, datetime]:
        return f"https://upload.invalid/{object_key}", expires_at

    async def unexpected_verify(_object_key: str, _archive_bytes: int) -> None:
        raise AssertionError("HEAD must not run for mismatched metadata")

    monkeypatch.setattr(versions_api, "presign_put", fake_presign)
    monkeypatch.setattr(
        versions_api,
        "verify_uploaded_object",
        unexpected_verify,
    )
    prepared = await client.post(
        f"/v0/units/{unit.id}/versions:prepare",
        headers=identity.headers,
        json={
            "base_version": 1,
            "content_sha256": CONTENT_B.hex(),
            "archive_sha256": ARCHIVE_B.hex(),
            "size": 120,
            "archive_bytes": 90,
        },
    )
    response = await client.post(
        f"/v0/units/{unit.id}/versions:confirm",
        headers=identity.headers,
        json={
            "object_key": prepared.json()["upload"]["object_key"],
            "content_sha256": CONTENT_C.hex(),
            "archive_sha256": ARCHIVE_C.hex(),
            "base_version": 1,
            "env": {"os": "windows", "app": "0.1.0"},
            "client_mtime": None,
        },
    )

    assert response.status_code == 422
    assert response.json()["error"]["code"] == "checksum_mismatch"


async def test_same_content_race_reprepares_as_duplicate(
    client: AsyncClient,
    identity: Identity,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """A consumed shared pending is recovered by prepare without a new version."""

    unit, _ = await create_versioned_unit(identity)
    async with get_session_factory()() as db:
        second_device = Device(
            user_id=identity.user.id,
            name="Portable",
            os="linux",
            app_version="0.1.0",
        )
        db.add(second_device)
        await db.commit()
    second_headers = {
        "Authorization": f"Bearer {identity.token}",
        "X-Device-Id": str(second_device.id),
    }
    expires_at = datetime.now(UTC) + timedelta(minutes=15)

    async def fake_presign(object_key: str) -> tuple[str, datetime]:
        return f"https://upload.invalid/{object_key}", expires_at

    async def fake_verify(_object_key: str, _archive_bytes: int) -> None:
        return None

    monkeypatch.setattr(versions_api, "presign_put", fake_presign)
    monkeypatch.setattr(versions_api, "verify_uploaded_object", fake_verify)
    prepare_body = {
        "base_version": 1,
        "content_sha256": CONTENT_B.hex(),
        "archive_sha256": ARCHIVE_B.hex(),
        "size": 120,
        "archive_bytes": 90,
    }
    first_prepare = await client.post(
        f"/v0/units/{unit.id}/versions:prepare",
        headers=identity.headers,
        json=prepare_body,
    )
    second_prepare = await client.post(
        f"/v0/units/{unit.id}/versions:prepare",
        headers=second_headers,
        json=prepare_body,
    )
    object_key = first_prepare.json()["upload"]["object_key"]
    assert second_prepare.json()["upload"]["object_key"] == object_key
    confirm_body = {
        "object_key": object_key,
        "content_sha256": CONTENT_B.hex(),
        "archive_sha256": ARCHIVE_B.hex(),
        "base_version": 1,
        "env": {"os": "windows", "app": "0.1.0"},
        "client_mtime": None,
    }

    first_confirm = await client.post(
        f"/v0/units/{unit.id}/versions:confirm",
        headers={
            **identity.headers,
            "Idempotency-Key": "same-content-first",
        },
        json=confirm_body,
    )
    second_confirm = await client.post(
        f"/v0/units/{unit.id}/versions:confirm",
        headers={
            **second_headers,
            "Idempotency-Key": "same-content-second",
        },
        json=confirm_body,
    )
    recovered = await client.post(
        f"/v0/units/{unit.id}/versions:prepare",
        headers=second_headers,
        json=prepare_body,
    )

    assert first_confirm.status_code == 201
    assert first_confirm.json() == {"version": 2}
    assert second_confirm.status_code == 422
    assert second_confirm.json()["error"]["code"] == "unknown_upload"
    assert recovered.status_code == 200
    assert recovered.json() == {"duplicate": True, "version": 2}
    async with get_session_factory()() as db:
        version_count = await db.scalar(
            select(func.count()).select_from(SaveVersion).where(SaveVersion.unit_id == unit.id)
        )
    assert version_count == 2


async def test_three_device_conflicts_are_strictly_sequential(
    client: AsyncClient,
    identity: Identity,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """Three devices produce one open A/B conflict at a time with zero loss."""

    unit, _ = await create_versioned_unit(identity)
    async with get_session_factory()() as db:
        second_device = Device(
            user_id=identity.user.id,
            name="Portable",
            os="linux",
            app_version="0.1.0",
        )
        third_device = Device(
            user_id=identity.user.id,
            name="Thor",
            os="android",
            app_version="0.1.0",
        )
        db.add_all([second_device, third_device])
        await db.commit()
    auth = {"Authorization": f"Bearer {identity.token}"}
    second_headers = {**auth, "X-Device-Id": str(second_device.id)}
    third_headers = {**auth, "X-Device-Id": str(third_device.id)}
    expires_at = datetime.now(UTC) + timedelta(minutes=15)

    async def fake_presign(object_key: str) -> tuple[str, datetime]:
        return f"https://upload.invalid/{object_key}", expires_at

    async def fake_verify(_object_key: str, _archive_bytes: int) -> None:
        return None

    monkeypatch.setattr(versions_api, "presign_put", fake_presign)
    monkeypatch.setattr(versions_api, "verify_uploaded_object", fake_verify)

    prepared: dict[str, str] = {}
    for name, content, archive, headers in (
        ("second", CONTENT_B, ARCHIVE_B, second_headers),
        ("first", CONTENT_C, ARCHIVE_C, identity.headers),
        ("third", CONTENT_D, ARCHIVE_D, third_headers),
    ):
        response = await client.post(
            f"/v0/units/{unit.id}/versions:prepare",
            headers=headers,
            json={
                "base_version": 1,
                "content_sha256": content.hex(),
                "archive_sha256": archive.hex(),
                "size": 120,
                "archive_bytes": 90,
            },
        )
        assert response.status_code == 200
        prepared[name] = response.json()["upload"]["object_key"]

    async def confirm(
        *,
        name: str,
        content: bytes,
        archive: bytes,
        headers: dict[str, str],
        idempotency_key: str,
    ):
        return await client.post(
            f"/v0/units/{unit.id}/versions:confirm",
            headers={**headers, "Idempotency-Key": idempotency_key},
            json={
                "object_key": prepared[name],
                "content_sha256": content.hex(),
                "archive_sha256": archive.hex(),
                "base_version": 1,
                "env": {"os": "test", "app": "0.1.0"},
                "client_mtime": None,
            },
        )

    advanced = await confirm(
        name="second",
        content=CONTENT_B,
        archive=ARCHIVE_B,
        headers=second_headers,
        idempotency_key="cascade-advance",
    )
    first_conflict = await confirm(
        name="first",
        content=CONTENT_C,
        archive=ARCHIVE_C,
        headers=identity.headers,
        idempotency_key="cascade-first-conflict",
    )
    blocked = await confirm(
        name="third",
        content=CONTENT_D,
        archive=ARCHIVE_D,
        headers=third_headers,
        idempotency_key="cascade-blocked",
    )

    assert advanced.status_code == 201
    assert first_conflict.status_code == 409
    assert first_conflict.json()["error"]["code"] == "cas_conflict"
    assert blocked.status_code == 409
    assert blocked.json()["error"]["code"] == "open_conflict"
    async with get_session_factory()() as db:
        retained_pending = await db.get(PendingUpload, prepared["third"])
    assert retained_pending is not None

    resolved = await client.post(
        f"/v0/conflicts/{first_conflict.json()['conflict_id']}/resolve",
        headers=identity.headers,
        json={"winner": 2},
    )
    second_conflict = await confirm(
        name="third",
        content=CONTENT_D,
        archive=ARCHIVE_D,
        headers=third_headers,
        idempotency_key="cascade-after-resolution",
    )

    assert resolved.status_code == 200
    assert second_conflict.status_code == 409
    assert second_conflict.json()["error"]["code"] == "cas_conflict"
    async with get_session_factory()() as db:
        versions = (
            await db.scalars(
                select(SaveVersion)
                .where(SaveVersion.unit_id == unit.id)
                .order_by(SaveVersion.number)
            )
        ).all()
        conflicts = (
            await db.scalars(
                select(Conflict).where(Conflict.unit_id == unit.id).order_by(Conflict.created_at)
            )
        ).all()
        pending_count = await db.scalar(
            select(func.count()).select_from(PendingUpload).where(PendingUpload.unit_id == unit.id)
        )
    assert [version.number for version in versions] == [1, 2, 3, 4]
    assert [version.content_sha256 for version in versions] == [
        CONTENT_A,
        CONTENT_B,
        CONTENT_C,
        CONTENT_D,
    ]
    assert [
        (conflict.status, conflict.version_a, conflict.version_b) for conflict in conflicts
    ] == [
        ("resolved", 2, 3),
        ("open", 2, 4),
    ]
    assert pending_count == 0


async def test_restore_creates_new_version_without_copying_object(
    client: AsyncClient,
    identity: Identity,
) -> None:
    """Restore advances the head but reuses immutable S3 object metadata."""

    unit, _ = await create_versioned_unit(identity)
    response = await client.post(
        f"/v0/units/{unit.id}/restore",
        headers=identity.headers,
        json={"version": 1},
    )

    assert response.status_code == 201
    assert response.json() == {"version": 2}
    async with get_session_factory()() as db:
        restored = await db.scalar(
            select(SaveVersion).where(
                SaveVersion.unit_id == unit.id,
                SaveVersion.number == 2,
            )
        )
        refreshed_unit = await db.get(SaveUnit, unit.id)
    assert restored is not None
    assert restored.kind == "restore"
    assert restored.object_key.endswith(f"/{CONTENT_A.hex()}.tar.zst")
    assert refreshed_unit is not None
    assert refreshed_unit.head_version == 2


async def test_history_and_download_return_integrity_metadata(
    client: AsyncClient,
    identity: Identity,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """History is descending and download carries both frozen hashes."""

    unit, _ = await create_versioned_unit(identity)
    expires_at = datetime.now(UTC) + timedelta(minutes=15)

    async def fake_presign(object_key: str) -> tuple[str, datetime]:
        return f"https://download.invalid/{object_key}", expires_at

    monkeypatch.setattr(versions_api, "presign_get", fake_presign)
    history = await client.get(
        f"/v0/units/{unit.id}/versions",
        headers=identity.headers,
    )
    download = await client.get(
        f"/v0/units/{unit.id}/versions/1/download",
        headers=identity.headers,
    )

    assert history.status_code == 200
    assert history.json()["head_version"] == 1
    assert history.json()["versions"][0]["content_sha256"] == CONTENT_A.hex()
    assert download.status_code == 200
    assert download.json()["archive_sha256"] == ARCHIVE_A.hex()
    assert download.json()["content_sha256"] == CONTENT_A.hex()
    assert download.json()["archive_bytes"] == 80


async def test_resolve_preserves_both_versions_and_is_replay_safe(
    client: AsyncClient,
    identity: Identity,
) -> None:
    """Resolution moves only the head and never deletes the losing branch."""

    unit, conflict = await create_versioned_unit(identity, with_conflict=True)
    assert conflict is not None
    url = f"/v0/conflicts/{conflict.id}/resolve"

    resolved = await client.post(
        url,
        headers=identity.headers,
        json={"winner": 2},
    )
    replay = await client.post(
        url,
        headers=identity.headers,
        json={"winner": 2},
    )
    changed_winner = await client.post(
        url,
        headers=identity.headers,
        json={"winner": 1},
    )

    assert resolved.status_code == replay.status_code == 200
    assert resolved.json() == {
        "unit_id": str(unit.id),
        "head_version": 2,
    }
    assert changed_winner.status_code == 410
    assert changed_winner.json()["error"]["code"] == "already_resolved"
    async with get_session_factory()() as db:
        versions = (
            await db.scalars(select(SaveVersion.number).where(SaveVersion.unit_id == unit.id))
        ).all()
    assert sorted(versions) == [1, 2]


async def test_open_conflict_is_listed_and_blocks_restore(
    client: AsyncClient,
    identity: Identity,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """Prepare, confirm, and restore all preserve an existing A/B conflict."""

    unit, conflict = await create_versioned_unit(identity, with_conflict=True)
    assert conflict is not None
    object_key = f"u/{identity.user.id}/{unit.id}/{CONTENT_C.hex()}.tar.zst"
    async with get_session_factory()() as db:
        db.add(
            PendingUpload(
                object_key=object_key,
                unit_id=unit.id,
                content_sha256=CONTENT_C,
                archive_sha256=ARCHIVE_C,
                size_bytes=140,
                archive_bytes=100,
            )
        )
        await db.commit()

    async def unexpected_verify(_object_key: str, _archive_bytes: int) -> None:
        raise AssertionError("HEAD must not run during an open conflict")

    monkeypatch.setattr(
        versions_api,
        "verify_uploaded_object",
        unexpected_verify,
    )
    listed = await client.get(
        "/v0/conflicts?open=1",
        headers=identity.headers,
    )
    prepare = await client.post(
        f"/v0/units/{unit.id}/versions:prepare",
        headers=identity.headers,
        json={
            "base_version": 1,
            "content_sha256": CONTENT_C.hex(),
            "archive_sha256": ARCHIVE_C.hex(),
            "size": get_settings().max_unit_bytes + 1,
            "archive_bytes": 100,
        },
    )
    confirm = await client.post(
        f"/v0/units/{unit.id}/versions:confirm",
        headers=identity.headers,
        json={
            "object_key": object_key,
            "content_sha256": CONTENT_C.hex(),
            "archive_sha256": ARCHIVE_C.hex(),
            "base_version": 1,
            "env": {"os": "windows", "app": "0.1.0"},
            "client_mtime": None,
        },
    )
    restore = await client.post(
        f"/v0/units/{unit.id}/restore",
        headers=identity.headers,
        json={"version": 1},
    )

    assert listed.status_code == 200
    assert listed.json()["conflicts"][0]["id"] == str(conflict.id)
    assert listed.json()["conflicts"][0]["version_a"]["number"] == 1
    assert listed.json()["conflicts"][0]["version_b"]["number"] == 2
    for response in (prepare, confirm, restore):
        assert response.status_code == 409
        payload = response.json()
        assert payload["error"]["code"] == "open_conflict"
        assert payload["conflict_id"] == str(conflict.id)
        assert payload["version_a"]["number"] == 1
        assert payload["version_b"]["number"] == 2
    async with get_session_factory()() as db:
        version_count = await db.scalar(
            select(func.count()).select_from(SaveVersion).where(SaveVersion.unit_id == unit.id)
        )
        conflict_count = await db.scalar(
            select(func.count())
            .select_from(Conflict)
            .where(
                Conflict.unit_id == unit.id,
                Conflict.status == "open",
            )
        )
        pending = await db.get(PendingUpload, object_key)
    assert version_count == 2
    assert conflict_count == 1
    assert pending is not None


async def test_numbering_uses_max_after_resolving_to_older_branch(
    client: AsyncClient,
    identity: Identity,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """Resolve 6 over 7, restore to 8, then normal confirm to 9."""

    async with get_session_factory()() as db:
        unit = SaveUnit(
            user_id=identity.user.id,
            emulator="folder",
            unit_key="numbering",
            unit_type="dir",
            game_key="numbering",
            game_label="Numbering",
            head_version=6,
        )
        db.add(unit)
        await db.flush()
        db.add_all(
            [
                SaveVersion(
                    unit_id=unit.id,
                    number=6,
                    parent_number=None,
                    content_sha256=CONTENT_A,
                    archive_sha256=ARCHIVE_A,
                    size_bytes=100,
                    archive_bytes=80,
                    object_key=(f"u/{identity.user.id}/{unit.id}/{CONTENT_A.hex()}.tar.zst"),
                    origin_device=identity.device.id,
                    env={},
                    kind="normal",
                ),
                SaveVersion(
                    unit_id=unit.id,
                    number=7,
                    parent_number=6,
                    content_sha256=CONTENT_B,
                    archive_sha256=ARCHIVE_B,
                    size_bytes=120,
                    archive_bytes=90,
                    object_key=(f"u/{identity.user.id}/{unit.id}/{CONTENT_B.hex()}.tar.zst"),
                    origin_device=identity.device.id,
                    env={},
                    kind="conflict_branch",
                ),
            ]
        )
        conflict = Conflict(
            unit_id=unit.id,
            version_a=6,
            version_b=7,
        )
        db.add(conflict)
        await db.commit()

    resolved = await client.post(
        f"/v0/conflicts/{conflict.id}/resolve",
        headers=identity.headers,
        json={"winner": 6},
    )
    restored = await client.post(
        f"/v0/units/{unit.id}/restore",
        headers=identity.headers,
        json={"version": 6},
    )
    expires_at = datetime.now(UTC) + timedelta(minutes=15)

    async def fake_presign(object_key: str) -> tuple[str, datetime]:
        return f"https://upload.invalid/{object_key}", expires_at

    async def fake_verify(_object_key: str, _archive_bytes: int) -> None:
        return None

    monkeypatch.setattr(versions_api, "presign_put", fake_presign)
    monkeypatch.setattr(versions_api, "verify_uploaded_object", fake_verify)
    prepared = await client.post(
        f"/v0/units/{unit.id}/versions:prepare",
        headers=identity.headers,
        json={
            "base_version": 8,
            "content_sha256": CONTENT_C.hex(),
            "archive_sha256": ARCHIVE_C.hex(),
            "size": 140,
            "archive_bytes": 100,
        },
    )
    confirmed = await client.post(
        f"/v0/units/{unit.id}/versions:confirm",
        headers=identity.headers,
        json={
            "object_key": prepared.json()["upload"]["object_key"],
            "content_sha256": CONTENT_C.hex(),
            "archive_sha256": ARCHIVE_C.hex(),
            "base_version": 8,
            "env": {"os": "windows", "app": "0.1.0"},
            "client_mtime": None,
        },
    )

    assert resolved.status_code == 200
    assert restored.status_code == 201
    assert restored.json() == {"version": 8}
    assert prepared.status_code == 200
    assert confirmed.status_code == 201
    assert confirmed.json() == {"version": 9}
    async with get_session_factory()() as db:
        refreshed = await db.get(SaveUnit, unit.id)
        versions = (
            await db.scalars(
                select(SaveVersion)
                .where(SaveVersion.unit_id == unit.id)
                .order_by(SaveVersion.number)
            )
        ).all()
    assert refreshed is not None
    assert refreshed.head_version == 9
    assert [version.number for version in versions] == [6, 7, 8, 9]
    assert versions[2].kind == "restore"
    assert versions[2].parent_number == 6
    assert versions[3].kind == "normal"
    assert versions[3].parent_number == 8
