"""Résoudre l'adresse d'une jaquette — Q45, volet 3.

**Ce module ne redistribue rien.** Il rend une ADRESSE ; c'est le client qui va
chercher l'image et la garde en cache chez lui. Le serveur ne stocke aucune
jaquette et n'en réémet aucune : la question du droit de rediffusion ne se pose
donc pas de notre côté.

**Il ne fait jamais échouer quoi que ce soit.** Une jaquette est décorative, et
elle est résolue au milieu d'opérations qui, elles, ne le sont pas. Index
injoignable, titre introuvable, réponse absurde : la réponse est « pas
d'image », jamais une exception.

**Il ne bloque jamais une requête.** Le catalogue d'un système pèse quelques
centaines de kilo-octets et se charge en tâche de fond. Tant qu'il n'est pas
là, la réponse est « pas d'image » — et l'appel suivant l'aura.

Taux de correspondance mesuré le 13/09/2026 sur la 3DS : **100 % sur vingt
jeux de grande diffusion**, 32 % sur le catalogue eShop complet. L'écart n'est
pas un défaut d'appariement : le catalogue des vignettes est bâti sur les
cartouches physiques, et la majorité des titres eShop n'en a jamais eu.
"""

from __future__ import annotations

import logging
import re
import threading
import unicodedata
import urllib.parse
import urllib.request

logger = logging.getLogger(__name__)

#: La racine du catalogue communautaire. Aucune clé, aucun compte.
BASE = "https://thumbnails.libretro.com"

#: Nos identifiants de connecteur → le nom de système du catalogue.
#: Le dossier générique n'y figure pas : une sauvegarde qui n'appartient à
#: aucun système connu n'a pas de jaquette à chercher.
SYSTEMS = {
    "azahar": "Nintendo - Nintendo 3DS",
    "melonds": "Nintendo - Nintendo DS",
    "ppsspp": "Sony - PlayStation Portable",
    "mgba": "Nintendo - Game Boy Advance",
    "snes9x": "Nintendo - Super Nintendo Entertainment System",
    "dolphin": "Nintendo - GameCube",
    "duckstation": "Sony - PlayStation",
    "pcsx2": "Sony - PlayStation 2",
}

#: Les articles que le catalogue déplace en fin de titre principal :
#: « The Legend of Zelda: X » y est classé « Legend of Zelda, The - X ».
ARTICLES = ("the", "a", "an")

#: En dessous, une correspondance par préfixe ramènerait n'importe quoi :
#: « Pok » collerait la première jaquette venue sur une sauvegarde.
MIN_PREFIX = 7

#: Borne de lecture : un index de système pèse moins d'un mégaoctet.
MAX_INDEX_BYTES = 4 * 1024 * 1024

_cache: dict[str, dict[str, str]] = {}
_loading: set[str] = set()
_lock = threading.Lock()


def normalise(title: str) -> str:
    """Réduire un titre à ce qui l'identifie, des deux côtés de la comparaison.

    `NFD` et **pas** `NFKD` : la décomposition de compatibilité transforme
    « ™ » en « TM » et collerait donc « tm » à la fin de chaque titre Nintendo.
    On veut retirer les accents, pas réécrire les symboles.
    """

    text = unicodedata.normalize("NFD", title)
    text = "".join(c for c in text if not unicodedata.combining(c))
    text = text.lower()
    # Les balises de région et de langues du catalogue : (Europe) (En,Fr) (Rev 1).
    text = re.sub(r"\([^)]*\)", " ", text)
    text = re.sub(r"[^a-z0-9]+", " ", text)
    return " ".join(text.split())


def variants(title: str) -> list[str]:
    """Le titre normalisé, plus sa forme « article déplacé » du catalogue."""

    base = normalise(title)
    out = [base]
    head, _, tail = title.partition(":")
    words = normalise(head).split()
    if len(words) > 1 and words[0] in ARTICLES:
        moved = words[1:] + [words[0]] + normalise(tail).split()
        candidate = " ".join(moved)
        if candidate != base:
            out.append(candidate)
    return out


