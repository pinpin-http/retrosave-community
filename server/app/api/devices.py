"""Device registration and listing endpoints."""

from __future__ import annotations

import uuid
from datetime import UTC, datetime

from fastapi import APIRouter
from fastapi.encoders import jsonable_encoder
from fastapi.responses import JSONResponse
from sqlalchemy import select

from app.api.deps import (
    CurrentDevice,
    CurrentUser,
    Db,
    IdempotencyHeader,
    replay_idempotent,
    store_idempotent,
)
from app.api.errors import ApiError
from app.db.models import Device
from app.schemas import (
    DeviceCreate,
    DeviceCreated,
    DeviceRename,
    DevicesResponse,
    DeviceSummary,
)

router = APIRouter(prefix="/v0/devices", tags=["devices"])


@router.post("", response_model=DeviceCreated)
async def create_device(
    body: DeviceCreate,
    db: Db,
    user: CurrentUser,
    # Pas de CurrentDevice ici : c'est l'endpoint qui CRÉE l'appareil, donc
    # X-Device-Id n'existe pas encore. CurrentUser suffit pour l'authentification.
    idempotency_key: IdempotencyHeader = None,
) -> JSONResponse:
    """Register one authenticated POC client."""

    # Appelé une seule fois au premier lancement de l'app/CLI.
    # Retourne device_id que le client stocke localement et envoie dans
    # X-Device-Id à toutes les requêtes suivantes.
    replay = await replay_idempotent(db, user, idempotency_key)
    if replay is not None:
        return replay

    device = Device(
        user_id=user.id,
        name=body.name,  # Ex. 'AYN Thor', 'PC bureau' — choisi par l'utilisateur au premier lancement.
        os=body.os,
        app_version=body.app_version,
    )
    db.add(device)
    await db.flush()
    payload = jsonable_encoder(DeviceCreated(device_id=device.id))
    if idempotency_key:
        await store_idempotent(db, user, idempotency_key, 201, payload)
    else:
        await db.commit()
    return JSONResponse(payload, status_code=201)


@router.get("", response_model=DevicesResponse)
async def list_devices(
    db: Db,
    user: CurrentUser,
    _device: CurrentDevice,
) -> DevicesResponse:
    """List only devices belonging to the authenticated user."""

    # Utilisé par l'écran AD-36 pour afficher les appareils et leur last_seen_at.
    # Tri par created_at ASC : les appareils les plus anciens en premier (ordre stable).
    devices = (
        await db.scalars(
            select(Device).where(Device.user_id == user.id).order_by(Device.created_at)
        )
    ).all()
    return DevicesResponse(
        devices=[
            DeviceSummary(
                id=device.id,
                name=device.name,
                os=device.os,
                # last_seen_at est null si l'appareil n'a jamais fait de requête
                # avec X-Device-Id (bug côté client ou appareil en cours de setup).
                last_seen_at=device.last_seen_at,
                revoked_at=device.revoked_at,
            )
            for device in devices
        ]
    )


async def _owned_device(db: Db, user: CurrentUser, device_id: str) -> Device:
    """Resolve one device of the authenticated user, or raise."""

    try:
        identifier = uuid.UUID(device_id)
    except ValueError as error:
        raise ApiError(422, "validation_error", "Identifiant d'appareil invalide.") from error
    device = await db.get(Device, identifier)
    if device is None:
        raise ApiError(404, "not_found", "Appareil introuvable.")
    # Même réponse que pour les autres ressources : un appareil d'un autre
    # compte n'est ni listable, ni modifiable, ni révocable.
    if device.user_id != user.id:
        raise ApiError(403, "not_owner", "Ressource appartenant à un autre compte.")
    return device


@router.patch("/{device_id}", response_model=DeviceSummary)
async def rename_device(
    device_id: str,
    body: DeviceRename,
    db: Db,
    user: CurrentUser,
    _device: CurrentDevice,
) -> DeviceSummary:
    """Rename one device for display (EXP-01)."""

    # Purement cosmétique : le nom sert à reconnaître l'origine d'une version.
    # Il n'entre dans aucune décision de synchronisation et ne touche à aucun
    # fichier — renommer un appareil ne renomme jamais une sauvegarde.
    device = await _owned_device(db, user, device_id)
    device.name = body.name
    await db.commit()
    return DeviceSummary(
        id=device.id,
        name=device.name,
        os=device.os,
        last_seen_at=device.last_seen_at,
        revoked_at=device.revoked_at,
    )


@router.post("/{device_id}/revoke", response_model=DeviceSummary)
async def revoke_device(
    device_id: str,
    db: Db,
    user: CurrentUser,
    caller: CurrentDevice,
) -> DeviceSummary:
    """Refuse further requests from one device without destroying anything."""

    device = await _owned_device(db, user, device_id)
    # Se révoquer soi-même déconnecterait l'appareil au milieu du geste, sans
    # moyen de revenir en arrière depuis cet écran. On refuse clairement plutôt
    # que de laisser l'utilisateur se verrouiller dehors.
    if device.id == caller.id:
        raise ApiError(
            409,
            "self_revocation",
            "Cet appareil ne peut pas se révoquer lui-même.",
        )
    # Rejouer une révocation ne change rien : la date reste celle de la première.
    if device.revoked_at is None:
        device.revoked_at = datetime.now(UTC)
        await db.commit()
    return DeviceSummary(
        id=device.id,
        name=device.name,
        os=device.os,
        last_seen_at=device.last_seen_at,
        revoked_at=device.revoked_at,
    )
