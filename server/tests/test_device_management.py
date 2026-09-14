"""Device rename and revocation (EXP-01).

Ces tests tiennent surtout sur un point : une révocation doit **refuser** un
appareil, pas seulement l'afficher barré. Un écran qui annonce « accès révoqué »
alors que l'appareil continue de publier serait pire que pas d'écran du tout.
"""

from __future__ import annotations

import uuid

from app.db.models import Device
from app.db.session import get_session_factory
from conftest import Identity
from httpx import AsyncClient


async def _register(client: AsyncClient, identity: Identity, name: str) -> str:
    """Register a second device and return its identifier."""

    response = await client.post(
        "/v0/devices",
        headers={"Authorization": f"Bearer {identity.token}"},
        json={"name": name, "os": "linux", "app_version": "0.1.0"},
    )
    assert response.status_code == 201
    return response.json()["device_id"]


async def test_rename_changes_the_label_and_nothing_else(
    client: AsyncClient,
    identity: Identity,
) -> None:
    """Renaming is cosmetic: same identifier, same registration."""

    other = await _register(client, identity, "Portable")

    response = await client.patch(
        f"/v0/devices/{other}",
        headers=identity.headers,
        json={"name": "Console du salon"},
    )

    assert response.status_code == 200
    assert response.json()["name"] == "Console du salon"
    assert response.json()["id"] == other
    assert response.json()["revoked_at"] is None

    listing = await client.get("/v0/devices", headers=identity.headers)
    names = {device["name"] for device in listing.json()["devices"]}
    assert names == {"PC bureau", "Console du salon"}


async def test_an_empty_name_is_refused(
    client: AsyncClient,
    identity: Identity,
) -> None:
    """A device with no name would remove the only cue in the history."""

    other = await _register(client, identity, "Portable")

    response = await client.patch(
        f"/v0/devices/{other}",
        headers=identity.headers,
        json={"name": ""},
    )

    assert response.status_code == 422


async def test_a_revoked_device_is_actually_refused(
    client: AsyncClient,
    identity: Identity,
) -> None:
    """Revocation must stop the device, not merely flag it."""

    other = await _register(client, identity, "Portable")
    other_headers = {
        "Authorization": f"Bearer {identity.token}",
        "X-Device-Id": other,
    }
    # Avant la révocation, cet appareil est accepté : sans cette vérification,
    # le test suivant pourrait passer pour une raison sans rapport.
    assert (await client.get("/v0/units", headers=other_headers)).status_code == 200

    revoked = await client.post(
        f"/v0/devices/{other}/revoke",
        headers=identity.headers,
    )
    assert revoked.status_code == 200
    assert revoked.json()["revoked_at"] is not None

    refused = await client.get("/v0/units", headers=other_headers)
    assert refused.status_code == 403
    assert refused.json()["error"]["code"] == "device_revoked"


async def test_revocation_destroys_nothing(
    client: AsyncClient,
    identity: Identity,
) -> None:
    """The row stays, so the history can still name the origin device (I2)."""

    other = await _register(client, identity, "Portable")
    await client.post(f"/v0/devices/{other}/revoke", headers=identity.headers)

    async with get_session_factory()() as db:
        stored = await db.get(Device, uuid.UUID(other))
    assert stored is not None
    assert stored.name == "Portable"

    listing = await client.get("/v0/devices", headers=identity.headers)
    revoked = [d for d in listing.json()["devices"] if d["id"] == other]
    assert len(revoked) == 1
    assert revoked[0]["revoked_at"] is not None


async def test_revoking_twice_keeps_the_first_date(
    client: AsyncClient,
    identity: Identity,
) -> None:
    """Replaying the call must not move the recorded moment."""

    other = await _register(client, identity, "Portable")
    first = await client.post(f"/v0/devices/{other}/revoke", headers=identity.headers)
    second = await client.post(f"/v0/devices/{other}/revoke", headers=identity.headers)

    assert first.status_code == second.status_code == 200
    assert first.json()["revoked_at"] == second.json()["revoked_at"]


async def test_a_device_cannot_revoke_itself(
    client: AsyncClient,
    identity: Identity,
) -> None:
    """Otherwise the user locks themselves out mid-gesture."""

    response = await client.post(
        f"/v0/devices/{identity.device.id}/revoke",
        headers=identity.headers,
    )

    assert response.status_code == 409
    assert response.json()["error"]["code"] == "self_revocation"


async def test_an_unknown_device_is_not_found(
    client: AsyncClient,
    identity: Identity,
) -> None:
    """Ownership is resolved before anything is written."""

    stranger = str(uuid.uuid4())

    renamed = await client.patch(
        f"/v0/devices/{stranger}",
        headers=identity.headers,
        json={"name": "Pris"},
    )
    assert renamed.status_code == 404

    revoked = await client.post(
        f"/v0/devices/{stranger}/revoke",
        headers=identity.headers,
    )
    assert revoked.status_code == 404


async def test_a_malformed_identifier_is_refused_before_any_write(
    client: AsyncClient,
    identity: Identity,
) -> None:
    """A path segment is never trusted as a UUID."""

    response = await client.post(
        "/v0/devices/pas-un-uuid/revoke",
        headers=identity.headers,
    )

    assert response.status_code == 422
