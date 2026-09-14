"""Auth, device, unit, ownership, and idempotency API tests."""

from __future__ import annotations

from app.db.models import Device, SaveUnit, User
from app.db.session import get_session_factory
from app.security import generate_invite_token
from conftest import Identity
from httpx import AsyncClient
from sqlalchemy import func, select


async def test_invalid_token_uses_frozen_error_envelope(
    client: AsyncClient,
) -> None:
    """Unknown and malformed tokens must both stop with invalid_token."""

    response = await client.get(
        "/v0/devices",
        headers={
            "Authorization": "Bearer rsc_" + "0" * 40,
            "X-Device-Id": "00000000-0000-0000-0000-000000000000",
        },
    )

    assert response.status_code == 401
    assert response.json() == {
        "error": {
            "code": "invalid_token",
            "message": "Token invalide.",
            "details": {},
        }
    }


async def test_device_registration_is_idempotent(
    client: AsyncClient,
    identity: Identity,
) -> None:
    """Replaying one write key returns one identifier and creates one row."""

    headers = {
        "Authorization": f"Bearer {identity.token}",
        "Idempotency-Key": "register-laptop",
    }
    body = {"name": "Portable", "os": "linux", "app_version": "0.1.0"}

    first = await client.post("/v0/devices", headers=headers, json=body)
    second = await client.post("/v0/devices", headers=headers, json=body)

    assert first.status_code == second.status_code == 201
    assert first.json() == second.json()
    async with get_session_factory()() as db:
        count = await db.scalar(
            select(func.count()).select_from(Device).where(Device.user_id == identity.user.id)
        )
    assert count == 2


async def test_authenticated_request_updates_device_last_seen(
    client: AsyncClient,
    identity: Identity,
) -> None:
    """Every post-registration request refreshes the owned device best effort."""

    response = await client.get("/v0/devices", headers=identity.headers)

    assert response.status_code == 200
    async with get_session_factory()() as db:
        device = await db.get(Device, identity.device.id)
    assert device is not None
    assert device.last_seen_at is not None


async def test_unit_lifecycle_preserves_data(
    client: AsyncClient,
    identity: Identity,
) -> None:
    """Create, relabel, list, and mark missing without deleting the unit."""

    body = {
        "emulator": "ppsspp",
        "unit_key": "ULES01234GAMEDATA",
        "unit_type": "dir",
        "game_key": "ULES01234",
        "game_label": "ULES01234",
    }
    created = await client.post(
        "/v0/units",
        headers=identity.headers,
        json=body,
    )
    # M8 §7 : un renvoi de la même identité avec un libellé brut ne change rien.
    # L'amélioration d'un libellé `auto` a ses propres tests (test_label_source).
    existing = await client.post(
        "/v0/units",
        headers=identity.headers,
        json={**body, "game_label": "ULES01234"},
    )

    assert created.status_code == 201
    assert existing.status_code == 200
    assert existing.json()["unit"]["id"] == created.json()["unit"]["id"]
    assert existing.json()["unit"]["game_label"] == "ULES01234"
    unit_id = created.json()["unit"]["id"]

    patched = await client.patch(
        f"/v0/units/{unit_id}",
        headers=identity.headers,
        json={"game_label": "GTA LCS"},
    )
    missing = await client.post(
        f"/v0/units/{unit_id}/missing",
        headers=identity.headers,
        json={},
    )
    listed = await client.get("/v0/units", headers=identity.headers)

    assert patched.status_code == 200
    assert patched.json()["unit"]["game_label"] == "GTA LCS"
    assert missing.status_code == 200
    assert missing.json() == {"state": "missing"}
    assert listed.status_code == 200
    assert listed.json()["units"][0]["state"] == "missing"
    assert listed.json()["units"][0]["head"] is None
    async with get_session_factory()() as db:
        assert await db.get(SaveUnit, unit_id) is not None


async def test_unit_owner_is_checked_by_identifier(
    client: AsyncClient,
    identity: Identity,
) -> None:
    """An authenticated user cannot access another user's unit."""

    created = await client.post(
        "/v0/units",
        headers=identity.headers,
        json={
            "emulator": "folder",
            "unit_key": "fixture",
            "unit_type": "dir",
            "game_key": "fixture",
            "game_label": "Fixture",
        },
    )
    unit_id = created.json()["unit"]["id"]

    token, token_hash = generate_invite_token()
    async with get_session_factory()() as db:
        other_user = User(invite_token_hash=token_hash, label="other")
        db.add(other_user)
        await db.flush()
        other_device = Device(
            user_id=other_user.id,
            name="Other PC",
            os="windows",
            app_version="0.1.0",
        )
        db.add(other_device)
        await db.commit()

    response = await client.get(
        f"/v0/units/{unit_id}/versions",
        headers={
            "Authorization": f"Bearer {token}",
            "X-Device-Id": str(other_device.id),
        },
    )

    assert response.status_code == 403
    assert response.json()["error"]["code"] == "not_owner"


async def test_extra_write_field_is_rejected(
    client: AsyncClient,
    identity: Identity,
) -> None:
    """The API must not silently accept a field absent from the frozen table."""

    response = await client.post(
        "/v0/units",
        headers=identity.headers,
        json={
            "emulator": "folder",
            "unit_key": "fixture",
            "unit_type": "dir",
            "game_key": "fixture",
            "game_label": "Fixture",
            "invented": True,
        },
    )

    assert response.status_code == 422
    assert response.json()["error"]["code"] == "validation_error"
