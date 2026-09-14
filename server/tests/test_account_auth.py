"""Comptes hébergés : authentification par JWT (CLD-01).

Ce que ces tests fixent, avant toute ligne d'implémentation :

1. **Le service hébergé n'a plus d'URL ni de jeton à coller.** L'application
   présente un JWT obtenu par connexion e-mail/mot de passe ; le serveur le
   vérifie et retrouve — ou crée — le compte correspondant.
2. **L'auto-hébergement ne change pas d'un pouce.** Le jeton d'invitation reste
   accepté, sur les mêmes routes, avec le même format. Quelqu'un qui héberge
   RetroSave chez lui ne doit rien devoir à un fournisseur d'identité.
3. **Un serveur sans configuration de comptes refuse les JWT proprement.** Le
   défaut est l'auto-hébergement : tant que personne n'a posé de secret, la
   seule authentification est le jeton d'invitation.

La vérification porte sur la signature, l'expiration, l'émetteur et
l'audience. Un JWT non signé ou signé avec une autre clé n'ouvre rien : c'est
le seul point où ce fichier fait vraiment son travail.
"""

from __future__ import annotations

import time
import uuid

import jwt
import pytest
from app.config import get_settings
from app.db.models import User
from app.db.session import get_session_factory
from conftest import Identity
from cryptography.hazmat.primitives.asymmetric import ec
from httpx import AsyncClient
from sqlalchemy import select

# Au moins 32 octets : en dessous, PyJWT avertit à juste titre qu'une clé
# HMAC trop courte affaiblit SHA-256. Le secret de test dépasse 32 octets.
SECRET = "secret-de-test-fournisseur-au-moins-32-octets"
ISSUER = "https://identite.exemple.fr"
AUDIENCE = "authenticated"


@pytest.fixture
def cloud_accounts(monkeypatch: pytest.MonkeyPatch) -> None:
    """Configure this server as a hosted one, for the duration of one test."""

    monkeypatch.setenv("ACCOUNT_JWT_SECRET", SECRET)
    monkeypatch.setenv("ACCOUNT_ISSUER", ISSUER)
    monkeypatch.setenv("ACCOUNT_AUDIENCE", AUDIENCE)
    # Le `.env` du poste peut désigner un vrai fournisseur : sans cette purge,
    # la suite irait interroger le réseau et testerait autre chose que ce
    # qu'elle annonce.
    monkeypatch.delenv("ACCOUNT_JWKS_URL", raising=False)
    # Les réglages sont mis en cache pour tout le processus : sans purge, le
    # test lirait la configuration d'un test précédent.
    get_settings.cache_clear()
    yield
    get_settings.cache_clear()


def account_token(
    subject: str = "11111111-1111-1111-1111-111111111111",
    email: str = "joueur@exemple.fr",
    *,
    secret: str = SECRET,
    issuer: str = ISSUER,
    audience: str = AUDIENCE,
    expires_in: int = 3600,
    algorithm: str = "HS256",
) -> str:
    """Mint the kind of token the identity provider hands to the app."""

    now = int(time.time())
    return jwt.encode(
        {
            "sub": subject,
            "email": email,
            "iss": issuer,
            "aud": audience,
            "iat": now,
            "exp": now + expires_in,
        },
        secret,
        algorithm=algorithm,
    )


async def test_a_first_sign_in_creates_the_account(
    client: AsyncClient,
    cloud_accounts: None,
) -> None:
    """No invitation, no URL, no token to paste: signing in is enough."""

    response = await client.post(
        "/v0/devices",
        headers={"Authorization": f"Bearer {account_token()}"},
        json={"name": "Thor", "os": "android", "app_version": "0.1.0"},
    )

    assert response.status_code == 201, response.text
    async with get_session_factory()() as db:
        user = await db.scalar(
            select(User).where(User.auth_subject == "11111111-1111-1111-1111-111111111111")
        )
    assert user is not None
    # Le libellé sert à reconnaître le compte dans les journaux d'exploitation.
    assert user.label == "joueur@exemple.fr"
    # Un compte hébergé n'a pas de jeton d'invitation : la colonne doit
    # l'accepter, sinon l'inscription est impossible sans en fabriquer un faux.
    assert user.invite_token_hash is None


async def test_signing_in_twice_reuses_the_same_account(
    client: AsyncClient,
    cloud_accounts: None,
) -> None:
    """The subject identifies the account; a new session is not a new user."""

    headers = {"Authorization": f"Bearer {account_token()}"}
    first = await client.post(
        "/v0/devices",
        headers=headers,
        json={"name": "Thor", "os": "android", "app_version": "0.1.0"},
    )
    # Un second jeton, émis plus tard, pour le même sujet.
    second = await client.post(
        "/v0/devices",
        headers={"Authorization": f"Bearer {account_token(expires_in=7200)}"},
        json={"name": "PC", "os": "linux", "app_version": "0.1.0"},
    )

    assert first.status_code == second.status_code == 201
    async with get_session_factory()() as db:
        users = (
            await db.scalars(
                select(User).where(User.auth_subject == "11111111-1111-1111-1111-111111111111")
            )
        ).all()
    assert len(users) == 1

    # Et les deux appareils appartiennent bien au MÊME compte.
    listing = await client.get(
        "/v0/devices",
        headers={
            "Authorization": f"Bearer {account_token()}",
            "X-Device-Id": second.json()["device_id"],
        },
    )
    assert {device["name"] for device in listing.json()["devices"]} == {"Thor", "PC"}


