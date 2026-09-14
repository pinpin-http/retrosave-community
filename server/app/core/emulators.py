"""La liste des émulateurs que le service accepte — un seul endroit.

Elle était écrite quatre fois : deux schémas Pydantic, une contrainte `CHECK`
et une garde côté client C++. Le 13/09/2026 cinq adaptateurs livrés — mGBA,
Snes9x, Dolphin, DuckStation, PCSX2 — étaient absents de toutes ces copies :
leurs sauvegardes étaient découvertes et affichées, mais refusées à la
déclaration, donc jamais protégées.

Ajouter un adaptateur, c'est donc ajouter une ligne ICI, puis générer une
migration. `test_supported_emulators.py` vérifie que cette liste couvre les
manifestes réellement présents dans `adapters/`.

`folder` n'a pas de manifeste : c'est l'adaptateur générique, « n'importe quel
dossier désigné par l'utilisateur ».
"""

from __future__ import annotations

from typing import Final

SUPPORTED_EMULATORS: Final[tuple[str, ...]] = (
    "azahar",
    "dolphin",
    "duckstation",
    "folder",
    "melonds",
    "mgba",
    "pcsx2",
    "ppsspp",
    "retroarch",
    "snes9x",
)


def check_constraint() -> str:
    """L'expression SQL de la contrainte, pour le modèle et les migrations."""

    values = ",".join(f"'{name}'" for name in SUPPORTED_EMULATORS)
    return f"emulator IN ({values})"
