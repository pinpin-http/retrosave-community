"""Génère `server/app/data/3ds_titles.json` — M8 §6.

**À lancer à la main, jamais au runtime.** Le serveur ne va pas sur Internet :
il lit un fichier versionné dans le dépôt. Un service tiers qui tombe, change de
schéma ou disparaît ne doit pas pouvoir empêcher une unité d'être créée.

Ce qu'on garde : `titleid_low` → liste de `{name, region}`. Rien d'autre — pas
d'URL, pas d'image, pas de code produit, pas d'éditeur. C'est le minimum pour
afficher un nom, et le minimum est exactement ce qu'on redistribue.

Usage :

    .venv/Scripts/python.exe scripts/data/fetch_3ds_titles.py
"""

from __future__ import annotations

import json
import pathlib
from datetime import UTC, datetime

import httpx

SOURCE = "https://raw.githubusercontent.com/hax0kartik/3dsdb/master/jsons/list_{code}.json"
SOURCE_HOME = "https://github.com/hax0kartik/3dsdb"

#: Codes du dépôt source → codes de région retenus. La règle de choix du §6
#: parle en USA/EUR, pas en US/GB : on traduit ici plutôt que dans le serveur.
REGIONS = {
    "US": "USA",
    "GB": "EUR",
    "JP": "JPN",
    "KR": "KOR",
    "TW": "TWN",
}

#: Seules les applications portent des sauvegardes. `0004000E` (mises à jour) et
#: `0004008C` (DLC) n'ont pas de dossier `title/00040000/<low>/data/`.
APPLICATION_PREFIX = "00040000"


def fetch(code: str) -> list[dict]:
    response = httpx.get(SOURCE.format(code=code), timeout=120.0, follow_redirects=True)
    response.raise_for_status()
    return response.json()


def main() -> None:
    titles: dict[str, list[dict[str, str]]] = {}
    counts: dict[str, int] = {}

    for code, region in REGIONS.items():
        kept = 0
        for entry in fetch(code):
            title_id = str(entry.get("TitleID") or "").strip().lower()
            name = str(entry.get("Name") or "").strip()
            if len(title_id) != 16 or not title_id.startswith(APPLICATION_PREFIX) or not name:
                continue
            low = title_id[8:]
            candidates = titles.setdefault(low, [])
            # Jamais de fusion d'entrées (§6) : deux régions qui nomment
            # différemment le même jeu restent deux candidats distincts, et
            # c'est le serveur qui tranche selon le compte.
            if not any(c["region"] == region and c["name"] == name for c in candidates):
                candidates.append({"name": name, "region": region})
                kept += 1
        counts[region] = kept

    document = {
        "_source": SOURCE_HOME,
        "_generated_at": datetime.now(UTC).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "_note": (
            "Généré par scripts/data/fetch_3ds_titles.py. Noms de jeux seulement : "
            "aucune image, aucune URL, aucun identifiant de boutique. Le serveur ne "
            "réinterroge jamais la source au runtime."
        ),
        "_counts": counts,
        "titles": {low: titles[low] for low in sorted(titles)},
    }

    target = pathlib.Path(__file__).parents[2] / "server" / "app" / "data" / "3ds_titles.json"
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(
        json.dumps(document, ensure_ascii=False, indent=1, sort_keys=False) + "\n",
        encoding="utf-8",
    )
    print(f"{target}: {len(document['titles'])} titres, {counts}")


if __name__ == "__main__":
    main()
