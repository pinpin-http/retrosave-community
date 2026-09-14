"""M8 §7 : le nom saisi par l'utilisateur ne doit jamais être effacé.

C'est le pire défaut possible pour cette fonctionnalité : renommer une unité à
la main, puis la voir reprendre son serial au prochain scan. Ces tests le
reproduisent depuis l'API, pas depuis la fonction pure.
"""

from __future__ import annotations

from conftest import Identity
from httpx import AsyncClient

BODY = {
    "emulator": "ppsspp",
    "unit_key": "UCET00357_GameData0",
    "unit_type": "dir",
    "game_key": "UCET00357",
    "game_label": "UCET00357",
}


async def declare(client: AsyncClient, identity: Identity, label: str) -> dict:
    response = await client.post(
        "/v0/units",
        headers=identity.headers,
        json={**BODY, "game_label": label},
    )
    return response.json()["unit"]


async def test_a_new_unit_starts_as_auto(
    client: AsyncClient,
    identity: Identity,
) -> None:
    unit = await declare(client, identity, "UCET00357")

    assert unit["label_source"] == "auto"


async def test_a_client_that_reads_the_sfo_upgrades_a_raw_label(
    client: AsyncClient,
    identity: Identity,
) -> None:
    """§7 : améliorer un libellé `auto` est autorisé à tout moment."""

    await declare(client, identity, "UCET00357")
    upgraded = await declare(client, identity, "LocoRoco")

    assert upgraded["game_label"] == "LocoRoco"
    assert upgraded["label_source"] == "auto"


async def test_an_older_client_cannot_push_the_serial_back(
    client: AsyncClient,
    identity: Identity,
) -> None:
    """Sans cette règle, la bibliothèque régresserait à chaque passe.

    Un client incapable de lire le `PARAM.SFO` envoie le serial ; « le dernier
    qui écrit gagne » suffirait à défaire le travail du client qui sait lire.
    """

    await declare(client, identity, "UCET00357")
    await declare(client, identity, "LocoRoco")
    regressed = await declare(client, identity, "UCET00357")

    assert regressed["game_label"] == "LocoRoco"


async def test_two_resolved_labels_let_the_latest_client_win(
    client: AsyncClient,
    identity: Identity,
) -> None:
    """Assoupli le 03/08 après constat sur le Thor.

    Le serveur n'a aucun moyen honnête de départager deux lectures légitimes, et
    la condition qui servait à le deviner refusait au passage le libellé nettoyé
    de melonDS — « Super Mario 64 DS » est un préfixe de son propre nom de
    fichier, donc « brut ». La garantie qui compte reste `user`.
    """

    await declare(client, identity, "UCET00357")
    await declare(client, identity, "LocoRoco")
    other = await declare(client, identity, "Loco Roco (Europe)")

    assert other["game_label"] == "Loco Roco (Europe)"


async def test_case_23_a_manual_rename_survives_every_later_scan(
    client: AsyncClient,
    identity: Identity,
) -> None:
    """Cas 23 du banc : le libellé utilisateur survit partout.

    Le renommage est suivi de tout ce qu'un scan peut envoyer : le serial d'un
    client ancien, puis un titre lu dans le SFO par un client récent. Aucun des
    deux ne doit passer.
    """

    created = await declare(client, identity, "UCET00357")
    renamed = await client.patch(
        f"/v0/units/{created['id']}",
        headers=identity.headers,
        json={"game_label": "LocoRoco — ma partie"},
    )

    after_old_client = await declare(client, identity, "UCET00357")
    after_new_client = await declare(client, identity, "LocoRoco")
    listed = await client.get("/v0/units", headers=identity.headers)

    assert renamed.status_code == 200
    assert renamed.json()["unit"]["label_source"] == "user"
    assert after_old_client["game_label"] == "LocoRoco — ma partie"
    assert after_new_client["game_label"] == "LocoRoco — ma partie"
    assert listed.json()["units"][0]["game_label"] == "LocoRoco — ma partie"
    assert listed.json()["units"][0]["label_source"] == "user"


async def test_a_user_can_always_rename_again(
    client: AsyncClient,
    identity: Identity,
) -> None:
    """La garde protège de l'automatique, jamais de l'utilisateur lui-même."""

    created = await declare(client, identity, "UCET00357")
    await client.patch(
        f"/v0/units/{created['id']}",
        headers=identity.headers,
        json={"game_label": "Premier nom"},
    )
    second = await client.patch(
        f"/v0/units/{created['id']}",
        headers=identity.headers,
        json={"game_label": "Second nom"},
    )

    assert second.json()["unit"]["game_label"] == "Second nom"
    assert second.json()["unit"]["label_source"] == "user"
