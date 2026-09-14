"""Authentication, device, database, and idempotency dependencies."""

from __future__ import annotations

import asyncio
import json
import time
import urllib.request
import uuid
from datetime import UTC, datetime, timedelta
from typing import Annotated, Any

from fastapi import Depends, Header
from fastapi.encoders import jsonable_encoder
from fastapi.responses import JSONResponse
from sqlalchemy import select
from sqlalchemy.exc import SQLAlchemyError
from sqlalchemy.ext.asyncio import AsyncSession

from app.api.errors import ApiError
from app.config import get_settings
from app.db.models import Device, IdempotencyKey, User
from app.db.session import get_db
from app.security import (
    account_token_kid,
    hash_token,
    is_valid_token_format,
    looks_like_account_token,
    verify_account_token,
)

# Alias de dépendance FastAPI : au lieu d'écrire `db: Annotated[AsyncSession, Depends(get_db)]`
# dans chaque signature d'endpoint, on écrit `db: Db`. FastAPI injecte automatiquement
# une session de base de données par requête.
Db = Annotated[AsyncSession, Depends(get_db)]


# ─── Les clés publiques du fournisseur d'identité ─────────────────────────
# Mises en cache : le fournisseur fait tourner ses clés, mais pas à chaque
# requête. Un cache éternel finirait par refuser tout le monde après une
# rotation ; pas de cache du tout ferait un appel réseau par requête.
_jwks_cache: dict[str, tuple[float, dict[str, object]]] = {}


async def fetch_published_key(url: str, kid: str | None) -> object | None:
    """Return the published public key named by `kid`, or None.

    Remplacée dans les tests : le réseau n'a rien à faire dans une suite de
    tests, mais tout le reste — sélection par `kid`, vérification de signature,
    ouverture du compte — doit s'exécuter pour de vrai.
    """

    if kid is None:
        return None
    settings = get_settings()
    cached = _jwks_cache.get(url)
    now = time.monotonic()
    keys: dict[str, object] | None = None
    if cached is not None and now - cached[0] < settings.account_jwks_ttl_s:
        keys = cached[1]  # type: ignore[assignment]
        if kid in keys:
            return keys[kid]
        # Clé inconnue : elle vient peut-être d'une rotation. On relit une fois,
        # puis on s'arrête — sinon un `kid` inventé provoquerait un appel réseau
        # à chaque requête, ce qui se transforme vite en amplification.
        if now - cached[0] < 30:
            return None

    def read() -> dict[str, object]:
        import jwt

        # Bornes explicites : un JWKS est un petit document, et ce serveur ne
        # doit pas pouvoir être retenu par un fournisseur lent ou bavard.
        request = urllib.request.Request(url, headers={"Accept": "application/json"})
        with urllib.request.urlopen(request, timeout=5) as response:
            document = json.loads(response.read(262144).decode("utf-8"))
        published: dict[str, object] = {}
        for entry in document.get("keys", []):
            identifier = entry.get("kid")
            if not isinstance(identifier, str):
                continue
            try:
                published[identifier] = jwt.PyJWK(entry).key
            except Exception:  # noqa: BLE001, S112 — une clé illisible en laisse d'autres
                continue
        return published

    try:
        # `urlopen` est bloquant : l'exécuter dans le fil de la boucle
        # d'événements figerait tout le serveur le temps de l'appel.
        keys = await asyncio.to_thread(read)
    except Exception:  # noqa: BLE001 — fournisseur injoignable = jeton refusé
        return None
    _jwks_cache[url] = (now, keys)
    return keys.get(kid)


