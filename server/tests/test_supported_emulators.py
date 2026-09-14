"""Tout adaptateur livré doit pouvoir déclarer une unité — constaté le 13/09/2026.

Le défaut était silencieux et total : mGBA, Snes9x, Dolphin, DuckStation et
PCSX2 ont été ajoutés côté clients, mais la liste des émulateurs acceptés n'a
bougé ni dans le schéma d'entrée, ni dans la contrainte `CHECK` de la table.
Résultat sur une vraie partie : la sauvegarde Golden Sun était découverte,
affichée dans la bibliothèque, et refusée à la déclaration. Aucune version
n'était jamais créée — une sauvegarde qu'on croit protégée et qui ne l'est pas
est pire qu'une sauvegarde qu'on sait non couverte.

La liste vit dans `app.core.emulators` : un seul endroit à modifier pour le
prochain adaptateur, et ce test échoue tant que le serveur n'en accepte pas un.
"""

from __future__ import annotations

import pytest
from app.core.emulators import SUPPORTED_EMULATORS
from conftest import Identity
from httpx import AsyncClient


@pytest.mark.parametrize("emulator", sorted(SUPPORTED_EMULATORS))
async def test_every_supported_emulator_can_declare_a_unit(
    client: AsyncClient,
    identity: Identity,
    emulator: str,
) -> None:
    response = await client.post(
        "/v0/units",
        headers=identity.headers,
        json={
            "emulator": emulator,
            "unit_key": "Golden Sun (FR).sav",
            "unit_type": "file",
            "game_key": "golden sun",
            "game_label": "Golden Sun",
        },
    )

    # 201 : l'unité est nouvelle. Une redéclaration rendrait 200.
    assert response.status_code == 201, response.text
    assert response.json()["unit"]["emulator"] == emulator


async def test_an_unknown_emulator_is_still_refused(
    client: AsyncClient,
    identity: Identity,
) -> None:
    """Élargir la liste n'est pas l'ouvrir : un identifiant inconnu reste 422."""

    response = await client.post(
        "/v0/units",
        headers=identity.headers,
        json={
            "emulator": "emulateur-invente",
            "unit_key": "x.sav",
            "unit_type": "file",
            "game_key": "x",
            "game_label": "X",
        },
    )

    assert response.status_code == 422


def test_the_shipped_adapters_and_the_server_agree() -> None:
    """La liste du serveur couvre les manifestes réellement livrés.

    Sans ce test, le prochain adaptateur ajouté dans `adapters/` repartirait
    exactement dans le même mur, et rien ne le dirait avant une vraie partie.
    """

    from pathlib import Path

    manifests = Path(__file__).resolve().parents[2] / "adapters"
    shipped = {path.stem for path in manifests.glob("*.toml")}

    assert shipped, "aucun manifeste trouvé : le chemin des adaptateurs a bougé"
    assert shipped <= set(SUPPORTED_EMULATORS), shipped - set(SUPPORTED_EMULATORS)
