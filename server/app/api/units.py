"""Save-unit declaration, listing, labeling, and missing-state endpoints."""

from __future__ import annotations

import uuid
from datetime import UTC, datetime

from fastapi import APIRouter
from fastapi.encoders import jsonable_encoder
from fastapi.responses import JSONResponse
from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession

from app.api.deps import (
    CurrentDevice,
    CurrentUser,
    Db,
    IdempotencyHeader,
    replay_idempotent,
    store_idempotent,
)
from app.api.errors import ApiError
from app.core import artwork
from app.core.labels import USER, improved_label
from app.core.titles_3ds import candidates_for, infer_account_region, pick_name
from app.db.models import Device, SaveUnit, SaveVersion, User
from app.schemas import (
    EmptyRequest,
    HeadSummary,
    MissingResponse,
    UnitCreate,
    UnitPatch,
    UnitResponse,
    UnitsResponse,
    UnitSummary,
)

router = APIRouter(prefix="/v0/units", tags=["units"])


async def load_unit_for_user(
    db: AsyncSession,
    user: User,
    unit_id: object,
    *,
    for_update: bool = False,
) -> SaveUnit:
    """Load one unit and distinguish absence from ownership failure."""

    # for_update=True ajoute SELECT FOR UPDATE : utilisé avant toute écriture sur
    # l'unité (patch, missing) pour verrouiller la ligne et éviter les races.
    # Les lectures simples (list, serialize) n'ont pas besoin du verrou.
    statement = select(SaveUnit).where(SaveUnit.id == unit_id)
    if for_update:
        statement = statement.with_for_update()
    unit = await db.scalar(statement)
    if unit is None:
        raise ApiError(404, "not_found", "Unité introuvable.")
    # Vérification d'ownership explicite : un UUID d'unité valide mais appartenant
    # à un autre utilisateur retourne 403, pas 404 — on ne révèle pas l'existence.
    if unit.user_id != user.id:
        raise ApiError(403, "not_owner", "Ressource appartenant à un autre compte.")
    return unit


async def serialize_head(
    db: AsyncSession,
    unit_id: object,
    number: int,
) -> HeadSummary | None:
    """Build the shared public version summary."""

    # head_version == 0 signifie qu'aucune version n'a encore été confirmée —
    # l'unité a été déclarée mais pas encore synchronisée. On retourne None
    # plutôt qu'une erreur pour que l'UI puisse afficher "pas encore synchronisé".
    if number == 0:
        return None
    # JOIN pour récupérer le nom de l'appareil source en une seule requête.
    row = (
        await db.execute(
            select(SaveVersion, Device.name)
            .outerjoin(Device, Device.id == SaveVersion.origin_device)
            .where(
                SaveVersion.unit_id == unit_id,
                SaveVersion.number == number,
            )
        )
    ).one_or_none()
    if row is None:
        raise ApiError(404, "not_found", "Version introuvable.")
    version, device_name = row
    return HeadSummary(
        number=version.number,
        content_sha256=version.content_sha256.hex(),
        size_bytes=version.size_bytes,
        created_at=version.created_at,
        origin_device_name=device_name,
    )


async def serialize_unit(db: AsyncSession, unit: SaveUnit) -> UnitSummary:
    """Build the exact unit shape used by API endpoints #4–#6."""

    # Fonction de sérialisation commune à list_units, create_unit, patch_unit et mark_missing
    # pour garantir que toutes les réponses d'unité ont exactement la même forme.
    return UnitSummary(
        id=unit.id,
        emulator=unit.emulator,
        unit_key=unit.unit_key,
        unit_type=unit.unit_type,
        game_key=unit.game_key,
        game_label=unit.game_label,
        label_source=unit.label_source,
        head_version=unit.head_version,
        state=unit.state,
        updated_at=unit.updated_at,
        # serialize_head charge la version en tête pour inclure content_sha256, taille
        # et appareil source dans la réponse — utile pour l'écran de liste Android.
        head=await serialize_head(db, unit.id, unit.head_version),
        # Q45 : une ADRESSE de jaquette, jamais l'image. Ne bloque jamais et
        # ne lève jamais : une jaquette absente laisse la pastille de repli.
        artwork_url=artwork.artwork_url(unit.emulator, unit.game_label),
    )


