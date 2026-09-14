"""Transactional compare-and-swap logic for immutable save versions."""

# Ce fichier est le cœur du serveur. Il implémente le mécanisme CAS
# (Compare-And-Swap) qui garantit qu’aucune version n’est jamais perdue
# même quand deux appareils pushent simultanément.
#
# Principe du CAS :
#   - Chaque client connaît la dernière version qu’il a synchronisée (base_version).
#   - Au confirm, le serveur vérifie : "est-ce que la tête actuelle est toujours
#     la version que le client croyait ?" Si oui → version normale. Sinon →
#     branche de conflit (les deux contenus sont préservés, I3).
#   - La vérification + la création se font dans une seule transaction avec
#     SELECT FOR UPDATE pour bloquer les accès concurrents.

from __future__ import annotations

import uuid
from dataclasses import asdict, dataclass
from datetime import UTC, datetime
from typing import Any

from fastapi.encoders import jsonable_encoder
from sqlalchemy import delete, func, select
from sqlalchemy.ext.asyncio import AsyncSession

from app.api.errors import ApiError
from app.db.models import (
    Conflict,
    Device,
    IdempotencyKey,
    PendingUpload,
    SaveUnit,
    SaveVersion,
    User,
)


@dataclass(frozen=True)
class ConfirmBody:
    """Internal representation of the unchanged public confirm body."""

    # frozen=True : ces dataclasses sont des valeurs immuables passées entre
    # fonctions — frozen empêche toute modification accidentelle après création.
    object_key: str
    content_sha256: bytes
    archive_sha256: bytes
    base_version: int  # Numéro de version que le client croyait être la tête.
    device_id: uuid.UUID
    env: dict[str, Any]
    client_mtime: datetime | None


@dataclass(frozen=True)
class VersionHead:
    """Conflict response metadata independent from HTTP serialization."""

    # Résumé d’une version pour le payload de réponse en cas de conflit.
    # Contient ce dont l’UI a besoin pour aider l’utilisateur à choisir.
    number: int
    content_sha256: str  # Hex string pour le transport JSON.
    size_bytes: int
    created_at: datetime
    origin_device_name: str | None  # Null si l’appareil a été supprimé (ON DELETE SET NULL).


@dataclass(frozen=True)
class OpenConflict:
    """The only open A/B conflict allowed for one unit."""

    # AD-24 : au plus un conflit ouvert par unité à la fois. Cette dataclass
    # porte les informations des deux branches pour les afficher à l’utilisateur.
    conflict_id: uuid.UUID
    version_a: VersionHead  # La tête au moment du conflit (version "déjà là").
    version_b: VersionHead  # La branche perdante (version "du client qui a perdu").


@dataclass(frozen=True)
class Confirmed:
    """Successful CAS result."""

    # Résultat heureux : le CAS a réussi, une nouvelle version normale a été créée.
    version: int  # Numéro de la version créée, retourné au client.


@dataclass(frozen=True)
class CasConflict:
    """Preserved losing branch and the current head."""

    # Résultat CAS raté : la tête a changé depuis base_version. Les deux versions
    # sont conservées et un conflit est ouvert pour résolution manuelle.
    conflict_id: uuid.UUID
    head: VersionHead  # Version actuellement en tête (la "gagnante" du CAS).
    yours: VersionHead  # Version du client (la "perdante", rangée en conflict_branch).


class OpenConflictBlocked(RuntimeError):
    """Reject prepare/confirm/restore while the A/B conflict is unresolved."""

    # Exception levée quand on tente une opération d’écriture alors qu’un conflit
    # est déjà ouvert — AD-24 impose la résolution séquentielle.
    def __init__(self, conflict: OpenConflict) -> None:
        super().__init__("an open conflict already exists")
        self.conflict = conflict


async def next_number(db: AsyncSession, unit_id: uuid.UUID) -> int:
    """Allocate max(number)+1 under the caller’s locked unit transaction."""

    # Doit toujours être appelé à l’intérieur d’une transaction qui tient
    # le verrou SELECT FOR UPDATE sur l’unité — sinon deux appels concurrents
    # pourraient allouer le même numéro.
    maximum = await db.scalar(
        select(func.max(SaveVersion.number)).where(SaveVersion.unit_id == unit_id)
    )
    # Si c’est la première version de l’unité, MAX() retourne NULL → on commence à 1.
    return (maximum or 0) + 1