async def test_two_subjects_are_two_isolated_accounts(
    client: AsyncClient,
    cloud_accounts: None,
) -> None:
    """Isolation is the whole point of a shared server (CLD-05)."""

    mine = await client.post(
        "/v0/devices",
        headers={"Authorization": f"Bearer {account_token()}"},
        json={"name": "Mon Thor", "os": "android", "app_version": "0.1.0"},
    )
    stranger_token = account_token(
        subject="22222222-2222-2222-2222-222222222222",
        email="autre@exemple.fr",
    )
    await client.post(
        "/v0/devices",
        headers={"Authorization": f"Bearer {stranger_token}"},
        json={"name": "Son PC", "os": "linux", "app_version": "0.1.0"},
    )

    # L'inconnu emploie MON identifiant d'appareil : refusé, comme pour un
    # jeton d'invitation d'un autre compte.
    refused = await client.get(
        "/v0/units",
        headers={
            "Authorization": f"Bearer {stranger_token}",
            "X-Device-Id": mine.json()["device_id"],
        },
    )
    assert refused.status_code == 403
    assert refused.json()["error"]["code"] == "not_owner"


@pytest.mark.parametrize(
    ("description", "token_kwargs"),
    [
        ("signé avec une autre clé", {"secret": "une-autre-cle-tout-aussi-longue-et-differente"}),
        ("émis par quelqu'un d'autre", {"issuer": "https://pirate.exemple/auth/v1"}),
        ("destiné à un autre service", {"audience": "un-autre-service"}),
        ("expiré", {"expires_in": -60}),
    ],
)
async def test_a_token_that_does_not_hold_up_opens_nothing(
    client: AsyncClient,
    cloud_accounts: None,
    description: str,
    token_kwargs: dict,
) -> None:
    """Signature, issuer, audience and expiry are all checked."""

    response = await client.post(
        "/v0/devices",
        headers={"Authorization": f"Bearer {account_token(**token_kwargs)}"},
        json={"name": "Intrus", "os": "linux", "app_version": "0.1.0"},
    )

    assert response.status_code == 401, f"{description} : {response.text}"
    assert response.json()["error"]["code"] == "invalid_token"
    # Et surtout : aucun compte n'a été créé au passage.
    async with get_session_factory()() as db:
        assert await db.scalar(select(User).where(User.auth_subject.is_not(None))) is None


async def test_an_unsigned_token_is_refused(
    client: AsyncClient,
    cloud_accounts: None,
) -> None:
    """`alg: none` is the oldest trick against JWT verification."""

    forged = jwt.encode(
        {
            "sub": "33333333-3333-3333-3333-333333333333",
            "iss": ISSUER,
            "aud": AUDIENCE,
            "exp": int(time.time()) + 3600,
        },
        key="",
        algorithm="none",
    )

    response = await client.post(
        "/v0/devices",
        headers={"Authorization": f"Bearer {forged}"},
        json={"name": "Intrus", "os": "linux", "app_version": "0.1.0"},
    )

    assert response.status_code == 401


async def test_a_token_without_subject_is_refused(
    client: AsyncClient,
    cloud_accounts: None,
) -> None:
    """Without a subject there is no account to attach anything to."""

    now = int(time.time())
    anonymous = jwt.encode(
        {"iss": ISSUER, "aud": AUDIENCE, "iat": now, "exp": now + 3600},
        SECRET,
        algorithm="HS256",
    )

    response = await client.post(
        "/v0/devices",
        headers={"Authorization": f"Bearer {anonymous}"},
        json={"name": "Sans nom", "os": "linux", "app_version": "0.1.0"},
    )

    assert response.status_code == 401


async def test_invitation_tokens_still_work_on_a_hosted_server(
    client: AsyncClient,
    identity: Identity,
    cloud_accounts: None,
) -> None:
    """Self-hosting must never become collateral damage of the paid offer."""

    response = await client.get("/v0/units", headers=identity.headers)

    assert response.status_code == 200


