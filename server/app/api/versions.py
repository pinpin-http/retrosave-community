"""Version prepare, history, download, and restore endpoints."""

# Ce fichier implémente le flux en 3 étapes pour pousser une version :
#
#   1. prepare  → le client annonce le contenu qu’il va uploader.
#                 Le serveur vérifie les limites, détecte les doublons,
#                 et retourne une URL présignée PUT vers S3.
#
#   2. PUT S3   → le client uploade l’archive directement vers S3
#                 (sans passer par le serveur).
#
#   3. confirm  → le client dit "j’ai uploadé". Le serveur vérifie que
#                 l’objet est bien dans S3 (HEAD), puis exécute le CAS
#                 transactionnel (core/sync.py).
#
# Ce découplage permet au serveur de rester léger (il ne voit jamais
# les octets de sauvegarde) tout en garantissant l’intégrité (AD-06).

from __future__ import annotations

import uuid
from datetime import UTC, datetime

from fastapi import APIRouter
from fastapi.encoders import jsonable_encoder
from fastapi.responses import JSONResponse
from sqlalchemy import func, select
from sqlalchemy.dialects.postgresql import insert

from app.api.deps import (
    CurrentDevice,
    CurrentUser,
    Db,
    IdempotencyHeader,
    replay_idempotent,
    store_idempotent,
)
from app.api.errors import ApiError
from app.api.units import load_unit_for_user
from app.config import get_settings
from app.core.sync import (
    ConfirmBody,
    OpenConflict,
    OpenConflictBlocked,
    confirm_result_response,
    confirm_version,
    get_open_conflict,
    next_number,
)
from app.db.models import Device, PendingUpload, SaveVersion
from app.s3 import presign_get, presign_put, verify_uploaded_object
from app.schemas import (
    DownloadResponse,
    DuplicateVersion,
    RestoreRequest,
    UploadPrepared,
    UploadTarget,
    VersionConfirm,
    VersionCreated,
    VersionPrepare,
    VersionsResponse,
    VersionSummary,
)

router = APIRouter(prefix="/v0/units", tags=["versions"])


async def open_conflict_response(
    db: Db,
    user: CurrentUser,
    idempotency_key: str | None,
    conflict: OpenConflict,
) -> JSONResponse:
    """Return and optionally cache the AD-24 sequential-conflict response."""

    # Réponse commune pour tous les endpoints qui refusent quand un conflit est ouvert.
    # On la met en cache idempotent pour que les retries du client reçoivent le même 409.
    payload = ApiError(
        409,
        "open_conflict",
        "Un conflit est déjà ouvert pour cette unité.",
        # extra=conflict : les détails du conflit (version_a, version_b) sont ajoutés
        # à la racine du JSON pour que le client puisse afficher "résolvez d’abord".
        extra=jsonable_encoder(conflict),
    ).payload()
    if idempotency_key:
        await store_idempotent(
            db,
            user,
            idempotency_key,
            409,
            payload,
        )
    return JSONResponse(payload, status_code=409)


