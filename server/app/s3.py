"""S3-compatible clients used for private object storage."""

# AD-25 : ce fichier expose DEUX clients boto3 distincts avec des rôles séparés.
#
# Pourquoi deux clients ?
#   Le protocole SigV4 (authentification AWS/S3) intègre l’URL de l’endpoint dans
#   la signature cryptographique de l’URL présignée. Si on signait les URLs avec
#   l’adresse interne (‘http://minio:9000’), les clients Android (qui sont hors Docker)
#   recevraient des URLs avec un hôte ‘minio’ qu’ils ne peuvent pas résoudre.
#
#   Solution :
#   - get_s3_client()  → endpoint interne (minio:9000)  → HEAD, GC, healthcheck
#   - get_s3_signer()  → endpoint public (IP LAN ou R2) → génère les URLs présignées
#     remises aux clients Android et CLI.

import asyncio
from datetime import UTC, datetime, timedelta
from functools import lru_cache
from typing import Any

import boto3
from botocore.config import Config
from botocore.exceptions import ClientError

from app.api.errors import ApiError
from app.config import get_settings


@lru_cache
def get_s3_client() -> Any:
    """Build the operations-only client for HEAD, health, and GC."""

    # Client interne : utilisé uniquement pour les opérations côté serveur
    # (vérifier qu’un objet existe, mesurer sa taille, le supprimer).
    # Ne génère jamais d’URL envoyée aux clients.
    settings = get_settings()
    return _build_client(settings.s3_endpoint)


@lru_cache
def get_s3_signer() -> Any:
    """Build the local-only signer for client-facing PUT/GET URLs."""

    # Client de signature : génère des URLs présignées PUT/GET qui seront envoyées
    # aux clients Android et CLI. Configuré avec l’endpoint PUBLIC pour que les
    # URLs générées soient joignables depuis l’extérieur de Docker.
    # generate_presigned_url() est une opération LOCALE (pas d’appel réseau),
    # donc ce client n’a pas besoin d’accéder à MinIO.
    return _build_client(get_settings().s3_signing_endpoint)


def _build_client(endpoint_url: str) -> Any:
    settings = get_settings()
    # path = adressage par chemin : http://minio:9000/bucket/key
    # auto = adressage virtuel : http://bucket.minio:9000/key (requis par AWS S3 réel)
    # MinIO et R2 fonctionnent en mode ‘path’, donc on force ce mode.
    addressing_style = "path" if settings.s3_force_path_style else "auto"
    return boto3.client(
        "s3",
        endpoint_url=endpoint_url,
        region_name=settings.s3_region,
        aws_access_key_id=settings.s3_access_key_id,
        aws_secret_access_key=settings.s3_secret_access_key,
        config=Config(s3={"addressing_style": addressing_style}),
    )


async def check_storage() -> bool:
    """Verify that the configured private bucket exists."""

    # asyncio.to_thread() exécute l’appel boto3 (synchrone) dans un thread séparé
    # pour ne pas bloquer la boucle d’événements asyncio. Nécessaire car boto3
    # n’a pas d’API async native.
    settings = get_settings()
    await asyncio.to_thread(
        get_s3_client().head_bucket,
        Bucket=settings.s3_bucket,
    )
    return True


async def presign_put(object_key: str) -> tuple[str, datetime]:
    """Create a short-lived direct PUT URL for one private object."""

    settings = get_settings()
    # Utilise get_s3_signer() (endpoint public) pour que l’URL retournée au client
    # pointe vers une adresse joignable depuis son réseau.
    url = await asyncio.to_thread(
        get_s3_signer().generate_presigned_url,
        "put_object",
        Params={"Bucket": settings.s3_bucket, "Key": object_key},
        ExpiresIn=settings.presign_expires_s,
    )
    # On calcule expires_at localement plutôt que de parser l’URL, pour éviter
    # la fragilité du parsing d’URL S3. L’approximation est suffisante pour l’UI.
    return url, datetime.now(UTC) + timedelta(seconds=settings.presign_expires_s)


async def presign_get(object_key: str) -> tuple[str, datetime]:
    """Create a short-lived direct GET URL for one private object."""

    settings = get_settings()
    url = await asyncio.to_thread(
        get_s3_signer().generate_presigned_url,
        "get_object",
        Params={"Bucket": settings.s3_bucket, "Key": object_key},
        ExpiresIn=settings.presign_expires_s,
    )
    return url, datetime.now(UTC) + timedelta(seconds=settings.presign_expires_s)


async def head_object_size(object_key: str) -> int:
    """Return the uploaded archive size without downloading its contents."""

    # HEAD (pas GET) : récupère uniquement les métadonnées de l’objet (taille,
    # ETag, etc.) sans télécharger son contenu — essentiel pour des archives
    # pouvant atteindre 256 Mo.
    response = await asyncio.to_thread(
        get_s3_client().head_object,
        Bucket=get_settings().s3_bucket,
        Key=object_key,
    )
    return int(response["ContentLength"])


async def verify_uploaded_object(
    object_key: str,
    expected_archive_bytes: int,
) -> None:
    """Verify object presence and size before the metadata transaction."""

    # Cette vérification est faite AVANT d’ouvrir la transaction CAS pour deux raisons :
    # 1. Éviter de tenir un verrou PostgreSQL (SELECT FOR UPDATE) pendant un appel S3.
    # 2. Échouer tôt : si l’objet est absent, pas la peine d’ouvrir une transaction.
    try:
        actual_archive_bytes = await head_object_size(object_key)
    except ClientError as error:
        # Les différents backends S3 retournent des codes d’erreur différents pour
        # "objet absent" : MinIO retourne "404", R2 retourne "NoSuchKey", d’autres
        # peuvent retourner "NotFound". On normalise ici.
        error_code = str(error.response.get("Error", {}).get("Code", ""))
        if error_code not in {"404", "NoSuchKey", "NotFound"}:
            raise  # Erreur réseau ou d’auth — on propage.
        raise ApiError(
            422,
            "checksum_mismatch",
            "Objet uploadé absent.",
        ) from error
    # Vérifie la taille : si le client a uploadé un fichier tronqué (crash pendant
    # l’upload), la taille sera différente de ce que le prepare a enregistré.
    if actual_archive_bytes != expected_archive_bytes:
        raise ApiError(
            422,
            "checksum_mismatch",
            "Taille de l’archive uploadée incorrecte.",
            details={
                "expected_archive_bytes": expected_archive_bytes,
                "actual_archive_bytes": actual_archive_bytes,
            },
        )


async def delete_object(object_key: str) -> None:
    """Delete an object; callers are restricted to the orphan GC job."""

    # Appelé UNIQUEMENT par le GC des uploads orphelins (jobs.py).
    # Jamais en réponse à une requête utilisateur — invariant I2 (aucune API
    # de suppression d’objet exposée au POC).
    await asyncio.to_thread(
        get_s3_client().delete_object,
        Bucket=get_settings().s3_bucket,
        Key=object_key,
    )