async def test_a_self_hosted_server_refuses_account_tokens(
    client: AsyncClient,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """No account secret configured means one authentication, not two."""

    # Pas de fixture `cloud_accounts` ici : c'est un serveur personnel ordinaire.
    # Les deux réglages sont purgés, `.env` compris — sans quoi ce test
    # dépendrait de la configuration du poste qui l'exécute.
    monkeypatch.delenv("ACCOUNT_JWT_SECRET", raising=False)
    monkeypatch.delenv("ACCOUNT_JWKS_URL", raising=False)
    get_settings.cache_clear()
    response = await client.post(
        "/v0/devices",
        headers={"Authorization": f"Bearer {account_token()}"},
        json={"name": "Thor", "os": "android", "app_version": "0.1.0"},
    )

    assert response.status_code == 401
    assert response.json()["error"]["code"] == "invalid_token"
    get_settings.cache_clear()


async def test_a_malformed_credential_is_refused_before_any_query(
    client: AsyncClient,
    cloud_accounts: None,
) -> None:
    """Neither an invitation token nor a JWT: refused on sight."""

    for credential in ("", "pas-un-jeton", f"rsc_{uuid.uuid4().hex}", "a.b"):
        response = await client.get(
            "/v0/units",
            headers={
                "Authorization": f"Bearer {credential}",
                "X-Device-Id": "00000000-0000-0000-0000-000000000000",
            },
        )
        assert response.status_code == 401, credential


# ─── Signature asymétrique (ES256 + JWKS) ─────────────────────────────────
# Les jetons sont signés par une clé privée que le fournisseur garde, puis le
# serveur vérifie
# avec la clé PUBLIQUE publiée sur son JWKS. Conséquence directe, et c'est tout
# l'intérêt : **le serveur n'a plus aucun secret à détenir**. Une fuite de sa
# configuration ne permet de fabriquer aucun jeton.

SIGNING_KEY = ec.generate_private_key(ec.SECP256R1())
OTHER_KEY = ec.generate_private_key(ec.SECP256R1())


def asymmetric_token(
    key: ec.EllipticCurvePrivateKey = SIGNING_KEY,
    *,
    subject: str = "44444444-4444-4444-4444-444444444444",
    email: str = "joueuse@exemple.fr",
    kid: str = "cle-de-test",
    expires_in: int = 3600,
) -> str:
    """Mint a token issued by a modern asymmetric identity provider."""

    now = int(time.time())
    return jwt.encode(
        {
            "sub": subject,
            "email": email,
            "iss": ISSUER,
            "aud": AUDIENCE,
            "iat": now,
            "exp": now + expires_in,
        },
        key,
        algorithm="ES256",
        headers={"kid": kid},
    )


@pytest.fixture
def asymmetric_accounts(monkeypatch: pytest.MonkeyPatch) -> None:
    """Configure the server to trust one published public key."""

    monkeypatch.setenv("ACCOUNT_ISSUER", ISSUER)
    monkeypatch.setenv("ACCOUNT_AUDIENCE", AUDIENCE)
    monkeypatch.setenv("ACCOUNT_JWKS_URL", "https://identite.exemple.fr/.well-known/jwks.json")
    monkeypatch.delenv("ACCOUNT_JWT_SECRET", raising=False)
    get_settings.cache_clear()

    # Le réseau n'a rien à faire dans une suite de tests : on remplace la seule
    # fonction qui sort, et tout le reste — sélection par `kid`, vérification,
    # ouverture du compte — s'exécute pour de vrai.
    from app.api import deps

    async def published_key(url: str, kid: str | None):
        assert url.endswith("/jwks.json")
        return SIGNING_KEY.public_key() if kid == "cle-de-test" else None

    monkeypatch.setattr(deps, "fetch_published_key", published_key)
    yield
    get_settings.cache_clear()


async def test_a_token_signed_by_the_published_key_opens_the_account(
    client: AsyncClient,
    asymmetric_accounts: None,
) -> None:
    """No shared secret anywhere: the public key is enough to verify."""

    response = await client.post(
        "/v0/devices",
        headers={"Authorization": f"Bearer {asymmetric_token()}"},
        json={"name": "Thor", "os": "android", "app_version": "0.1.0"},
    )

    assert response.status_code == 201, response.text
    async with get_session_factory()() as db:
        user = await db.scalar(
            select(User).where(User.auth_subject == "44444444-4444-4444-4444-444444444444")
        )
    assert user is not None
    assert user.label == "joueuse@exemple.fr"


async def test_a_token_signed_by_another_key_is_refused(
    client: AsyncClient,
    asymmetric_accounts: None,
) -> None:
    """A perfectly formed token from somebody else's project opens nothing."""

    response = await client.post(
        "/v0/devices",
        headers={"Authorization": f"Bearer {asymmetric_token(OTHER_KEY)}"},
        json={"name": "Intrus", "os": "linux", "app_version": "0.1.0"},
    )

    assert response.status_code == 401
    async with get_session_factory()() as db:
        assert await db.scalar(select(User).where(User.auth_subject.is_not(None))) is None


async def test_a_token_naming_an_unknown_key_is_refused(
    client: AsyncClient,
    asymmetric_accounts: None,
) -> None:
    """An unknown `kid` must not fall back to some other key."""

    response = await client.post(
        "/v0/devices",
        headers={"Authorization": f"Bearer {asymmetric_token(kid='cle-inventee')}"},
        json={"name": "Intrus", "os": "linux", "app_version": "0.1.0"},
    )

    assert response.status_code == 401


async def test_an_expired_asymmetric_token_is_refused(
    client: AsyncClient,
    asymmetric_accounts: None,
) -> None:
    """A valid signature is not a valid session."""

    response = await client.post(
        "/v0/devices",
        headers={"Authorization": f"Bearer {asymmetric_token(expires_in=-60)}"},
        json={"name": "Intrus", "os": "linux", "app_version": "0.1.0"},
    )

    assert response.status_code == 401