@router.post(
    "/{unit_id}/versions:prepare",
    response_model=DuplicateVersion | UploadPrepared,
)
async def prepare_version(
    unit_id: uuid.UUID,
    body: VersionPrepare,
    db: Db,
    user: CurrentUser,
    _device: CurrentDevice,
    idempotency_key: IdempotencyHeader = None,
) -> JSONResponse:
    """Deduplicate the current head or reserve a direct-upload object key."""

    # Étape 1 du flux push. Le client annonce ce qu’il va uploader (checksums + tailles)
    # AVANT de l’uploader — le serveur peut ainsi refuser sans gaspiller la bande passante.
    replay = await replay_idempotent(db, user, idempotency_key)
    if replay is not None:
        return replay
    # for_update=True : on verrouille l’unité pendant la vérification du conflit
    # et l’insertion du pending_upload pour éviter des races entre deux prepare concurrents.
    unit = await load_unit_for_user(db, user, unit_id, for_update=True)
    # AD-24 : refus immédiat si un conflit est déjà ouvert sur cette unité.
    # Le client doit d’abord résoudre le conflit avant de pousser de nouvelles versions.
    open_conflict = await get_open_conflict(db, unit.id)
    if open_conflict is not None:
        return await open_conflict_response(
            db,
            user,
            idempotency_key,
            open_conflict,
        )
    # Vérification de taille avant tout : 256 Mio max (invariant I1 + §7.3).
    if body.size > get_settings().max_unit_bytes:
        raise ApiError(
            413,
            "payload_too_large",
            "L’unité dépasse la taille maximale.",
        )
    # Déduplication (AD-10) : si le client pousse exactement le même contenu que
    # la tête actuelle (même content_sha256), pas besoin d’uploader ni de créer
    # une nouvelle version — on retourne {duplicate:true, version:N}.
    # Cas concret : deux appareils en ligne, l’un pousse, l’autre voit la tête
    # et appelle prepare juste après : dédup évite une version redondante.
    if unit.head_version:
        head_content = await db.scalar(
            select(SaveVersion.content_sha256).where(
                SaveVersion.unit_id == unit.id,
                SaveVersion.number == unit.head_version,
            )
        )
        if head_content == bytes.fromhex(body.content_sha256):
            payload = jsonable_encoder(DuplicateVersion(duplicate=True, version=unit.head_version))
            if idempotency_key:
                await store_idempotent(
                    db,
                    user,
                    idempotency_key,
                    200,
                    payload,
                )
            else:
                await db.commit()
            return JSONResponse(payload, status_code=200)

    # L’object_key est adressé par contenu (AD-10) : u/<user>/<unit>/<sha256>.tar.zst.
    # Même contenu = même clé, ce qui permet aux restores de réutiliser l’objet
    # S3 existant sans copie (économie de stockage).
    object_key = f"u/{user.id}/{unit.id}/{body.content_sha256}.tar.zst"
    # UPSERT : si le client retente un prepare pour le même contenu (timeout réseau,
    # retry), on met à jour le slot existant plutôt que d’en créer un deuxième.
    # La date created_at est rafraîchie pour éviter un GC prématuré sur un retried prepare.
    await db.execute(
        insert(PendingUpload)
        .values(
            object_key=object_key,
            unit_id=unit.id,
            content_sha256=bytes.fromhex(body.content_sha256),
            archive_sha256=bytes.fromhex(body.archive_sha256),
            size_bytes=body.size,
            archive_bytes=body.archive_bytes,
        )
        .on_conflict_do_update(
            index_elements=[PendingUpload.object_key],
            set_={
                "unit_id": unit.id,
                "content_sha256": bytes.fromhex(body.content_sha256),
                "archive_sha256": bytes.fromhex(body.archive_sha256),
                "size_bytes": body.size,
                "archive_bytes": body.archive_bytes,
                "created_at": func.now(),
            },
        )
    )
    # Génère l’URL présignée PUT (valide presign_expires_s secondes, défaut 15 min).
    # Le client doit uploader directement vers cette URL — le serveur ne verra jamais
    # les octets de l’archive (AD-06).
    url, expires_at = await presign_put(object_key)
    payload = jsonable_encoder(
        UploadPrepared(
            upload=UploadTarget(
                url=url,
                object_key=object_key,
                expires_at=expires_at,
            )
        )
    )
    if idempotency_key:
        await store_idempotent(db, user, idempotency_key, 200, payload)
    else:
        await db.commit()
    return JSONResponse(payload, status_code=200)