@router.get("", response_model=UnitsResponse)
async def list_units(
    db: Db,
    user: CurrentUser,
    _device: CurrentDevice,
) -> UnitsResponse:
    """List all units for the current POC user without pagination."""

    # Pas de pagination au POC : on cible < 300 unités par compte (banc #10).
    # Tri par updated_at DESC : les unités modifiées récemment en premier,
    # ce qui correspond à ce que l'utilisateur voudra voir en ouvrant l'app.
    units = (
        await db.scalars(
            select(SaveUnit).where(SaveUnit.user_id == user.id).order_by(SaveUnit.updated_at.desc())
        )
    ).all()
    return UnitsResponse(units=[await serialize_unit(db, unit) for unit in units])


async def resolve_azahar_label(db: AsyncSession, user: User, body: UnitCreate) -> str:
    """M8 §6 : poser le vrai nom d'un jeu 3DS à la création de l'unité.

    Seulement si le client n'en connaît pas déjà un meilleur : un `game_label`
    qui n'est plus le `titleid_low` vient de quelque part, et l'écraser avec la
    table serait présumer que la table a toujours raison.

    Ne lève jamais. Une table absente, illisible ou muette laisse l'identifiant
    en place, ce qui est exactement le comportement d'avant M8 (§10).
    """

    # Si ce n'est pas une unité Azahar, ou si le client a déjà un libellé
    # meilleur que le titleid_low (ex. il a lu SMDH), on n'écrase rien.
    if body.emulator != "azahar" or body.game_label != body.unit_key:
        return body.game_label

    try:
        # Cherche des candidats dans la table 3DS vendorée (3ds_titles.json)
        # par titleid_low. Peut retourner plusieurs régions pour le même jeu.
        candidates = candidates_for(body.unit_key)
        if not candidates:
            return body.game_label
        # Inférer la région préférée du compte en regardant les libellés existants
        # (si l'utilisateur a 5 jeux EUR et 1 USA, il a probablement un système EUR).
        existing = (
            await db.scalars(
                select(SaveUnit).where(
                    SaveUnit.user_id == user.id,
                    SaveUnit.emulator == "azahar",
                )
            )
        ).all()
        region = infer_account_region({other.unit_key: other.game_label for other in existing})
        # Choisit le nom dans la région la plus probable, ou retombe sur le titleid.
        return pick_name(candidates, region) or body.game_label
    except (OSError, ValueError):
        # Un libellé ne fait JAMAIS échouer une passe (AD-35) :
        # si la table est corrompue ou illisible, on garde l'identifiant brut.
        return body.game_label


@router.post("", response_model=UnitResponse)
async def create_unit(
    body: UnitCreate,
    db: Db,
    user: CurrentUser,
    _device: CurrentDevice,
    idempotency_key: IdempotencyHeader = None,
) -> JSONResponse:
    """Declare a unit or return the existing unique identity."""

    # Endpoint idempotent : si l'unité (emulator, unit_key) existe déjà pour cet
    # utilisateur, on retourne l'existante (200) plutôt que de créer un doublon.
    # Le client appelle cet endpoint à chaque scan pour s'assurer que l'unité est
    # enregistrée côté serveur avant de pusher.
    replay = await replay_idempotent(db, user, idempotency_key)
    if replay is not None:
        return replay
    unit = await db.scalar(
        select(SaveUnit).where(
            SaveUnit.user_id == user.id,
            SaveUnit.emulator == body.emulator,
            SaveUnit.unit_key == body.unit_key,
        )
    )
    # status_code 201 si créée, 200 si existante — distinction importante pour
    # les clients qui veulent savoir si c'est une première synchronisation.
    status_code = 200
    if unit is None:
        status_code = 201
        unit = SaveUnit(
            user_id=user.id,
            emulator=body.emulator,
            unit_key=body.unit_key,
            unit_type=body.unit_type,
            game_key=body.game_key,
            # Pour Azahar, essaie de résoudre le nom du jeu depuis la table vendorée.
            # Pour tous les autres émulateurs, utilise le libellé envoyé par le client.
            game_label=await resolve_azahar_label(db, user, body),
        )
        db.add(unit)
        await db.flush()
    else:
        # M8 §7 : un client qui sait lire le PARAM.SFO corrige le serial posé par
        # un client plus ancien — mais jamais un nom saisi à la main, et jamais
        # dans l'autre sens.
        # improved_label() applique la règle : on ne remplace que si c'est une
        # amélioration (label 'auto' brut → label résolu), jamais si 'user'.
        better = improved_label(
            stored_label=unit.game_label,
            stored_source=unit.label_source,
            incoming_label=body.game_label,
            unit_key=unit.unit_key,
        )
        if better is not None:
            unit.game_label = better
            unit.updated_at = datetime.now(UTC)
            await db.flush()
    payload = jsonable_encoder(UnitResponse(unit=await serialize_unit(db, unit)))
    if idempotency_key:
        await store_idempotent(
            db,
            user,
            idempotency_key,
            status_code,
            payload,
        )
    else:
        await db.commit()
    return JSONResponse(payload, status_code=status_code)