async def _account_user(db: Db, credential: str) -> User:
    """Resolve — or open — the hosted account behind one verified token.

    CLD-01. Le service hébergé n'a ni URL ni jeton à coller : l'application se
    connecte auprès du fournisseur d'identité et présente le JWT obtenu.

    Un serveur **auto-hébergé** n'a aucun secret de compte configuré : il
    refuse donc les JWT, et son unique authentification reste le jeton
    d'invitation. C'est le défaut, et c'est voulu — personne ne doit dépendre
    d'un fournisseur d'identité pour héberger RetroSave chez lui.
    """

    settings = get_settings()
    # Deux modes, et le bon d'abord : la clé PUBLIQUE publiée par le
    # fournisseur. Le secret partagé n'est qu'un repli pour un fournisseur qui
    # ne publie pas de JWKS.
    if settings.account_jwks_url:
        key = await fetch_published_key(
            settings.account_jwks_url,
            account_token_kid(credential),
        )
        algorithms = settings.account_jwks_algorithms
    elif settings.account_jwt_secret:
        key = settings.account_jwt_secret
        algorithms = settings.account_algorithms
    else:
        raise ApiError(401, "invalid_token", "Token invalide.")
    if key is None:
        raise ApiError(401, "invalid_token", "Token invalide.")
    claims = verify_account_token(
        credential,
        secret=key,
        issuer=settings.account_issuer,
        audience=settings.account_audience,
        algorithms=algorithms,
    )
    # Signature, expiration, émetteur, audience : un seul refus pour tous les
    # cas. Distinguer aiderait surtout celui qui essaie.
    if claims is None:
        raise ApiError(401, "invalid_token", "Token invalide.")

    user = await db.scalar(select(User).where(User.auth_subject == claims.subject))
    if user is not None:
        if user.disabled_at is not None:
            raise ApiError(401, "invalid_token", "Token invalide.")
        return user

    # Première connexion : le compte s'ouvre ici. Aucun jeton d'invitation ne
    # lui est fabriqué — ce serait un second secret utilisable, donc une
    # seconde surface d'attaque, pour rien.
    user = User(
        auth_subject=claims.subject,
        invite_token_hash=None,
        label=claims.email or claims.subject,
    )
    db.add(user)
    try:
        await db.commit()
    except SQLAlchemyError:
        # Deux appareils qui se connectent en même temps peuvent créer le
        # compte à la même seconde : l'unicité du sujet tranche, et le perdant
        # relit simplement la ligne du gagnant.
        await db.rollback()
        user = await db.scalar(select(User).where(User.auth_subject == claims.subject))
        if user is None:
            raise ApiError(401, "invalid_token", "Token invalide.") from None
    return user


async def get_current_user(
    db: Db,
    authorization: Annotated[str | None, Header()] = None,
) -> User:
    """Resolve a bearer credential: hosted account token, or invitation token."""

    # Découpe "Bearer rsc_abc123..." en schéma + token.
    scheme, _, token = (authorization or "").partition(" ")
    if scheme.lower() != "bearer":
        raise ApiError(401, "invalid_token", "Token invalide.")
    # Le tri se fait sur la FORME, avant toute vérification : un JWT porte deux
    # points, un jeton d'invitation aucun. Ce n'est pas une validation, c'est
    # le choix de la vérification à appliquer.
    if looks_like_account_token(token):
        return await _account_user(db, token)
    # Rejet rapide avant toute requête SQL : format invalide = 401 immédiat.
    # is_valid_token_format() vérifie la regex rsc_[0-9a-f]{40}.
    if not is_valid_token_format(token):
        raise ApiError(401, "invalid_token", "Token invalide.")
    # On compare uniquement les HASH — le token en clair n'est jamais stocké (AD-07).
    # hash_token() calcule SHA-256 du token ; la colonne invite_token_hash contient
    # ce même hash, jamais le plaintext.
    user = await db.scalar(select(User).where(User.invite_token_hash == hash_token(token)))
    # Message identique que le token soit inconnu ou révoqué : on ne révèle pas
    # si le compte existe pour éviter l'énumération de comptes.
    if user is None or user.disabled_at is not None:
        raise ApiError(401, "invalid_token", "Token invalide.")
    return user


# Alias de dépendance : `user: CurrentUser` dans un endpoint résout l'authentification
# complète et retourne l'objet User ou lève 401 automatiquement.
CurrentUser = Annotated[User, Depends(get_current_user)]