@router.post("/{unit_id}/versions:confirm", response_model=VersionCreated)
async def confirm_uploaded_version(
    unit_id: uuid.UUID,
    body: VersionConfirm,
    db: Db,
    user: CurrentUser,
    device: CurrentDevice,
    idempotency_key: IdempotencyHeader = None,
) -> JSONResponse:
    """Verify the prepared object then execute the transactional CAS."""

    # Étape 3 du flux push. Le client a uploadé l’archive vers S3 et appelle
    # confirm pour "valider" cet upload et créer la version en base.
    replay = await replay_idempotent(db, user, idempotency_key)
    if replay is not None:
        return replay

    # Chargement préliminaire sans FOR UPDATE : on vérifie juste l’ownership
    # avant de faire le HEAD S3 (opération réseau). Le FOR UPDATE viendra plus tard
    # dans confirm_version() pour la transaction CAS réelle.
    unit = await load_unit_for_user(db, user, unit_id)
    open_conflict = await get_open_conflict(db, unit.id)
    if open_conflict is not None:
        return await open_conflict_response(
            db,
            user,
            idempotency_key,
            open_conflict,
        )

    # Validation de l’object_key avant tout : le client ne doit pas pouvoir
    # confirmer un objet qui n’appartient pas à cette unité/utilisateur.
    object_prefix = f"u/{user.id}/{unit.id}/"
    if not body.object_key.startswith(object_prefix) or not body.object_key.endswith(".tar.zst"):
        raise ApiError(
            422,
            "validation_error",
            "Clé d’objet invalide pour cette unité.",
        )
    # Charge le pending_upload pour récupérer archive_bytes attendu AVANT de commiter.
    # On a besoin de cette valeur après le commit pour appeler verify_uploaded_object().
    pending = await db.get(PendingUpload, body.object_key)
    if pending is None:
        raise ApiError(
            422,
            "unknown_upload",
            "Upload préparé introuvable.",
        )
    if (
        pending.unit_id != unit.id
        or pending.content_sha256 != bytes.fromhex(body.content_sha256)
        or pending.archive_sha256 != bytes.fromhex(body.archive_sha256)
        or body.object_key != f"{object_prefix}{pending.content_sha256.hex()}.tar.zst"
    ):
        raise ApiError(
            422,
            "checksum_mismatch",
            "Les métadonnées ne correspondent pas à l’upload préparé.",
        )
    # On capture expected_archive_bytes AVANT le commit car après le commit la session
    # SQLAlchemy n’a plus de transaction active et accéder à pending.archive_bytes
    # pourrait déclencher un lazy load (ou une erreur en mode async).
    expected_archive_bytes = pending.archive_bytes
    await db.commit()

    # HEAD S3 AVANT la transaction CAS : on vérifie que l’objet existe réellement
    # dans S3 et a la bonne taille. Cette vérification est hors transaction pour
    # éviter de tenir un verrou PostgreSQL pendant un appel réseau (qui peut prendre
    # plusieurs secondes). Si le HEAD échoue → 422 sans avoir ouvert de transaction.
    await verify_uploaded_object(body.object_key, expected_archive_bytes)
    try:
        # Exécute la transaction CAS dans core/sync.py. Peut retourner :
        # - Confirmed(version=N)           → 201, version normale créée
        # - CasConflict(conflict_id, ...)  → 409, branche de conflit créée
        result = await confirm_version(
            db,
            user,
            unit.id,
            ConfirmBody(
                object_key=body.object_key,
                content_sha256=bytes.fromhex(body.content_sha256),
                archive_sha256=bytes.fromhex(body.archive_sha256),
                base_version=body.base_version,
                device_id=device.id,
                env=body.env,
                client_mtime=body.client_mtime,
            ),
            idempotency_key=idempotency_key,
        )
    except OpenConflictBlocked as error:
        # Un conflit s’est ouvert ENTRE le premier check et la transaction CAS
        # (race condition possible si deux clients confirment en parallèle).
        # On retourne la même réponse 409 qu’au-dessus.
        return await open_conflict_response(
            db,
            user,
            idempotency_key,
            error.conflict,
        )
    status_code, payload = confirm_result_response(result)
    return JSONResponse(payload, status_code=status_code)


@router.get("/{unit_id}/versions", response_model=VersionsResponse)
async def list_versions(
    unit_id: uuid.UUID,
    db: Db,
    user: CurrentUser,
    _device: CurrentDevice,
) -> VersionsResponse:
    """Return immutable history in descending server-number order."""

    unit = await load_unit_for_user(db, user, unit_id)
    # Toutes les versions, incluant conflict_branch et restore — l’écran de détail
    # les affiche toutes pour permettre la restauration de n’importe quelle version.
    # Tri par number DESC : la version la plus récente en premier.
    rows = (
        await db.execute(
            select(SaveVersion, Device.name)
            .outerjoin(Device, Device.id == SaveVersion.origin_device)
            .where(SaveVersion.unit_id == unit.id)
            .order_by(SaveVersion.number.desc())
        )
    ).all()
    return VersionsResponse(
        versions=[
            VersionSummary(
                number=version.number,
                kind=version.kind,
                size_bytes=version.size_bytes,
                content_sha256=version.content_sha256.hex(),
                origin_device_name=device_name,
                client_mtime=version.client_mtime,
                created_at=version.created_at,
                parent_number=version.parent_number,
            )
            for version, device_name in rows
        ],
        head_version=unit.head_version,
    )