async def get_open_conflict(
    db: AsyncSession,
    unit_id: uuid.UUID,
) -> OpenConflict | None:
    """Load the single open conflict and both immutable summaries."""

    # AD-24 garantit au plus un conflit ouvert par unité. On charge les deux
    # VersionHead en même temps pour éviter plusieurs allers-retours SQL.
    conflict = await db.scalar(
        select(Conflict).where(
            Conflict.unit_id == unit_id,
            Conflict.status == "open",
        )
    )
    if conflict is None:
        return None
    return OpenConflict(
        conflict_id=conflict.id,
        version_a=await version_head(db, unit_id, conflict.version_a),
        version_b=await version_head(db, unit_id, conflict.version_b),
    )


async def confirm_version(
    db: AsyncSession,
    user: User,
    unit_id: uuid.UUID,
    body: ConfirmBody,
    *,
    idempotency_key: str | None = None,
) -> Confirmed | CasConflict:
    """Create one normal version or preserve a concurrent branch atomically."""

    # Une seule transaction couvre tout : vérification du CAS, création de la
    # version, suppression du pending_upload, et enregistrement de l’idempotence.
    # Si quoi que ce soit échoue, la transaction est annulée et aucun état n’est
    # modifié — l’opération est rejouable proprement.
    async with db.begin():
        # SELECT FOR UPDATE verrouille la ligne de l’unité pour la durée de la
        # transaction. Les autres transactions qui veulent confirmer sur la même
        # unité attendent ici — c’est ce qui rend le CAS atomique sous concurrence.
        # Le verrou SQL ne rafraîchit pas à lui seul une instance déjà chargée
        # par la route avant le HEAD S3 (expire_on_commit=False). Relire ses
        # attributs sous verrou évite de comparer le CAS à une tête périmée.
        unit = await db.get(SaveUnit, unit_id, with_for_update=True, populate_existing=True)
        if unit is None:
            raise ApiError(404, "not_found", "Unité introuvable.")
        if unit.user_id != user.id:
            raise ApiError(
                403,
                "not_owner",
                "Ressource appartenant à un autre compte.",
            )
        # Vérifie qu’aucun conflit n’est déjà ouvert AVANT de créer une version.
        # AD-24 : on ne peut avoir qu’un seul conflit A/B ouvert à la fois.
        open_conflict = await get_open_conflict(db, unit.id)
        if open_conflict is not None:
            raise OpenConflictBlocked(open_conflict)

        # Validation de sécurité de l’object_key : le client ne doit pas pouvoir
        # confirmer un objet qui appartient à une autre unité ou un autre utilisateur
        # (le préfixe encode user_id et unit_id).
        object_prefix = f"u/{user.id}/{unit.id}/"
        if not body.object_key.startswith(object_prefix) or not body.object_key.endswith(
            ".tar.zst"
        ):
            raise ApiError(
                422,
                "validation_error",
                "Clé d’objet invalide pour cette unité.",
            )
        # On charge le pending_upload avec FOR UPDATE pour l’atomicité : si deux
        # confirms arrivent pour le même object_key, le second trouvera le pending
        # déjà supprimé et obtiendra un 422 "unknown_upload".
        pending = await db.get(
            PendingUpload,
            body.object_key,
            with_for_update=True,
            populate_existing=True,
        )
        if pending is None:
            # Le pending n’existe pas : soit le client n’a pas appelé prepare,
            # soit il a déjà été consommé (double confirm), soit le GC l’a supprimé.
            raise ApiError(
                422,
                "unknown_upload",
                "Upload préparé introuvable.",
            )
        # Vérification croisée : les checksums du confirm doivent correspondre
        # exactement à ceux enregistrés au prepare. Protège contre un client
        # bugué qui confirmerait le mauvais objet.
        if (
            pending.unit_id != unit.id
            or pending.content_sha256 != body.content_sha256
            or pending.archive_sha256 != body.archive_sha256
            or body.object_key != f"{object_prefix}{pending.content_sha256.hex()}.tar.zst"
        ):
            raise ApiError(
                422,
                "checksum_mismatch",
                "Les métadonnées ne correspondent pas à l’upload préparé.",
            )

        # === DÉCISION CAS ===
        # C’est ici que tout se joue : si base_version correspond à la tête actuelle,
        # le push "passe" (version normale). Sinon, un autre appareil a poussé
        # entre-temps → branche de conflit (les deux versions sont conservées, I3).
        number = await next_number(db, unit.id)
        kind = "normal" if body.base_version == unit.head_version else "conflict_branch"
        if kind == "conflict_branch" and unit.head_version == 0:
            # head_version == 0 signifie qu’aucune version n’existe encore sur le
            # serveur. Un conflit est impossible dans ce cas : si deux clients pushent
            # la même unité "vierge", le premier gagne et le second obtient un conflit
            # normal. Mais base_version != 0 && head_version == 0 est un état incohérent
            # qui indique un bug côté client.
            raise ApiError(
                422,
                "validation_error",
                "La version de base est invalide pour une unité vide.",
            )

        # Crée la version immuable. Les tailles viennent du pending (vérifiées
        # par HEAD S3 avant cette transaction) plutôt que du body du client.
        version = SaveVersion(
            unit_id=unit.id,
            number=number,
            parent_number=body.base_version or None,
            content_sha256=body.content_sha256,
            archive_sha256=body.archive_sha256,
            size_bytes=pending.size_bytes,
            archive_bytes=pending.archive_bytes,
            object_key=body.object_key,
            origin_device=body.device_id,
            env=body.env,
            client_mtime=body.client_mtime,
            kind=kind,
        )
        db.add(version)
        # Supprime le pending_upload dans la même transaction : si le commit échoue,
        # le pending reste et le client peut retenter. Si le commit réussit, le
        # pending est proprement consommé.
        await db.execute(delete(PendingUpload).where(PendingUpload.object_key == body.object_key))

        if kind == "normal":
            # CAS réussi : on avance la tête de l’unité et on remet l’état à ‘active’
            # (au cas où l’unité était ‘missing’ — un push réussit la ressuscite).
            unit.head_version = number
            unit.state = "active"
            unit.updated_at = datetime.now(UTC)
            await db.flush()
            result: Confirmed | CasConflict = Confirmed(version=number)
        else:
            # CAS raté : on crée le conflit SANS changer head_version.
            # La tête reste sur l’ancienne version ; la nouvelle est rangée en
            # ‘conflict_branch’ et attend la résolution manuelle de l’utilisateur.
            conflict = Conflict(
                unit_id=unit.id,
                version_a=unit.head_version,  # La version qui "gagne" par défaut.
                version_b=number,  # La nouvelle version "perdante".
            )
            db.add(conflict)
            await db.flush()
            result = CasConflict(
                conflict_id=conflict.id,
                head=await version_head(db, unit.id, unit.head_version),
                yours=await version_head(db, unit.id, number),
            )

        # Enregistrement de l’idempotence DANS la même transaction : si le commit
        # échoue, la clé d’idempotence n’est pas stockée → un retry recréera
        # proprement la version. Si le commit réussit, la clé est disponible pour
        # les replays futurs du même client.
        if idempotency_key:
            status_code, payload = confirm_result_response(result)
            db.add(
                IdempotencyKey(
                    key=idempotency_key,
                    user_id=user.id,
                    status_code=status_code,
                    response=payload,
                )
            )
        return result