async def get_current_device(
    db: Db,
    user: CurrentUser,
    x_device_id: Annotated[str | None, Header()] = None,
) -> Device:
    """Validate device ownership and update last-seen best effort."""

    # L'UUID est envoyé par le client dans le header X-Device-Id à chaque requête,
    # après avoir été reçu au POST /v0/devices lors du premier lancement.
    try:
        device_id = uuid.UUID(x_device_id or "")
    except ValueError as error:
        raise ApiError(
            422,
            "validation_error",
            "X-Device-Id invalide.",
        ) from error
    device = await db.get(Device, device_id)
    if device is None:
        raise ApiError(404, "not_found", "Appareil introuvable.")
    # Vérifie que l'appareil appartient bien à l'utilisateur authentifié.
    # Évite qu'un utilisateur A utilise le device_id d'un utilisateur B.
    if device.user_id != user.id:
        raise ApiError(403, "not_owner", "Ressource appartenant à un autre compte.")
    # EXP-01 : un appareil révoqué est refusé ICI, donc sur TOUTES les routes
    # qui exigent un appareil — lecture comprise. Le refus est explicite et
    # distinct d'un 401 : le jeton du compte, lui, reste valable, et le client
    # doit pouvoir le dire à l'utilisateur plutôt que de l'envoyer ressaisir un
    # jeton qui n'est pas en cause.
    if device.revoked_at is not None:
        raise ApiError(403, "device_revoked", "Appareil révoqué.")
    try:
        # Mise à jour "best effort" : si le commit échoue (base surchargée,
        # deadlock), on rollback silencieusement et la requête continue normalement.
        # last_seen_at est purement informatif (AD-36) — une mise à jour ratée
        # n'est pas une raison de rejeter la requête.
        device.last_seen_at = datetime.now(UTC)
        await db.commit()
    except SQLAlchemyError:
        await db.rollback()
    return device


# Alias combinant auth utilisateur + validation appareil. Un endpoint qui déclare
# `_device: CurrentDevice` s'assure que le header X-Device-Id est valide et que
# last_seen_at est mis à jour, sans polluer sa signature avec les détails.
CurrentDevice = Annotated[Device, Depends(get_current_device)]

# Alias pour le header d'idempotence. Le nom 'Idempotency-Key' (avec tiret) est
# converti en snake_case par FastAPI ; alias= permet de garder la casse HTTP officielle.
IdempotencyHeader = Annotated[str | None, Header(alias="Idempotency-Key")]


async def replay_idempotent(
    db: AsyncSession,
    user: User,
    key: str | None,
) -> JSONResponse | None:
    """Return a non-expired prior response for the same user and key."""

    # Appelé EN PREMIER dans chaque endpoint d'écriture. Si un replay est trouvé,
    # l'endpoint retourne immédiatement sans exécuter la logique métier.
    if not key:
        return None
    # La recherche porte sur le COUPLE (compte, clé). Un utilisateur B qui
    # emploie la clé d'un utilisateur A ne trouve donc rien et exécute sa
    # propre écriture : l'isolation est préservée sans que personne ne soit
    # bloqué.
    #
    # Auparavant la clé était cherchée seule, et une clé déjà prise par un
    # autre compte valait `403 not_owner`. Comme les clients dérivent leur clé
    # de l'identité de l'opération — et qu'une `unit_key` est la même pour tous
    # ceux qui jouent au même jeu — le deuxième joueur d'un titre populaire se
    # retrouvait refusé pendant vingt-quatre heures.
    cached = await db.get(IdempotencyKey, {"user_id": user.id, "key": key})
    if cached is None:
        return None
    # Vérifie l'expiration côté applicatif en plus du GC horaire, car le GC
    # peut avoir du retard. La clé expirée est supprimée immédiatement ici.
    expires_at = cached.created_at + timedelta(hours=get_settings().idempotency_ttl_h)
    if expires_at <= datetime.now(UTC):
        await db.delete(cached)
        await db.commit()
        return None
    # Retourne exactement la même réponse que la première fois : même status_code,
    # même body JSON. Le client ne peut pas distinguer un replay d'une vraie réponse.
    return JSONResponse(cached.response, status_code=cached.status_code)


async def store_idempotent(
    db: AsyncSession,
    user: User,
    key: str | None,
    status_code: int,
    payload: dict[str, Any],
) -> None:
    """Persist a JSON-safe write response for 24-hour replay."""

    if not key:
        return
    # Note : dans confirm_version (core/sync.py), le stockage se fait DANS la
    # même transaction que la création de version. Ici (autres endpoints), on
    # commit séparément car la logique métier a déjà commité. Les deux chemins
    # garantissent que la réponse stockée correspond exactement à ce qui s'est passé.
    db.add(
        IdempotencyKey(
            key=key,
            user_id=user.id,
            status_code=status_code,
            # jsonable_encoder convertit les UUIDs, datetimes, etc. en types
            # JSON-sérialisables avant de stocker dans la colonne JSONB.
            response=jsonable_encoder(payload),
        )
    )
    await db.commit()