@router.get(
    "/{unit_id}/versions/{number}/download",
    response_model=DownloadResponse,
)
async def download_version(
    unit_id: uuid.UUID,
    number: int,
    db: Db,
    user: CurrentUser,
    _device: CurrentDevice,
) -> DownloadResponse:
    """Return a short-lived private download URL and integrity metadata."""

    unit = await load_unit_for_user(db, user, unit_id)
    version = await db.scalar(
        select(SaveVersion).where(
            SaveVersion.unit_id == unit.id,
            SaveVersion.number == number,
        )
    )
    if version is None:
        raise ApiError(404, "not_found", "Version introuvable.")
    # L’URL GET présignée est valide presign_expires_s secondes (défaut 15 min).
    # On retourne aussi archive_sha256 et content_sha256 pour que le client puisse
    # vérifier l’intégrité après le téléchargement (§6.5 : vérification bout-en-bout).
    url, expires_at = await presign_get(version.object_key)
    return DownloadResponse(
        url=url,
        archive_sha256=version.archive_sha256.hex(),
        content_sha256=version.content_sha256.hex(),
        archive_bytes=version.archive_bytes,
        expires_at=expires_at,
    )


@router.post("/{unit_id}/restore", response_model=VersionCreated)
async def restore_version(
    unit_id: uuid.UUID,
    body: RestoreRequest,
    db: Db,
    user: CurrentUser,
    device: CurrentDevice,
    idempotency_key: IdempotencyHeader = None,
) -> JSONResponse:
    """Create a new head pointing to an existing immutable object."""

    # Restore = créer une NOUVELLE version (kind=’restore’) dont l’object_key
    # pointe vers l’archive d’une version ancienne. Pas de copie S3 (AD-10).
    # Les clients reçoivent ce restore comme un PULL normal au prochain scan.
    replay = await replay_idempotent(db, user, idempotency_key)
    if replay is not None:
        return replay
    # FOR UPDATE : on va modifier head_version, besoin du verrou pour éviter
    # les races avec un confirm concurrent.
    unit = await load_unit_for_user(db, user, unit_id, for_update=True)
    open_conflict = await get_open_conflict(db, unit.id)
    if open_conflict is not None:
        # Ne peut pas restorer pendant un conflit ouvert — AD-24.
        return await open_conflict_response(
            db,
            user,
            idempotency_key,
            open_conflict,
        )
    # Charge la version source pour copier ses métadonnées (object_key, checksums, tailles).
    source = await db.scalar(
        select(SaveVersion).where(
            SaveVersion.unit_id == unit.id,
            SaveVersion.number == body.version,
        )
    )
    if source is None:
        raise ApiError(404, "not_found", "Version introuvable.")

    number = await next_number(db, unit.id)
    restored = SaveVersion(
        unit_id=unit.id,
        number=number,
        # parent_number = ancienne tête : garde la trace de l’historique de navigation.
        parent_number=unit.head_version or None,
        # Toutes les métadonnées de contenu sont copiées depuis la version source —
        # même object_key S3, mêmes checksums. Aucune copie d’objet S3 (AD-10).
        content_sha256=source.content_sha256,
        archive_sha256=source.archive_sha256,
        size_bytes=source.size_bytes,
        archive_bytes=source.archive_bytes,
        object_key=source.object_key,
        origin_device=device.id,
        env={},  # Pas d’env pour un restore — l’env source appartient au push original.
        client_mtime=None,
        kind="restore",
    )
    db.add(restored)
    # La nouvelle tête pointe sur la version restore, pas sur la source.
    # Les clients verront head_version=N (le restore) et téléchargeront la même
    # archive que la version source (même object_key).
    unit.head_version = number
    unit.updated_at = datetime.now(UTC)
    await db.flush()
    payload = jsonable_encoder(VersionCreated(version=number))
    if idempotency_key:
        await store_idempotent(db, user, idempotency_key, 201, payload)
    else:
        await db.commit()
    return JSONResponse(payload, status_code=201)
