"""Unauthenticated service health endpoint."""

import asyncio

from fastapi import APIRouter

from app.db.session import check_database
from app.s3 import check_storage
from app.schemas import HealthResponse
from app.version import app_version

# Pas de préfixe /v0 : /healthz est hors versionnement, accessible sans token.
# Utilisé par le healthcheck Docker Compose et par les clients qui vérifient
# la joignabilité du serveur avant d'afficher l'écran de connexion.
router = APIRouter()


@router.get("/healthz", response_model=HealthResponse)
async def healthz() -> HealthResponse:
    """Report PostgreSQL and object-storage readiness."""

    # asyncio.gather : les deux checks (DB et S3) s'exécutent en parallèle
    # plutôt que séquentiellement — réduit le temps de réponse du healthcheck
    # de ~2× sur des connexions lentes.
    database_ok, storage_ok = await asyncio.gather(
        check_database(),
        check_storage(),
    )
    return HealthResponse(
        status="ok",
        db=database_ok,
        s3=storage_ok,
        # La version est informatif uniquement (Q28, AD-31). Les clients qui ne
        # connaissent pas ce champ l'ignorent (ignoreUnknownKeys côté Kotlin).
        version=app_version(),
    )
