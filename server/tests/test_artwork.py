"""Résolution d'une jaquette (Q45, volet 3).

Ce que ce module doit garantir, et qui compte plus que le taux de réussite :

1. **Il ne fait jamais échouer quoi que ce soit.** Une jaquette est décorative.
   Un index injoignable, un titre introuvable ou une réponse absurde rendent
   « pas d'image », jamais une erreur — surtout pas pendant la création d'une
   unité, qui est une opération de synchronisation.
2. **Il ne bloque jamais une requête.** Le catalogue est chargé en tâche de
   fond ; tant qu'il n'est pas là, la réponse est « pas d'image ».
3. **Il ne redistribue rien.** Le serveur rend une ADRESSE ; c'est le client
   qui va chercher l'image. Nous ne stockons ni ne réémettons aucune jaquette.

Le taux de correspondance mesuré le 13/09/2026 : 100 % sur vingt jeux 3DS de
grande diffusion, 32 % sur le catalogue eShop complet — ce dernier étant
dominé par des titres dématérialisés qui n'ont jamais eu de jaquette.
"""

from __future__ import annotations

from app.core import artwork

# Un extrait réel de l'index, avec ses conventions : article déplacé, deux-points
# devenus tirets, accents absents, balises de région et de langues.
INDEX = [
    "Professor Layton and the Azran Legacy (Europe).png",
    "Professor Layton and the Azran Legacy (USA).png",
    "Legend of Zelda, The - Ocarina of Time 3D (Europe) (En,Fr,De,Es,It).png",
    "Pokemon X (Europe) (En,Ja,Fr,De,Es,It,Ko).png",
    "Nintendogs + Cats - Golden Retriever & New Friends (USA).png",
    "Mario Kart 7 (Europe) (En,Fr,De,Es,It).png",
]


def resolve(label: str, emulator: str = "azahar") -> str | None:
    """Résoudre sans réseau : l'index est fourni directement."""

    return artwork.match_in_index(emulator, label, artwork.build_index(INDEX))


def test_an_exact_title_is_found() -> None:
    assert resolve("Mario Kart 7") is not None
    assert "Mario%20Kart%207" in resolve("Mario Kart 7")


def test_a_trademark_sign_does_not_prevent_the_match() -> None:
    # Le piège : une normalisation Unicode de COMPATIBILITÉ transforme « ™ » en
    # « TM » et colle donc « tm » à la fin de chaque titre Nintendo.
    assert resolve("Professor Layton and the Azran Legacy™") is not None


def test_an_accent_does_not_prevent_the_match() -> None:
    # Le catalogue écrit « Pokemon », les titres officiels « Pokémon ».
    assert resolve("Pokémon X") is not None


def test_a_leading_article_is_moved_like_the_catalogue_does() -> None:
    # « The Legend of Zelda: Ocarina of Time 3D » est catalogué
    # « Legend of Zelda, The - Ocarina of Time 3D ».
    assert resolve("The Legend of Zelda™: Ocarina of Time 3D") is not None


def test_a_longer_catalogue_title_still_matches_by_prefix() -> None:
    # Le titre officiel est plus court que celui du catalogue.
    assert resolve("Nintendogs + Cats: Golden Retriever") is not None


def test_an_unknown_title_yields_no_artwork() -> None:
    assert resolve("Un Jeu Qui N'existe Pas") is None


def test_a_too_short_label_never_matches_by_prefix() -> None:
    # Sans cette garde, « Pok » ramènerait la première entrée venue et
    # collerait une jaquette au hasard sur une sauvegarde.
    assert resolve("Pok") is None
    assert resolve("Mario") is None


def test_an_emulator_without_a_known_system_yields_nothing() -> None:
    assert resolve("Mario Kart 7", emulator="folder") is None


def test_a_raw_identifier_is_never_looked_up() -> None:
    # Un libellé encore brut — le titleid — n'a aucune chance de correspondre,
    # et l'interroger ne ferait que du bruit réseau.
    assert artwork.artwork_url("azahar", "000f3000") is None


def test_a_missing_index_yields_no_artwork_and_never_raises() -> None:
    # Le catalogue n'est pas encore chargé : la réponse est « pas d'image »,
    # jamais une erreur, jamais une attente.
    artwork.reset_cache()
    assert artwork.artwork_url("azahar", "Mario Kart 7", fetch=False) is None


def test_the_index_is_parsed_from_the_real_listing_shape() -> None:
    html = (
        "<html><body>"
        '<a href="Mario%20Kart%207%20(Europe)%20(En,Fr,De,Es,It).png">Mario Kart 7</a>'
        '<a href="../">parent</a>'
        '<a href="Autre%20Jeu.png">Autre</a>'
        "</body></html>"
    )
    noms = artwork.parse_listing(html)
    assert "Mario Kart 7 (Europe) (En,Fr,De,Es,It).png" in noms
    assert "Autre Jeu.png" in noms
    assert len(noms) == 2
