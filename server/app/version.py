"""Q28 : une seule source de vérité pour le numéro de version.

Le fichier `VERSION` à la racine du dépôt est lu par le serveur, le CLI et le
build Android. Trois constantes recopiées à la main divergent toujours, et une
version qui ment est pire qu'une version absente — c'est précisément ce qu'on
demande à `doctor` de diagnostiquer.

Le suffixe est le sha court du commit quand il est disponible : au build en CI
par la variable d'environnement, sinon rien plutôt qu'un mensonge.
"""

from __future__ import annotations

import os
from functools import lru_cache
from pathlib import Path

_IMAGE_VERSION_FILE = Path("/app/VERSION")


def _find_version_file() -> Path | None:
    """Remonter jusqu'au fichier plutôt que de supposer une profondeur.

    Le serveur tourne monté sur `/code` en dev et copié ailleurs en image :
    une profondeur codée en dur marche à un endroit et ment à l'autre.
    """

    override = os.environ.get("RETROSAVE_VERSION_FILE", "").strip()
    if override:
        candidate = Path(override)
        return candidate if candidate.is_file() else None
    if _IMAGE_VERSION_FILE.is_file():
        return _IMAGE_VERSION_FILE
    for parent in Path(__file__).resolve().parents:
        candidate = parent / "VERSION"
        if candidate.is_file():
            return candidate
    return None


@lru_cache(maxsize=1)
def app_version() -> str:
    """Return `X.Y.Z` or `X.Y.Z+<sha>` when the build knows its commit."""

    path = _find_version_file()
    if path is None:
        return "0.0.0"
    try:
        base = path.read_text(encoding="utf-8").strip()
    except OSError:
        return "inconnue"
    sha = (os.environ.get("GIT_SHA") or os.environ.get("RETROSAVE_GIT_SHA") or "").strip()
    return f"{base}+{sha}" if sha else base


def version_source() -> str:
    """Dire *d'où* vient la version, pas seulement laquelle.

    L'écart dev/image de Q30 s'est vu en production faute d'être annoncé. Une
    ligne au démarrage le rend visible au premier boot.
    """

    path = _find_version_file()
    return str(path) if path is not None else "aucune source"
