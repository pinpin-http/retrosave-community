"""Hourly maintenance jobs owned by the FastAPI process."""

from __future__ import annotations

import logging
from datetime import UTC, datetime, timedelta

from apscheduler.schedulers.asyncio import AsyncIOScheduler
from sqlalchemy import delete, exists, select

from app.config import get_settings
from app.db.models import IdempotencyKey, PendingUpload, SaveVersion
from app.db.session import get_session_factory
from app.s3 import delete_object

logger = logging.getLogger(__name__)


async def gc_pending_uploads() -> None:
    """Delete only unreferenced objects whose pending lease is older than TTL."""

    # Scénario nettoyé par ce job :
    # 1. Le client appelle prepare → un slot pending_uploads est créé + un objet S3 est uploadé.
    # 2. Le client crashe, perd sa connexion ou abandonne avant d'appeler confirm.
    # Résultat : un objet S3 "orphelin" (non référencé par aucune version) + un slot pending.
    # Ce job nettoie les deux après pending_upload_ttl_h heures (défaut : 24 h).
    cutoff = datetime.now(UTC) - timedelta(hours=get_settings().pending_upload_ttl_h)
    async with get_session_factory()() as db:
        pending_uploads = (
            await db.scalars(select(PendingUpload).where(PendingUpload.created_at < cutoff))
        ).all()
        deleted = 0
        for pending in pending_uploads:
            # Vérifie si l'objet S3 est référencé par au moins une version confirmée.
            # Cas possible : deux clients pushent le même contenu (même content_sha256),
            # l'un confirme → l'objet devient légitime → on supprime le slot pending
            # mais PAS l'objet S3 (il appartient maintenant à une version réelle, AD-10).
            is_referenced = await db.scalar(
                select(exists().where(SaveVersion.object_key == pending.object_key))
            )
            if not is_referenced:
                # L'objet S3 est orphelin : plus personne n'en a besoin, on le supprime.
                await delete_object(pending.object_key)
                deleted += 1
            # Dans tous les cas, le slot pending est supprimé — il a rempli son rôle
            # (ou expiré sans être confirmé).
            await db.delete(pending)
        await db.commit()
    logger.info("pending_upload_gc")
    if deleted:
        logger.info("orphan_objects_deleted")


async def purge_idempotency_keys() -> None:
    """Purge cached write responses older than their frozen 24-hour TTL."""

    # Les clés d'idempotence sont des caches de réponses (pour les retries clients).
    # Après 24 h, le client ne retente plus (ses timeouts sont bien inférieurs) et
    # la clé n'a plus d'utilité — on libère la place en base.
    cutoff = datetime.now(UTC) - timedelta(hours=get_settings().idempotency_ttl_h)
    async with get_session_factory()() as db:
        # DELETE en une seule requête SQL (pas de boucle) : idempotency_keys peut
        # avoir beaucoup d'entrées si les clients envoient des requêtes fréquentes.
        await db.execute(delete(IdempotencyKey).where(IdempotencyKey.created_at < cutoff))
        await db.commit()
    logger.info("idempotency_keys_purged")


def start_scheduler() -> AsyncIOScheduler:
    """Start the two and only two M1 maintenance jobs."""

    # APScheduler tourne dans la même boucle asyncio que FastAPI — pas de thread
    # séparé ni de process externe (Celery/RQ rejetés en AD-19).
    scheduler = AsyncIOScheduler(timezone="UTC")
    scheduler.add_job(
        gc_pending_uploads,
        "interval",
        hours=1,
        id="pending_upload_gc",
        # coalesce=True : si le job est en retard (serveur surchargé), on ne
        # rattrape pas les exécutions manquées — on lance une seule fois.
        coalesce=True,
        # max_instances=1 : si un tour prend plus d'une heure (beaucoup d'orphelins),
        # on n'en lance pas un deuxième en parallèle.
        max_instances=1,
    )
    scheduler.add_job(
        purge_idempotency_keys,
        "interval",
        hours=1,
        id="idempotency_purge",
        coalesce=True,
        max_instances=1,
    )
    scheduler.start()
    return scheduler
