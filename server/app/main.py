"""FastAPI application factory."""

import logging
from collections.abc import AsyncIterator
from contextlib import asynccontextmanager

from fastapi import FastAPI

from app.api.conflicts import router as conflicts_router
from app.api.devices import router as devices_router
from app.api.errors import install_error_handlers
from app.api.health import router as health_router
from app.api.units import router as units_router
from app.api.versions import router as versions_router
from app.config import get_settings
from app.core import artwork
from app.jobs import start_scheduler
from app.logging import configure_logging
from app.version import app_version, version_source


@asynccontextmanager
async def lifespan(_app: FastAPI) -> AsyncIterator[None]:
    """Own the two hourly maintenance jobs for this API process."""

    # AD-32 : annoncer la version *et sa source* au démarrage. L'écart dev/image
    # de Q30 n'était invisible que faute d'être dit ; il se voit maintenant au
    # premier boot, pas en production.
    logging.getLogger("app").info(
        "version %s (source : %s)",
        app_version(),
        version_source(),
    )
    # Démarre le scheduler APScheduler qui tourne les jobs de maintenance toutes
    # les heures (GC des uploads orphelins + purge des clés d'idempotence).
    # On le démarre ici plutôt qu'au niveau module pour qu'il soit lié au cycle
    # de vie de l'application — shutdown propre garanti même en cas d'exception.
    scheduler = start_scheduler()
    # Q45 : charger les catalogues de jaquettes hors du chemin des requêtes.
    # En tâche de fond, sans attendre : un catalogue injoignable ne doit pas
    # retarder d'une seconde le démarrage de l'API, et son absence ne coûte
    # qu'une pastille de repli à la place d'une image.
    artwork.preload()
    try:
        yield  # L'application tourne ici ; tout ce qui est après yield est le teardown.
    finally:
        # wait=False : on ne bloque pas l'arrêt du serveur en attendant la fin
        # d'un job en cours ; les jobs sont idempotents et se ré-exécuteront.
        scheduler.shutdown(wait=False)


def create_app() -> FastAPI:
    """Create the RetroSave API without browser-only middleware."""

    # Configure le logging JSON avant tout le reste pour que les éventuelles
    # erreurs de démarrage (config manquante, DB injoignable) soient déjà
    # dans le bon format et passent par le filtre de redaction des tokens.
    configure_logging(get_settings().log_level)
    application = FastAPI(
        title="RetroSave Community",
        version="0.1.0",
        # Swagger UI et ReDoc désactivés : aucun client navigateur dans le POC
        # (AD-22), et ces pages exposeraient le schéma d'API publiquement.
        docs_url=None,
        redoc_url=None,
        openapi_url=None,
        lifespan=lifespan,
    )
    # Les handlers d'erreur sont installés avant les routers pour qu'ils
    # interceptent aussi les erreurs levées pendant l'enregistrement des routes.
    install_error_handlers(application)
    # Chaque router est préfixé dans son propre fichier (ex. /v0/units).
    # L'ordre d'enregistrement n'a pas d'importance ici — FastAPI résout les
    # conflits de routes par ordre de déclaration dans chaque router.
    application.include_router(health_router)  # GET /healthz (sans auth)
    application.include_router(devices_router)  # POST/GET /v0/devices
    application.include_router(units_router)  # CRUD /v0/units
    application.include_router(versions_router)  # prepare / confirm / download / restore
    application.include_router(conflicts_router)  # liste + résolution des conflits
    return application


# Instance globale utilisée par Uvicorn pour démarrer le serveur.
# `create_app()` est appelée une seule fois au chargement du module.
app = create_app()
