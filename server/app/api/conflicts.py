"""Conflict listing and explicit winner resolution endpoints."""

from __future__ import annotations

import uuid
from datetime import UTC, datetime

from fastapi import APIRouter, Query
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
from app.api.units import serialize_head
from app.db.models import Conflict, SaveUnit
from app.schemas import (
    ConflictsResponse,
    ConflictSummary,
    ResolveRequest,
    ResolveResponse,
)

router = APIRouter(prefix="/v0/conflicts", tags=["conflicts"])


@router.get("", response_model=ConflictsResponse)
async def list_conflicts(
    db: Db,
    user: CurrentUser,
    _device: CurrentDevice,
    # Le paramètre ?open=1 est obligatoire dans le contrat API (§7.7) pour que
    # les clients futurs puissent lister les conflits résolus sans casser l'API.
    # Au POC on ne supporte que open=1 (ge=1, le=1), d'où le `del open_only`
    # juste après — le paramètre est validé (refusé si absent ou ≠ 1) mais inutilisé.
    open_only: int = Query(1, alias="open", ge=1, le=1),
) -> ConflictsResponse:
    """List open conflicts for units owned by the authenticated user."""

    del open_only  # La valeur est toujours 1 (contrainte ge=1 le=1) ; la condition
    # est encodée dans la requête SQL ci-dessous.
    # JOIN SaveUnit pour filtrer par user et récupérer game_label en une requête.
    rows = (
        await db.execute(
            select(Conflict, SaveUnit)
            .join(SaveUnit, SaveUnit.id == Conflict.unit_id)
            .where(
                SaveUnit.user_id == user.id,
                Conflict.status == "open",
            )
            .order_by(Conflict.created_at)  # Les plus anciens en premier : à résoudre en ordre.
        )
    ).all()
    conflicts: list[ConflictSummary] = []
    for conflict, unit in rows:
        # serialize_head charge les métadonnées de chaque version pour l'affichage
        # (taille, appareil source, date) — aide l'utilisateur à choisir.
        version_a = await serialize_head(db, unit.id, conflict.version_a)
        version_b = await serialize_head(db, unit.id, conflict.version_b)
        if version_a is None or version_b is None:
            raise ApiError(404, "not_found", "Version de conflit introuvable.")
        conflicts.append(
            ConflictSummary(
                id=conflict.id,
                unit_id=unit.id,
                unit_label=unit.game_label,  # Affiché dans l'écran "résoudre le conflit".
                version_a=version_a,
                version_b=version_b,
                created_at=conflict.created_at,
            )
        )
    return ConflictsResponse(conflicts=conflicts)


@router.post("/{conflict_id}/resolve", response_model=ResolveResponse)
async def resolve_conflict(
    conflict_id: uuid.UUID,
    body: ResolveRequest,
    db: Db,
    user: CurrentUser,
    _device: CurrentDevice,
    idempotency_key: IdempotencyHeader = None,
) -> JSONResponse:
    """Select one preserved version as head without deleting the loser."""

    # L'utilisateur a choisi quelle version garder (A ou B). On fait avancer
    # head_version vers le gagnant — la version perdante reste dans l'historique
    # (invariant I3 : rien n'est jamais supprimé).
    replay = await replay_idempotent(db, user, idempotency_key)
    if replay is not None:
        return replay
    row = (
        await db.execute(
            select(Conflict, SaveUnit)
            .join(SaveUnit, SaveUnit.id == Conflict.unit_id)
            .where(Conflict.id == conflict_id)
            # with_for_update(of=Conflict) : verrouille uniquement la ligne Conflict,
            # pas l'unité entière. On modifie aussi SaveUnit mais le verrou sur Conflict
            # suffit à sérialiser les résolutions concurrentes du même conflit.
            .with_for_update(of=Conflict)
        )
    ).one_or_none()
    if row is None:
        raise ApiError(404, "not_found", "Conflit introuvable.")
    conflict, unit = row
    if unit.user_id != user.id:
        raise ApiError(403, "not_owner", "Ressource appartenant à un autre compte.")
    # Vérifie que le gagnant proposé est bien l'une des deux versions du conflit.
    # Protège contre un client qui enverrait un numéro de version arbitraire.
    if body.winner not in (conflict.version_a, conflict.version_b):
        raise ApiError(
            422,
            "validation_error",
            "Le gagnant doit être une version du conflit.",
        )
    if conflict.status == "resolved":
        # Idempotence manuelle : si le conflit est déjà résolu avec le MÊME gagnant,
        # on retourne 200 comme si on venait de le résoudre (safe pour les retries).
        if conflict.winner == body.winner:
            payload = jsonable_encoder(
                ResolveResponse(
                    unit_id=unit.id,
                    head_version=body.winner,
                )
            )
            return JSONResponse(payload, status_code=200)
        # Résolu avec un AUTRE gagnant : c'est une vraie divergence, pas un retry.
        # 410 Gone : la ressource a changé d'état de façon irréversible.
        raise ApiError(
            410,
            "already_resolved",
            "Ce conflit a déjà été résolu avec un autre gagnant.",
        )

    conflict.status = "resolved"
    conflict.winner = body.winner
    conflict.resolved_at = datetime.now(UTC)
    # La tête de l'unité avance vers le gagnant. Les clients verront ce changement
    # au prochain GET /v0/units et téléchargeront la version choisie.
    unit.head_version = body.winner
    unit.updated_at = datetime.now(UTC)
    # La version perdante (l'autre branche) reste dans save_versions avec
    # kind='conflict_branch' — accessible dans l'historique, jamais supprimée (I3).
    payload = jsonable_encoder(ResolveResponse(unit_id=unit.id, head_version=body.winner))
    if idempotency_key:
        await store_idempotent(db, user, idempotency_key, 200, payload)
    else:
        await db.commit()
    return JSONResponse(payload, status_code=200)
