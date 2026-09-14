"""Règle d'amélioration d'un libellé automatique — M8 §7.

Le pire défaut possible pour cette fonctionnalité serait d'effacer le nom qu'un
utilisateur a saisi à la main. `label_source` existe pour ça, et cette règle-ci
répond à la question restante : *quand* un client a-t-il le droit d'améliorer un
libellé encore automatique ?

Pas « le dernier qui écrit gagne ». Un client incapable de lire le `PARAM.SFO`
envoie le serial ; s'il écrasait un titre déjà résolu, la bibliothèque
régresserait à chaque passe du client le plus ancien.
"""

from __future__ import annotations

AUTO = "auto"
USER = "user"


def is_raw_label(label: str, unit_key: str) -> bool:
    """Un libellé est « brut » quand il n'est qu'un morceau de l'identifiant.

    Une seule règle couvre tous les adaptateurs, sans que le serveur ait à
    connaître leurs conventions :

    - Azahar pose le `titleid_low`, qui **est** l'`unit_key` ;
    - PPSSPP pose le serial, qui en est le préfixe ;
    - melonDS et RetroArch posaient le nom de fichier sans extension, qui en
      est aussi un préfixe ;
    - l'adaptateur `folder` pose le nom du dossier, égal à l'`unit_key`.

    Un vrai titre — « LocoRoco » pour `UCET00357_GameData0`, « Mario Kart DS »
    pour `Mario_Kart_DS_(Europe).sav` — n'en est pas un préfixe.
    """

    return bool(label) and unit_key.startswith(label)


def improved_label(
    *,
    stored_label: str,
    stored_source: str,
    incoming_label: str,
    unit_key: str,
) -> str | None:
    """Rendre le libellé à écrire, ou `None` s'il ne faut rien changer.

    Deux refus, et un seul d'entre eux protège vraiment :

    1. `label_source == 'user'` → **jamais**. C'est la garantie du §7 ; sans
       elle, le renommage manuel serait effacé au prochain scan.
    2. Le stocké est déjà résolu et l'entrant est brut → c'est une régression,
       pas une amélioration. Un client antérieur à M8 envoie le serial ; le
       laisser écraser « LocoRoco » referait perdre le travail à chaque passe.

    Tout le reste passe, parce que §7 autorise l'amélioration d'un libellé
    `auto` à tout moment et que le serveur n'a aucun moyen honnête de départager
    deux lectures légitimes.

    **Corrigé le 03/08 après constat sur le Thor.** La règle exigeait aussi que
    l'entrant ne soit pas brut, et ce test-là était faux : « Super Mario 64 DS »
    est un préfixe de « Super Mario 64 DS (Europe) (En,Fr,De,Es,It).sav », donc
    « brut » au sens de [is_raw_label]. Le libellé nettoyé de melonDS et de
    RetroArch était refusé — exactement les deux adaptateurs que la règle
    prétendait couvrir, et rien en test ne l'avait montré parce que tous mes cas
    opposaient un serial à un vrai titre.
    """

    if stored_source != AUTO:
        return None
    if not incoming_label or incoming_label == stored_label:
        return None
    if not is_raw_label(stored_label, unit_key) and is_raw_label(incoming_label, unit_key):
        return None
    return incoming_label
