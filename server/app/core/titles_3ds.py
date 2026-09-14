"""Résolution des titres 3DS depuis la table vendorée — M8 §6.

Le serveur ne va **jamais** sur Internet : il lit un fichier versionné, généré à
la main par `scripts/data/fetch_3ds_titles.py`. Un service tiers qui tombe ou
change de schéma ne doit pas pouvoir empêcher la création d'une unité.

Et une table absente ou illisible n'est pas une erreur : c'est simplement une
résolution qui n'a pas lieu, et l'unité garde son `titleid_low` (§10).
"""

from __future__ import annotations

import json
import pathlib
from functools import lru_cache

DATA_PATH = pathlib.Path(__file__).resolve().parents[1] / "data" / "3ds_titles.json"

#: Ordre de repli du §6 quand rien n'indique la région du compte.
FALLBACK_REGIONS = ("USA", "EUR")


@lru_cache(maxsize=1)
def load_titles() -> dict[str, list[dict[str, str]]]:
    """Charger la table une fois. Toute défaillance rend une table vide."""

    try:
        document = json.loads(DATA_PATH.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    titles = document.get("titles")
    if not isinstance(titles, dict):
        return {}
    return titles


def candidates_for(titleid_low: str) -> list[dict[str, str]]:
    entries = load_titles().get(titleid_low.strip().lower())
    return entries if isinstance(entries, list) else []


def pick_name(
    candidates: list[dict[str, str]],
    preferred_region: str | None,
) -> str | None:
    """Choisir un nom parmi les candidats, **sans jamais fusionner** (§6).

    Un jeu multi-régions porte des noms différents — « Pokémon Moon » aux
    États-Unis, « ポケットモンスター ムーン » au Japon. Coller les deux
    produirait un libellé qui n'existe nulle part.
    """

    if not candidates:
        return None
    for region in (preferred_region, *FALLBACK_REGIONS):
        if region is None:
            continue
        for candidate in candidates:
            if candidate.get("region") == region and candidate.get("name"):
                return str(candidate["name"])
    first = candidates[0].get("name")
    return str(first) if first else None


def infer_account_region(labels_by_titleid: dict[str, str]) -> str | None:
    """Deviner la région du compte d'après les unités Azahar déjà nommées.

    On ne stocke pas la région : elle se relit des libellés existants. Un compte
    qui affiche déjà « The Legend of Zelda™: Ocarina of Time 3D » est européen,
    et sa prochaine unité doit l'être aussi — sinon la bibliothèque mélangerait
    les langues d'un jeu à l'autre.
    """

    tally: dict[str, int] = {}
    for titleid_low, label in labels_by_titleid.items():
        for candidate in candidates_for(titleid_low):
            if candidate.get("name") == label:
                region = str(candidate.get("region") or "")
                if region:
                    tally[region] = tally.get(region, 0) + 1
    if not tally:
        return None
    # À égalité, l'ordre de repli tranche : deux unités USA et deux EUR ne
    # doivent pas donner un résultat qui dépend de l'ordre d'insertion.
    best = max(tally.values())
    tied = [region for region, count in tally.items() if count == best]
    for region in FALLBACK_REGIONS:
        if region in tied:
            return region
    return min(tied)
