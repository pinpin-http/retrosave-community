"""Les NEUF connecteurs livrés, éprouvés de bout en bout sur un vrai serveur.

Pourquoi ce banc existe, et pourquoi il ne ressemble pas aux tests d'adaptateur
déjà présents : ceux-ci vérifient la DÉCOUVERTE, en mémoire, sans réseau. Ils
étaient tous verts le 13/09/2026 pendant que cinq connecteurs sur neuf étaient
incapables de publier quoi que ce soit — la liste blanche des identifiants
d'émulateur, côté client comme côté serveur, était restée celle du premier jour
du POC. Une sauvegarde était découverte, nommée, affichée dans la
bibliothèque… et refusée à la déclaration.

C'est le pire défaut qu'un tel service puisse avoir : l'utilisateur croit sa
partie protégée alors qu'elle ne l'est pas. Aucun test de découverte ne pouvait
l'attraper. Celui-ci va donc jusqu'au bout de la chaîne, pour chaque connecteur :

    dossier réel → découverte → libellé → déclaration → archive → version

et il refuse d'être vert si un seul connecteur s'arrête en route.

Les arborescences ne sont pas inventées pour l'occasion : ce sont celles de
`adapters/fixtures/`, relevées sur de vraies installations puis anonymisées.
Le contenu des sauvegardes, lui, n'a aucune importance — RetroSave transporte
des octets, il ne les interprète jamais.

Usage :
    python scripts/testing/test_adapters_e2e.py \\
        --agent desktop/build/dev/bin/retrosave-agent
"""

from __future__ import annotations

import argparse
import configparser
import json
import shutil
import sys
import time
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))

from test_agent_e2e import Agent
from test_api_interop import disposable_services

FIXTURES = ROOT / "adapters" / "fixtures" / "linux"

#: Un connecteur, la racine à lui désigner, et ce qu'il DOIT en tirer.
#:
#: `label` est le libellé attendu dans la bibliothèque. Là où il vaut la clé
#: elle-même, c'est voulu : un `titleid` 3DS ou un serial PSP ne devient un
#: vrai titre que plus tard, quand le serveur ou le `PARAM.SFO` le résout.
CASES = [
    {
        "emulator": "ppsspp",
        "root": FIXTURES / "ppsspp" / "PSP" / "SAVEDATA",
        "keys": ["NPJH50043DATA", "ULUS10041SAVE"],
        "labels": ["NPJH50043DATA", "ULUS10041SAVE"],
    },
    {
        "emulator": "melonds",
        "root": FIXTURES / "melonds",
        "keys": ["Pokémon_Édition_Noire_(France).sav", "orphan.sav"],
        "labels": ["Pokémon Édition Noire", "orphan"],
    },
    {
        "emulator": "azahar",
        "root": FIXTURES / "azahar",
        "keys": ["00abcdef"],
        "labels": ["00abcdef"],
    },
    {
        "emulator": "mgba",
        "root": FIXTURES / "mgba" / "roms",
        "keys": ["Golden Sun.sav", "Pokemon Emerald.sav"],
        "labels": ["Golden Sun", "Pokemon Emerald"],
    },
    {
        "emulator": "snes9x",
        "root": FIXTURES / "snes9x" / "roms",
        "keys": ["Chrono Trigger (U).srm", "Super Metroid.srm"],
        "labels": ["Chrono Trigger", "Super Metroid"],
    },
    {
        "emulator": "dolphin",
        "root": FIXTURES / "dolphin",
        "keys": [
            "01-GALE-SuperSmashBros.gci",
            "01-GM4E-MetroidPrime.gci",
            "01-GZLP-ZeldaWindWaker.gci",
        ],
        "labels": [
            "01-GALE-SuperSmashBros",
            "01-GM4E-MetroidPrime",
            "01-GZLP-ZeldaWindWaker",
        ],
    },
    {
        "emulator": "duckstation",
        "root": FIXTURES / "duckstation",
        "keys": ["SLUS-00594_FinalFantasyVII.mcd", "shared_card_1.mcd"],
        # L'underscore devient une espace : c'est la règle d'affichage
        # partagée (`display_name_basic.json`), pas une particularité PS1.
        "labels": ["SLUS-00594 FinalFantasyVII", "shared card 1"],
    },
    {
        "emulator": "pcsx2",
        "root": FIXTURES / "pcsx2",
        "keys": ["Mcd001.ps2", "Mcd002.ps2"],
        "labels": ["Mcd001", "Mcd002"],
    },
    {
        "emulator": "retroarch",
        "root": FIXTURES / "retroarch" / "saves",
        "keys": ["Pokemon Emerald (U).srm", "Sonic The Hedgehog 2.SRM"],
        "labels": ["Pokemon Emerald", "Sonic The Hedgehog 2"],
    },
]