def confirm_result_response(
    result: Confirmed | CasConflict,
) -> tuple[int, dict[str, Any]]:
    """Return the exact cached/HTTP response for a CAS result."""

    # Utilisé à deux endroits : pour construire la réponse HTTP directe,
    # et pour sérialiser le payload à stocker dans idempotency_keys.
    # Le format doit être identique dans les deux cas.
    if isinstance(result, Confirmed):
        return 201, {"version": result.version}
    payload = {
        "error": {
            "code": "cas_conflict",
            "message": "La tête a changé depuis la version de base.",
            "details": {},
        },
        # Les détails du conflit sont dans le corps du 409, pas seulement dans
        # l’enveloppe d’erreur — le client en a besoin pour afficher les deux branches.
        "conflict_id": result.conflict_id,
        "head": asdict(result.head),
        "yours": asdict(result.yours),
    }
    return 409, jsonable_encoder(payload)


async def version_head(
    db: AsyncSession,
    unit_id: uuid.UUID,
    number: int,
) -> VersionHead:
    """Build the exact conflict summary while inside a transaction."""

    # JOIN avec Device pour récupérer le nom de l’appareil source en une seule requête.
    # OUTER JOIN : si l’appareil a été supprimé (ON DELETE SET NULL), origin_device
    # est NULL et Device.name sera NULL — affiché comme "appareil inconnu" dans l’UI.
    row = (
        await db.execute(
            select(SaveVersion, Device.name)
            .outerjoin(Device, Device.id == SaveVersion.origin_device)
            .where(
                SaveVersion.unit_id == unit_id,
                SaveVersion.number == number,
            )
        )
    ).one()
    version, device_name = row
    return VersionHead(
        number=version.number,
        # content_sha256 est stocké en bytes dans la DB ; on le convertit en
        # hex string pour le transport JSON (lisible et standard).
        content_sha256=version.content_sha256.hex(),
        size_bytes=version.size_bytes,
        created_at=version.created_at,
        origin_device_name=device_name,
    )