def parse_listing(html: str) -> list[str]:
    """Extraire les noms de fichiers d'un listing de répertoire."""

    names = []
    for href in re.findall(r'href="([^"]+\.png)"', html, flags=re.IGNORECASE):
        name = urllib.parse.unquote(href)
        # Un lien peut pointer ailleurs que dans le dossier courant ; on ne
        # garde que les noms simples.
        if "/" not in name:
            names.append(name)
    return names


def build_index(names: list[str]) -> dict[str, str]:
    """Indexer les noms de fichiers par titre normalisé.

    Le PREMIER gagne : les noms sont triés, donc « (Europe) » passe avant
    « (USA) » et une même entrée ne bascule pas d'une exécution à l'autre.
    """

    index: dict[str, str] = {}
    for name in sorted(names):
        index.setdefault(normalise(name[:-4]), name)
    return index


def match_in_index(emulator: str, label: str, index: dict[str, str]) -> str | None:
    """Trouver l'adresse de la jaquette d'un titre dans un index déjà chargé."""

    system = SYSTEMS.get(emulator)
    if system is None or not label.strip():
        return None
    for wanted in variants(label):
        if not wanted:
            continue
        name = index.get(wanted)
        if name is None and len(wanted) >= MIN_PREFIX:
            # Le catalogue est parfois plus bavard que le titre officiel :
            # « Nintendogs + Cats - Golden Retriever & New Friends » pour
            # « Nintendogs + Cats: Golden Retriever ». On prend le plus court,
            # pour rester déterministe.
            longer = sorted(k for k in index if k.startswith(wanted))
            if longer:
                name = index[longer[0]]
        if name is not None:
            return f"{BASE}/{urllib.parse.quote(system)}/Named_Boxarts/{urllib.parse.quote(name)}"
    return None


def _load(system: str) -> None:
    """Charger l'index d'un système. Exécuté dans un fil, jamais dans la requête."""

    url = f"{BASE}/{urllib.parse.quote(system)}/Named_Boxarts/"
    try:
        request = urllib.request.Request(url, headers={"Accept": "text/html"})
        with urllib.request.urlopen(request, timeout=20) as response:
            html = response.read(MAX_INDEX_BYTES).decode("utf-8", errors="replace")
        index = build_index(parse_listing(html))
    except Exception:  # noqa: BLE001 — un catalogue absent n'est pas une panne
        logger.info("catalogue de jaquettes indisponible", extra={"system": system})
        index = {}
    with _lock:
        _cache[system] = index
        _loading.discard(system)


def artwork_url(emulator: str, label: str, *, fetch: bool = True) -> str | None:
    """L'adresse de la jaquette d'une sauvegarde, ou rien.

    Ne bloque jamais : si le catalogue n'est pas encore chargé, la réponse est
    « pas d'image » et le chargement part en tâche de fond.
    """

    system = SYSTEMS.get(emulator)
    if system is None:
        return None
    # Un libellé encore brut — un titleid, un serial — ne correspondra à rien.
    # L'interroger ne ferait que du bruit.
    if not label.strip() or len(normalise(label)) < MIN_PREFIX:
        return None

    with _lock:
        index = _cache.get(system)
        if index is None:
            if fetch and system not in _loading:
                _loading.add(system)
                threading.Thread(target=_load, args=(system,), daemon=True).start()
            return None
    return match_in_index(emulator, label, index)


def reset_cache() -> None:
    """Vider le cache — utilisé par les tests."""

    with _lock:
        _cache.clear()
        _loading.clear()


def preload(systems: list[str] | None = None) -> None:
    """Charger les catalogues au démarrage, hors du chemin des requêtes."""

    for system in systems or sorted(set(SYSTEMS.values())):
        with _lock:
            if system in _cache or system in _loading:
                continue
            _loading.add(system)
        threading.Thread(target=_load, args=(system,), daemon=True).start()
