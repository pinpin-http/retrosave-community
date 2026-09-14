"""Une clé d'idempotence appartient à un COMPTE, pas au serveur entier.

Défaut constaté le 12 septembre 2026, sur un serveur de démonstration à deux
comptes. Les clients dérivent leur `Idempotency-Key` de l'identité de
l'opération — pour déclarer une unité, c'est `sha256("unit|<émulateur>|<clé>")`,
stable par construction, et c'est exactement ce qui rend une reprise sûre.

Mais une `unit_key` n'est unique que dans un compte : `ULJM05800DATA00` est le
même dossier de sauvegarde chez tous ceux qui jouent à ce jeu. Deux comptes
différents produisaient donc la **même** clé, et le second recevait un
`403 not_owner` — pendant vingt-quatre heures, le temps que la clé expire.

Conséquence : sur un serveur partagé — c'est-à-dire l'offre hébergée de la V1 —
le deuxième joueur d'un jeu populaire ne pouvait plus déclarer sa sauvegarde.
Le premier n'y était pour rien, et le second n'avait aucun moyen de comprendre.
"""

from __future__ import annotations

from app.db.session import get_session_factory
from app.security import generate_invite_token
from conftest import Identity
from httpx import AsyncClient


async def _second_account(client: AsyncClient) -> Identity:
    """Create a second, unrelated account with its own registered device."""

    from app.db.models import Device, User

    token, token_hash = generate_invite_token()
    async with get_session_factory()() as db:
        user = User(invite_token_hash=token_hash, label="autre-joueur")
        db.add(user)
        await db.flush()
        device = Device(
            user_id=user.id,
            name="Console d'un inconnu",
            os="android",
            app_version="0.1.0",
        )
        db.add(device)
        await db.commit()
        return Identity(token=token, user=user, device=device)


async def test_two_accounts_may_share_one_idempotency_key(
    client: AsyncClient,
    identity: Identity,
) -> None:
    """The same derived key from two accounts must serve both."""

    stranger = await _second_account(client)
    # La clé que produit le client pour cette unité : identique des deux côtés,
    # puisqu'elle ne dépend que de l'émulateur et de la clé d'unité.
    shared_key = "sha256-de-unit-ppsspp-ULJM05800DATA00"
    body = {
        "emulator": "ppsspp",
        "unit_key": "ULJM05800DATA00",
        "unit_type": "dir",
        "game_key": "ULJM05800",
        "game_label": "ULJM05800DATA00",
    }

    mine = await client.post(
        "/v0/units",
        headers={**identity.headers, "Idempotency-Key": shared_key},
        json=body,
    )
    theirs = await client.post(
        "/v0/units",
        headers={**stranger.headers, "Idempotency-Key": shared_key},
        json=body,
    )

    assert mine.status_code == 201, mine.text
    # C'est ici que le défaut se voyait : 403 not_owner, et le second joueur
    # restait bloqué jusqu'à l'expiration de la clé.
    assert theirs.status_code == 201, theirs.text
    # Deux unités distinctes, une par compte : aucune donnée partagée.
    assert mine.json()["unit"]["id"] != theirs.json()["unit"]["id"]


async def test_replaying_ones_own_key_still_returns_the_same_response(
    client: AsyncClient,
    identity: Identity,
) -> None:
    """Scoping must not weaken idempotency inside one account."""

    body = {
        "emulator": "ppsspp",
        "unit_key": "ULUS10391GAMEDATA",
        "unit_type": "dir",
        "game_key": "ULUS10391",
        "game_label": "ULUS10391GAMEDATA",
    }
    headers = {**identity.headers, "Idempotency-Key": "une-cle-stable"}

    first = await client.post("/v0/units", headers=headers, json=body)
    second = await client.post("/v0/units", headers=headers, json=body)

    assert first.status_code == second.status_code == 201
    assert first.json() == second.json()


async def test_one_account_never_reads_anothers_cached_response(
    client: AsyncClient,
    identity: Identity,
) -> None:
    """The reason the 403 existed must still hold: no cross-account replay."""

    stranger = await _second_account(client)
    shared_key = "une-cle-que-les-deux-emploient"

    mine = await client.post(
        "/v0/units",
        headers={**identity.headers, "Idempotency-Key": shared_key},
        json={
            "emulator": "ppsspp",
            "unit_key": "UCES00420SAVEDATA",
            "unit_type": "dir",
            "game_key": "UCES00420",
            "game_label": "Daxter",
        },
    )
    # Le second compte envoie la même clé pour une unité DIFFÉRENTE : il doit
    # obtenir sa propre réponse, jamais celle du premier.
    theirs = await client.post(
        "/v0/units",
        headers={**stranger.headers, "Idempotency-Key": shared_key},
        json={
            "emulator": "melonds",
            "unit_key": "Pokemon Platinum.sav",
            "unit_type": "file",
            "game_key": "pokemon platinum",
            "game_label": "Pokemon Platinum",
        },
    )

    assert mine.status_code == theirs.status_code == 201
    assert theirs.json()["unit"]["unit_key"] == "Pokemon Platinum.sav"
    assert theirs.json()["unit"]["emulator"] == "melonds"
