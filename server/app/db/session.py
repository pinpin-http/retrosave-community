"""SQLAlchemy async engine, sessions, and health probe."""

from collections.abc import AsyncIterator
from functools import lru_cache

from sqlalchemy import text
from sqlalchemy.ext.asyncio import (
    AsyncEngine,
    AsyncSession,
    async_sessionmaker,
    create_async_engine,
)

from app.config import get_settings


@lru_cache
def get_engine() -> AsyncEngine:
    """Create the process-wide PostgreSQL engine."""

    return create_async_engine(
        get_settings().database_url,
        # pool_pre_ping=True : avant de donner une connexion du pool, SQLAlchemy
        # envoie un SELECT 1. Si la connexion a été fermée par PostgreSQL (timeout
        # d'inactivité, redémarrage), une nouvelle est créée automatiquement.
        # Sans ça, les premières requêtes après un long silence lèveraient des
        # erreurs "connection closed" aléatoires.
        pool_pre_ping=True,
    )


async def check_database() -> bool:
    """Verify that PostgreSQL accepts a trivial query."""

    # Utilisé par GET /healthz pour signaler si PostgreSQL est joignable.
    # On ne passe pas par le pool de sessions pour éviter de "consommer" une
    # connexion dédiée aux requêtes applicatives pendant un healthcheck.
    async with get_engine().connect() as connection:
        await connection.execute(text("SELECT 1"))
    return True


@lru_cache
def get_session_factory() -> async_sessionmaker[AsyncSession]:
    """Build the process-wide session factory."""

    return async_sessionmaker(
        get_engine(),
        # expire_on_commit=False : par défaut SQLAlchemy marque les objets comme
        # "expirés" après un commit, ce qui déclenche un nouveau SELECT au prochain
        # accès à un attribut. En contexte async, ce rechargement implicite peut
        # lever une erreur "no active transaction". On désactive ce comportement
        # pour garder les objets utilisables après commit.
        expire_on_commit=False,
    )


async def get_db() -> AsyncIterator[AsyncSession]:
    """Provide one request-scoped transaction-capable async session."""

    # Générateur utilisé comme dépendance FastAPI : `Db = Annotated[AsyncSession, Depends(get_db)]`.
    # Le `async with` ferme et nettoie la session à la fin de chaque requête,
    # même en cas d'exception — pas besoin de try/finally dans les endpoints.
    async with get_session_factory()() as session:
        yield session