@router.patch("/{unit_id}", response_model=UnitResponse)
async def patch_unit(
    unit_id: uuid.UUID,
    body: UnitPatch,
    db: Db,
    user: CurrentUser,
    _device: CurrentDevice,
    idempotency_key: IdempotencyHeader = None,
) -> JSONResponse:
    """Update only the human-readable game label."""

    # Seul game_label est modifiable par l'utilisateur — tout le reste (emulator,
    # unit_key, unit_type, game_key) est immuable une fois l'unité créée.
    replay = await replay_idempotent(db, user, idempotency_key)
    if replay is not None:
        return replay
    # FOR UPDATE : évite qu'un scan concurrent écrase le nouveau libellé pendant
    # qu'on le modifie.
    unit = await load_unit_for_user(db, user, unit_id, for_update=True)
    unit.game_label = body.game_label
    # Le renommage manuel est la seule chose qui pose 'user' : à partir de là,
    # aucune résolution automatique ne repassera dessus (§7).
    # C'est le verrou permanent : même un client plus récent avec la table 3DS
    # ne pourra plus écraser ce libellé (cas 23).
    unit.label_source = USER
    unit.updated_at = datetime.now(UTC)
    await db.flush()
    payload = jsonable_encoder(UnitResponse(unit=await serialize_unit(db, unit)))
    if idempotency_key:
        await store_idempotent(db, user, idempotency_key, 200, payload)
    else:
        await db.commit()
    return JSONResponse(payload, status_code=200)


@router.post("/{unit_id}/missing", response_model=MissingResponse)
async def mark_missing(
    unit_id: uuid.UUID,
    _body: EmptyRequest,
    db: Db,
    user: CurrentUser,
    _device: CurrentDevice,
    idempotency_key: IdempotencyHeader = None,
) -> JSONResponse:
    """Mark local disappearance without deleting any metadata or blob."""

    # Le client appelle cet endpoint quand un fichier de sauvegarde a disparu
    # localement (suppression accidentelle, déplacement, formatage).
    # Le serveur ne supprime rien (invariant I2) : la sauvegarde reste accessible
    # via l'historique. state='missing' est purement informatif pour l'UI.
    # Si > 10 unités disparaissent dans un même scan, le client passe en pause
    # sans appeler cet endpoint (protection anti-ransomware, §6.7).
    replay = await replay_idempotent(db, user, idempotency_key)
    if replay is not None:
        return replay
    unit = await load_unit_for_user(db, user, unit_id, for_update=True)
    unit.state = "missing"
    unit.updated_at = datetime.now(UTC)
    payload = jsonable_encoder(MissingResponse(state="missing"))
    if idempotency_key:
        await store_idempotent(db, user, idempotency_key, 200, payload)
    else:
        await db.commit()
    return JSONResponse(payload, status_code=200)