def register(url: str, token: str, name: str) -> str:
    """Enregistrer un appareil, comme le fait un client au premier lancement."""

    import urllib.request

    request = urllib.request.Request(
        f"{url}/v0/devices",
        data=json.dumps({"name": name, "os": "linux", "app_version": "0.1.0"}).encode(),
        headers={
            "Authorization": f"Bearer {token}",
            "Content-Type": "application/json",
            "Idempotency-Key": uuid.uuid4().hex,
        },
        method="POST",
    )
    with urllib.request.urlopen(request, timeout=10) as response:
        return json.load(response)["device_id"]


def designate_roots(agent: Agent, roots: dict[str, Path]) -> None:
    """Écrire toutes les racines d'un coup, comme le ferait l'interface."""

    agent.settings.parent.mkdir(parents=True, exist_ok=True)
    parser = configparser.ConfigParser()
    parser.optionxform = str
    if agent.settings.exists():
        parser.read(agent.settings)
    parser.setdefault("roots", {})
    for emulator, path in roots.items():
        parser["roots"][emulator] = str(path)
    with agent.settings.open("w") as handle:
        parser.write(handle, space_around_delimiters=False)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--agent", type=Path, required=True)
    arguments = parser.parse_args()
    if not arguments.agent.is_file():
        raise SystemExit(f"agent introuvable : {arguments.agent}")

    with disposable_services() as (url, token, _storage, directory):
        workspace = Path(directory)
        # Les fixtures sont COPIÉES : le banc écrit des `.rsc-bak` et des
        # marqueurs, et le dépôt ne doit pas en garder la trace.
        roots = {}
        for case in CASES:
            destination = workspace / "racines" / case["emulator"]
            shutil.copytree(case["root"], destination)
            roots[case["emulator"]] = destination

        agent = Agent(arguments.agent, workspace)
        try:
            # Les racines AVANT le rattachement : la configuration n'est relue
            # qu'à la fin d'une passe ou au rattachement, et une passe sans
            # aucune racine connue est refusée — à juste titre.
            designate_roots(agent, roots)
            agent.call({"method": "connect", "url": url, "token": token, "device_name": "Banc"})
            deadline = time.monotonic() + 30
            while not agent.call({"method": "status"})["connected"]:
                if time.monotonic() > deadline:
                    raise RuntimeError("l'appareil ne s'est jamais enregistré")
                time.sleep(0.2)

            # Trois passes : la première fait découvrir, la deuxième relit la
            # configuration et observe, la troisième publie — la stabilisation
            # exige deux observations espacées de dix secondes.
            for index in range(3):
                assert agent.call({"method": "sync"})["accepted"] is True
                agent.wait_idle(timeout=300)
                if index < 2:
                    time.sleep(11)
            summary = agent.wait_idle(timeout=300)["last"]
            assert summary["errors"] == 0, summary

            agent.call({"method": "units"})
            time.sleep(2)
            seen = agent.call({"method": "status"})["units"]
            by_emulator: dict[str, list[dict]] = {}
            for unit in seen:
                by_emulator.setdefault(unit["emulator"], []).append(unit)

            failures = []
            for case in CASES:
                emulator = case["emulator"]
                units = sorted(by_emulator.get(emulator, []), key=lambda u: u["key"])
                keys = [unit["key"] for unit in units]
                labels = [unit["label"] for unit in units]
                versions = [unit["version"] for unit in units]
                if keys != sorted(case["keys"]):
                    failures.append(f"{emulator} : découvert {keys}, attendu {case['keys']}")
                    continue
                if labels != [case["labels"][case["keys"].index(k)] for k in keys]:
                    failures.append(f"{emulator} : libellés {labels}, attendus {case['labels']}")
                    continue
                # Le cœur du banc : une version SERVEUR existe. Sans elle, la
                # sauvegarde n'est pas protégée, quoi qu'affiche la liste.
                if any(version < 1 for version in versions):
                    failures.append(f"{emulator} : publié {versions}, aucune version attendue à 0")
                    continue
                print(f"  ✓ {emulator:12} {len(units)} unité(s) publiée(s) : {', '.join(labels)}")

            # La jaquette est RAPPORTÉE, jamais exigée : elle dépend d'un
            # catalogue communautaire joignable, et rater une image ne met
            # aucune sauvegarde en danger.
            with_art = [unit for unit in seen if unit["icon"]]
            print(
                f"\nJaquettes trouvées : {len(with_art)}/{len(seen)} "
                "(indicatif — une image manquante ne compromet aucune sauvegarde)."
            )

            if failures:
                for failure in failures:
                    print(f"  ✗ {failure}", file=sys.stderr)
                raise SystemExit(f"{len(failures)} connecteur(s) en échec")
            print(
                f"\nBanc des connecteurs : {len(CASES)} connecteurs, {len(seen)} sauvegardes "
                "découvertes, nommées et publiées sur un vrai serveur."
            )
        finally:
            agent.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
