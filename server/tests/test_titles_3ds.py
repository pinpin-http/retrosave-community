"""M8 §6 : résolution des titres 3DS depuis la table vendorée."""

from __future__ import annotations

from app.core.titles_3ds import candidates_for, infer_account_region, pick_name
from conftest import Identity
from httpx import AsyncClient

ZELDA_OOT = "00033600"
POKEMON_MOON = "00175e00"
ABSENT = "000f3200"


def unit_body(unit_key: str, label: str | None = None) -> dict:
    return {
        "emulator": "azahar",
        "unit_key": unit_key,
        "unit_type": "dir",
        "game_key": f"3ds:{unit_key}",
        "game_label": label or unit_key,
    }


def test_the_vendored_table_is_present_and_shaped() -> None:
    """Si ce test tombe, le fichier n'a pas été embarqué — pas un détail.

    Un serveur sans sa table ne casse rien, mais il n'affiche plus aucun nom
    3DS, et personne ne s'en apercevrait avant de regarder la bibliothèque.
    """

    zelda = candidates_for(ZELDA_OOT)

    assert zelda, "table 3DS absente ou vide"
    assert all({"name", "region"} <= set(entry) for entry in zelda)


def test_a_multi_region_game_keeps_its_entries_separate() -> None:
    """§6 : jamais de fusion. Coller deux noms en produirait un inexistant."""

    names = {entry["name"] for entry in candidates_for(POKEMON_MOON)}

    assert len(names) > 1
    assert not any(" / " in name for name in names)


def test_the_account_region_wins_over_the_fallback_order() -> None:
    japanese_account = infer_account_region(
        {
            "0010ba00": "Inazuma Eleven GO Galaxy Big Bang(イナズマイレブンGO ギャラクシー ビッグバン)"
        }
    )
    chosen = pick_name(candidates_for(POKEMON_MOON), japanese_account)

    assert japanese_account == "JPN"
    assert chosen is not None
    assert "ムーン" in chosen


def test_usa_then_eur_when_the_account_says_nothing() -> None:
    assert infer_account_region({}) is None
    assert pick_name(candidates_for(POKEMON_MOON), None) == "Pokémon™ Moon"


def test_an_unknown_title_resolves_to_nothing() -> None:
    assert candidates_for(ABSENT) == []
    assert pick_name([], "USA") is None


async def test_a_new_azahar_unit_gets_its_real_name(
    client: AsyncClient,
    identity: Identity,
) -> None:
    created = await client.post(
        "/v0/units",
        headers=identity.headers,
        json=unit_body(ZELDA_OOT),
    )

    assert created.status_code == 201
    assert created.json()["unit"]["game_label"] == "The Legend of Zelda™: Ocarina of Time 3D"
    assert created.json()["unit"]["label_source"] == "auto"


async def test_a_label_the_client_already_resolved_is_not_overwritten(
    client: AsyncClient,
    identity: Identity,
) -> None:
    """La table n'a pas toujours raison ; un libellé non brut vient de quelque part."""

    created = await client.post(
        "/v0/units",
        headers=identity.headers,
        json=unit_body(ZELDA_OOT, "Zelda OoT — édition du joueur"),
    )

    assert created.json()["unit"]["game_label"] == "Zelda OoT — édition du joueur"


async def test_an_unknown_title_keeps_its_identifier(
    client: AsyncClient,
    identity: Identity,
) -> None:
    """§10 : le pire résultat acceptable est « on affiche l'identifiant »."""

    created = await client.post(
        "/v0/units",
        headers=identity.headers,
        json=unit_body(ABSENT),
    )

    assert created.status_code == 201
    assert created.json()["unit"]["game_label"] == ABSENT


async def test_the_account_region_carries_over_to_the_next_unit(
    client: AsyncClient,
    identity: Identity,
) -> None:
    """Sinon la bibliothèque mélangerait les langues d'un jeu à l'autre."""

    await client.post(
        "/v0/units",
        headers=identity.headers,
        json=unit_body("0010ba00"),
    )
    second = await client.post(
        "/v0/units",
        headers=identity.headers,
        json=unit_body(POKEMON_MOON),
    )

    assert "ムーン" in second.json()["unit"]["game_label"]


async def test_a_missing_table_is_not_an_error(
    client: AsyncClient,
    identity: Identity,
    monkeypatch,
) -> None:
    """§10 : une table absente ne fait jamais échouer une création d'unité.

    Le fichier peut manquer d'une image mal construite, ou être illisible. Le
    seul comportement acceptable est de retomber sur l'identifiant, exactement
    comme avant M8 — pas une unité en erreur.
    """

    import app.core.titles_3ds as module

    module.load_titles.cache_clear()
    monkeypatch.setattr(module, "DATA_PATH", module.DATA_PATH.with_name("absent.json"))
    try:
        created = await client.post(
            "/v0/units",
            headers=identity.headers,
            json=unit_body(ZELDA_OOT),
        )
    finally:
        module.load_titles.cache_clear()

    assert created.status_code == 201
    assert created.json()["unit"]["game_label"] == ZELDA_OOT


async def test_a_corrupt_table_is_not_an_error(
    client: AsyncClient,
    identity: Identity,
    monkeypatch,
    tmp_path,
) -> None:
    import app.core.titles_3ds as module

    broken = tmp_path / "3ds_titles.json"
    broken.write_text("{ ceci n'est pas du json", encoding="utf-8")
    module.load_titles.cache_clear()
    monkeypatch.setattr(module, "DATA_PATH", broken)
    try:
        created = await client.post(
            "/v0/units",
            headers=identity.headers,
            json=unit_body(ZELDA_OOT),
        )
    finally:
        module.load_titles.cache_clear()

    assert created.status_code == 201
    assert created.json()["unit"]["game_label"] == ZELDA_OOT
