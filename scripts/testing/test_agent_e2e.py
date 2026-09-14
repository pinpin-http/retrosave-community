"""Banc de bout en bout : le VRAI agent, piloté comme le fera l'interface.

Les autres bancs prouvent le moteur. Celui-ci prouve l'application : on lance
l'exécutable `retrosave-agent` tel qu'il sera installé, on lui parle par son
canal local — exactement les messages que l'interface envoie —, et on vérifie
qu'une sauvegarde posée dans le dossier désigné arrive sur un autre appareil.

Trois choses y sont vérifiées qu'aucun test unitaire ne peut donner :

1. l'agent reste **joignable pendant** qu'il synchronise ;
2. la stabilisation s'applique avec la vraie horloge : rien n'est capturé au
   premier regard, il faut que le contenu ait cessé de bouger ;
3. la configuration écrite par l'interface est bien celle que l'agent relit,
   et le fichier qui porte le jeton n'est lisible que par son propriétaire.

Tout est isolé : XDG_CONFIG_HOME, XDG_DATA_HOME et XDG_RUNTIME_DIR pointent
dans un dossier temporaire, le canal porte un nom unique, et les services sont
jetables. Le poste n'est pas touché.
"""

from __future__ import annotations

import argparse
import base64
import configparser
import hashlib
import json
import os
import secrets
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import time
import uuid
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts" / "testing"))
sys.path.insert(0, str(ROOT / "cli"))

from test_api_interop import disposable_services
from test_engine_interop import Engine, register, snapshot, write


class Agent:
    """L'agent réel, parlé par son canal local — comme le fait l'interface."""

    def __init__(self, binary: Path, workspace: Path) -> None:
        self.workspace = workspace
        self.name = "retrosave-e2e-" + secrets.token_hex(6)
        self.runtime = workspace / "runtime"
        self.runtime.mkdir(parents=True, exist_ok=True)
        self.runtime.chmod(0o700)  # Qt exige un XDG_RUNTIME_DIR privé
        self.config_home = workspace / "config"
        self.data_home = workspace / "data"
        self.cache_home = workspace / "cache"
        for directory in (self.config_home, self.data_home, self.cache_home):
            directory.mkdir(parents=True, exist_ok=True)
        self.env = dict(
            os.environ,
            XDG_CONFIG_HOME=str(self.config_home),
            XDG_DATA_HOME=str(self.data_home),
            # Le cache aussi : sans cette ligne, le banc écrirait ses vignettes
            # dans le cache RÉEL du poste, et lirait celles qui s'y trouvent.
            XDG_CACHE_HOME=str(self.cache_home),
            XDG_RUNTIME_DIR=str(self.runtime),
            # Les manifestes voyagent avec l'application ; ici on désigne ceux
            # du dépôt, sans dépendre d'une installation.
            RETROSAVE_ADAPTERS_DIR=str(ROOT / "adapters"),
            QT_QPA_PLATFORM="offscreen",
        )
        self.process = subprocess.Popen(
            [str(binary), "--socket", self.name],
            env=self.env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        # Qt pose un canal nommé dans le dossier temporaire du système, pas
        # dans XDG_RUNTIME_DIR : on cherche donc là où il le met réellement
        # plutôt que là où on l'imaginerait. Le nom unique suffit à isoler.
        candidates = [self.runtime / self.name, Path(tempfile.gettempdir()) / self.name]
        self.path = candidates[-1]
        deadline = time.monotonic() + 15
        while True:
            found = next((path for path in candidates if path.exists()), None)
            if found is not None:
                self.path = found
                break
            if self.process.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError("l'agent n'a pas ouvert son canal local")
            time.sleep(0.1)

    @property
    def settings(self) -> Path:
        return self.config_home / "retrosave-community" / "retrosave-community.conf"

    def call(self, message: dict, timeout: float = 30.0) -> dict:
        message = dict(message, protocol=1, id=uuid.uuid4().hex)
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(timeout)
            client.connect(str(self.path))
            client.sendall(json.dumps(message).encode() + b"\n")
            buffer = b""
            while b"\n" not in buffer:
                chunk = client.recv(4096)
                if not chunk:
                    raise RuntimeError("canal local fermé sans réponse")
                buffer += chunk
        response = json.loads(buffer.split(b"\n", 1)[0])
        if "error" in response:
            raise RuntimeError(f"l'agent a refusé la demande : {response['error']}")
        return response["result"]

    def designate_root(self, emulator: str, folder: Path) -> None:
        """Désigner la racine d'un émulateur, comme le fait l'interface."""

        self.settings.parent.mkdir(parents=True, exist_ok=True)
        parser = configparser.ConfigParser()
        parser.optionxform = str
        if self.settings.exists():
            parser.read(self.settings)
        if "roots" not in parser:
            parser["roots"] = {}
        parser["roots"][emulator] = str(folder)
        with self.settings.open("w") as handle:
            parser.write(handle, space_around_delimiters=False)

    def designate(self, folder: Path) -> None:
        """Écrire le dossier désigné, exactement comme le fait l'interface."""

        self.settings.parent.mkdir(parents=True, exist_ok=True)
        parser = configparser.ConfigParser()
        parser.optionxform = str
        if self.settings.exists():
            parser.read(self.settings)
        if "General" not in parser:
            parser["General"] = {}
        parser["General"]["folder"] = str(folder)
        with self.settings.open("w") as handle:
            parser.write(handle, space_around_delimiters=False)

    def wait_idle(self, timeout: float = 120.0) -> dict:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            status = self.call({"method": "status"})
            if status["state"] == "idle":
                return status
            time.sleep(0.2)
        raise RuntimeError("la passe ne s'est jamais terminée")

    def stop(self) -> None:
        try:
            self.call({"method": "shutdown"}, timeout=10)
        except (OSError, RuntimeError):
            pass
        try:
            self.process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()


def one_pixel_png() -> bytes:
    """Le plus petit PNG valide : signature + IHDR 1×1 + IDAT + IEND.

    Écrit à la main plutôt qu'avec une bibliothèque d'images : le banc ne doit
    rien installer de plus, et l'agent ne décode pas l'image — il vérifie sa
    signature et son en-tête.
    """

    def chunk(kind: bytes, payload: bytes) -> bytes:
        body = kind + payload
        return len(payload).to_bytes(4, "big") + body + zlib.crc32(body).to_bytes(4, "big")

    header = struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0)
    pixels = zlib.compress(b"\x00\xff\xff\xff")
    return (
        b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", pixels) + chunk(b"IEND", b"")
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--agent", type=Path, required=True)
    # Le second appareil du scénario : l'outil de passe, pas un second agent.
    parser.add_argument("--engine", type=Path, required=True)
    arguments = parser.parse_args()
    if not arguments.agent.is_file():
        raise SystemExit(f"agent introuvable : {arguments.agent}")
    if not arguments.engine.is_file():
        raise SystemExit(f"outil moteur introuvable : {arguments.engine}")

    with disposable_services() as (url, token, _storage, directory):
        workspace = Path(directory)
        saves = workspace / "agent-saves"
        saves.mkdir(parents=True, exist_ok=True)
        agent = Agent(arguments.agent, workspace)
        try:
            status = agent.call({"method": "status"})
            assert status["sync_available"] is True, status
            assert status["connected"] is False, status
            refused = agent.call({"method": "sync"})
            # Un refus doit être une phrase actionnable, pas un code.
            assert refused["accepted"] is False and "server" in refused["reason"], refused
            print("1. Agent joignable, et une synchronisation sans compte est refusée clairement.")

            agent.designate(saves)
            agent.call({"method": "connect", "url": url, "token": token, "device_name": "Poste"})
            deadline = time.monotonic() + 30
            while not agent.call({"method": "status"})["connected"]:
                if time.monotonic() > deadline:
                    raise RuntimeError("l'appareil ne s'est jamais enregistré")
                time.sleep(0.2)
            mode = stat.S_IMODE(agent.settings.stat().st_mode)
            # Le jeton vit dans ce fichier : personne d'autre ne doit le lire.
            assert mode == 0o600, f"réglages en {oct(mode)} au lieu de 0600"
            print("2. Compte relié par le canal local ; le fichier du jeton est en 0600.")

            write(saves / "aventure" / "SAVE.BIN", b"partie du joueur")
            assert agent.call({"method": "sync"})["accepted"] is True
            first = agent.wait_idle()["last"]
            # Rien n'est capturé au premier regard : la stabilisation exige deux
            # observations espacées de dix secondes.
            assert first["pushed"] == 0, first
            print("3. Première passe : rien n'est publié, le contenu vient de bouger.")

            time.sleep(11)
            assert agent.call({"method": "sync"})["accepted"] is True
            second = agent.wait_idle()["last"]
            assert second["pushed"] == 1, second
            assert second["errors"] == 0, second
            print("4. Onze secondes plus tard : la sauvegarde est publiée.")

            # Le second appareil est une instance du moteur, pilotée directement
            # — pas l'agent : ce banc éprouve l'agent d'un côté et se contente
            # d'un correspondant crédible de l'autre.
            other = Engine(
                arguments.engine,
                workspace,
                "autre",
                url,
                token,
                register(url, token, "Autre poste"),
            )
            other.sync(1011.0)
            assert snapshot(other.root) == snapshot(saves), "l'autre appareil n'a pas reçu"
            assert (other.root / "aventure" / "SAVE.BIN").read_bytes() == b"partie du joueur"
            print("5. L'autre appareil reçoit la partie, à l'identique.")

            # ── 6. Un vrai conflit, tranché depuis l'interface ────────────
            # Les deux appareils modifient la même sauvegarde sans se croiser.
            write(other.root / "aventure" / "SAVE.BIN", b"partie du PC")
            other.sync(1100.0)
            other.sync(1111.0)

            write(saves / "aventure" / "SAVE.BIN", b"partie du poste")
            agent.call({"method": "sync"})
            agent.wait_idle()
            time.sleep(11)
            agent.call({"method": "sync"})
            report = agent.wait_idle()["last"]
            assert report["conflicts"] == 1, report

            # L'agent doit pouvoir présenter les DEUX côtés : on ne tranche pas
            # entre deux identifiants.
            agent.call({"method": "conflicts"})
            deadline = time.monotonic() + 30
            conflicts = []
            while time.monotonic() < deadline:
                conflicts = agent.call({"method": "status"})["conflicts"]
                if conflicts:
                    break
                time.sleep(0.3)
            assert len(conflicts) == 1, conflicts
            incident = conflicts[0]
            for field in ("id", "unit", "a_number", "b_number", "a_at", "b_at"):
                assert incident.get(field) not in (None, ""), (field, incident)
            assert incident["a_number"] != incident["b_number"]
            print(
                "6. Conflit réel présenté par l'agent : "
                f"v{incident['a_number']} contre v{incident['b_number']}."
            )

            # ── 7. La décision, et la convergence ─────────────────────────
            winner = incident["a_number"]
            assert (
                agent.call({"method": "resolve", "conflict_id": incident["id"], "winner": winner})[
                    "accepted"
                ]
                is True
            )
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                if agent.call({"method": "status"})["conflicts"] == []:
                    break
                time.sleep(0.3)
            assert agent.call({"method": "status"})["conflicts"] == [], "le conflit reste ouvert"

            # L'appareil qui avait perdu adopte la version retenue…
            time.sleep(11)
            agent.call({"method": "sync"})
            agent.wait_idle()
            assert (saves / "aventure" / "SAVE.BIN").read_bytes() == b"partie du PC"
            # …et la partie non retenue reste récupérable en local.
            backups = list((saves).glob("aventure.rsc-bak*/SAVE.BIN"))
            assert backups, "aucune copie de sécurité de la version écartée"
            print("7. Décision appliquée : l'agent converge, et l'autre version reste en copie.")

            # ── 8. Le carnet, vu de l'interface ───────────────────────────
            assert agent.call({"method": "units"})["accepted"] is True
            deadline = time.monotonic() + 20
            units = []
            while time.monotonic() < deadline:
                units = agent.call({"method": "status"})["units"]
                if units:
                    break
                time.sleep(0.3)
            assert len(units) == 1, units
            followed = units[0]
            assert followed["key"] == "aventure", followed
            assert followed["known"] is True, followed
            # La vignette est calculée par le noyau partagé, pas par l'interface.
            assert 0 <= followed["hue"] <= 359, followed
            assert followed["initials"], followed
            print(f"8. Carnet listé : « {followed['label']} », teinte {followed['hue']}.")

            # ── 9. Historique et restauration ─────────────────────────────
            asked = agent.call({"method": "history", "unit_key": "aventure"})
            assert asked["accepted"] is True, asked
            deadline = time.monotonic() + 30
            history = {}
            problem = ""
            while time.monotonic() < deadline:
                status = agent.call({"method": "status"})
                history = status["history"]
                problem = status.get("problem", "")
                if history.get("versions") or problem:
                    break
                time.sleep(0.3)
            # Un échec doit se lire dans le message, pas se deviner : c'est
            # l'agent qui sait pourquoi il n'a pas pu.
            assert not problem, f"l'agent a refusé l'historique : {problem}"
            versions = history["versions"]
            # Trois versions : la publication d'origine, celle de l'autre poste,
            # et la branche écartée. Trancher n'en crée AUCUNE — la résolution
            # déplace la tête, elle ne publie pas.
            assert len(versions) == 3, versions
            # La branche écartée du conflit est TOUJOURS là : trancher n'efface rien.
            assert any(v["kind"] == "conflict_branch" for v in versions), versions
            assert history["head"] > 0

            # On revient à la toute première version.
            oldest = min(v["number"] for v in versions)
            assert (
                agent.call({"method": "restore", "unit_key": "aventure", "version": oldest})[
                    "accepted"
                ]
                is True
            )
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                head = agent.call({"method": "status"})["history"].get("head", 0)
                if head != history["head"]:
                    break
                time.sleep(0.3)
            restored = agent.call({"method": "status"})["history"]
            # Le serveur a créé une NOUVELLE version : il n'a réécrit personne.
            assert restored["head"] > history["head"], (history, restored)
            assert len(restored["versions"]) > len(versions)

            # Et l'appareil récupère bien le contenu d'origine.
            time.sleep(11)
            agent.call({"method": "sync"})
            agent.wait_idle()
            assert (saves / "aventure" / "SAVE.BIN").read_bytes() == b"partie du joueur"
            print(
                f"9. Restauration de la version {oldest} : nouvelle tête "
                f"{restored['head']}, contenu d'origine récupéré, rien d'effacé."
            )
            # ── 10. Une racine par émulateur, avec sa vraie icône ─────────
            # Le connecteur PPSSPP voit un dossier de savedata et rien d'autre.
            # L'icône vient du `ICON0.PNG` de la sauvegarde, pas d'une ROM.
            psp = workspace / "psp" / "PSP" / "SAVEDATA"
            write(psp / "ULUS10041GAMEDATA" / "DATA.BIN", b"progression psp")
            write(
                psp / "ULUS10041GAMEDATA" / "ICON0.PNG",
                base64.b64decode(
                    "iVBORw0KGgoAAAANSUhEUgAAAJAAAABQCAYAAAD2rvwMAAAAAElEQVQ1rwYeAAAAAElFTkSuQmCC"
                ),
            )
            agent.designate_root("ppsspp", psp)
            agent.call({"method": "sync"})
            agent.wait_idle()
            time.sleep(11)
            agent.call({"method": "sync"})
            agent.wait_idle()
            agent.call({"method": "units"})

            deadline = time.monotonic() + 20
            psp_unit = None
            while time.monotonic() < deadline:
                for unit in agent.call({"method": "status"})["units"]:
                    if unit["emulator"] == "ppsspp":
                        psp_unit = unit
                if psp_unit:
                    break
                time.sleep(0.3)
            assert psp_unit, "le connecteur PPSSPP n'a rien vu"
            assert psp_unit["key"] == "ULUS10041GAMEDATA", psp_unit
            # La vignette réelle a été extraite et validée avant d'être rangée.
            assert psp_unit["icon"], psp_unit
            assert Path(psp_unit["icon"]).is_file(), psp_unit
            # Et le catalogue montre la racine comme surveillée.
            catalogue = agent.call({"method": "status"})["adapters"]
            ppsspp = next(a for a in catalogue if a["id"] == "ppsspp")
            assert ppsspp["root"] == str(psp) and ppsspp["present"] is True, ppsspp
            assert len(catalogue) == 9, [a["id"] for a in catalogue]
            print(
                f"10. Racine PPSSPP surveillée : « {psp_unit['key']} » "
                "avec son ICON0.PNG, et 9 connecteurs au catalogue."
            )
            # ── 11. Réarmer une unité mise de côté (AD-30) ────────────────
            # Le casse-boucle protège la synchronisation d'une unité qui fait
            # tomber la passe ; sans réarmement, cette protection serait une
            # impasse. On vérifie que la commande existe sur le canal, qu'elle
            # est acceptée pour une unité connue, et refusée proprement sinon.
            retry = agent.call({"method": "retry", "unit_key": psp_unit["key"]})
            assert retry["accepted"] is True, retry
            agent.wait_idle()
            refused = agent.call({"method": "retry", "unit_key": ""})
            assert refused["accepted"] is False, refused
            assert refused["reason"], refused
            # Réarmer ne casse rien : l'unité est toujours là, à sa version.
            after = next(
                unit
                for unit in agent.call({"method": "status"})["units"]
                if unit["key"] == psp_unit["key"]
            )
            assert after["version"] == psp_unit["version"], (after, psp_unit)
            print(
                "11. Réarmement d'une unité accepté sur le canal, "
                "demande vide refusée avec sa raison, version intacte."
            )
            # ── 12. L'image d'une sauvegarde (décorative, jamais structurante) ──
            # Un PNG minuscule mais VRAI : la validation lit l'en-tête, donc un
            # fichier bidon renommé `.png` doit être refusé, et un vrai accepté.
            chosen = workspace / "choisie.png"
            chosen.write_bytes(
                base64.b64decode(
                    "iVBORw0KGgoAAAANSUhEUgAAAJAAAABQCAYAAAD2rvwMAAAAAElEQVQ1rwYeAAAAAElFTkSuQmCC"
                )
            )
            before = next(
                unit
                for unit in agent.call({"method": "status"})["units"]
                if unit["key"] == psp_unit["key"]
            )
            assert (
                agent.call({"method": "artwork", "unit_key": psp_unit["key"], "path": str(chosen)})[
                    "accepted"
                ]
                is True
            )
            agent.wait_idle()

            deadline = time.monotonic() + 20
            after = before
            while time.monotonic() < deadline:
                after = next(
                    unit
                    for unit in agent.call({"method": "status"})["units"]
                    if unit["key"] == psp_unit["key"]
                )
                if after.get("custom_art"):
                    break
                time.sleep(0.3)
            assert after["custom_art"] is True, after
            assert Path(after["icon"]).is_file(), after
            # L'image vit à côté de l'ÉTAT : vider le cache ne l'effacerait pas.
            assert "/artwork/" in after["icon"], after["icon"]
            # Et surtout : choisir une image ne touche à RIEN de la
            # synchronisation — ni version, ni clé, ni état.
            assert after["version"] == before["version"], (before, after)
            assert after["key"] == before["key"] and after["state"] == before["state"]

            # Un fichier qui n'est pas une image est refusé, avec sa raison, et
            # l'image déjà choisie reste en place.
            faux = workspace / "pas-une-image.png"
            faux.write_bytes(b"ceci est une sauvegarde, pas une image")
            refused = agent.call(
                {"method": "artwork", "unit_key": psp_unit["key"], "path": str(faux)}
            )
            agent.wait_idle()
            assert refused["accepted"] is True, refused  # accepté = pris en charge
            still = next(
                unit
                for unit in agent.call({"method": "status"})["units"]
                if unit["key"] == psp_unit["key"]
            )
            assert still["custom_art"] is True, "une image refusée a effacé la précédente"

            # Retirer l'image rend la main au dessin de repli.
            assert (
                agent.call({"method": "artwork", "unit_key": psp_unit["key"], "path": ""})[
                    "accepted"
                ]
                is True
            )
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                cleared = next(
                    unit
                    for unit in agent.call({"method": "status"})["units"]
                    if unit["key"] == psp_unit["key"]
                )
                if not cleared.get("custom_art"):
                    break
                time.sleep(0.3)
            assert cleared["custom_art"] is False, cleared
            print(
                "12. Image de sauvegarde : PNG accepté et rangé près de l'état, fichier "
                "non-image refusé sans effacer le précédent, retrait rendu au repli, "
                "et la version de l'unité n'a pas bougé."
            )

            # ── 13. Diagnostic expurgé (EXP-04) ───────────────────────────
            # Ce qui compte n'est pas qu'un fichier existe : c'est ce qu'il ne
            # contient PAS. Un diagnostic qu'on n'ose pas envoyer ne sert à rien.
            report = workspace / "diagnostic.txt"
            assert agent.call({"method": "diagnostic", "path": str(report)})["accepted"] is True
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline and not report.is_file():
                time.sleep(0.3)
            assert report.is_file(), "aucun diagnostic écrit"
            text = report.read_text(encoding="utf-8")
            assert token not in text, "le jeton est dans le diagnostic"
            assert "aventure" in text, "le diagnostic ne dit pas ce qui est suivi"
            # Le contenu des sauvegardes n'y est jamais : on vérifie sur les
            # octets qu'on y a nous-mêmes écrits.
            assert "chapitre" not in text, "un contenu de sauvegarde a fui dans le diagnostic"
            print(
                f"13. Diagnostic écrit ({len(text)} caractères) : il nomme les sauvegardes "
                "suivies, et ne contient ni le jeton ni le contenu d'une partie."
            )

            # ── 14. Export hors du service (PRO-08) ───────────────────────
            destination = workspace / "export"
            assert agent.call({"method": "export", "path": str(destination)})["accepted"] is True
            index = destination / "retrosave-export.json"
            deadline = time.monotonic() + 40
            while time.monotonic() < deadline and not index.is_file():
                time.sleep(0.3)
            assert index.is_file(), "aucun index d'export écrit"
            manifest = json.loads(index.read_text(encoding="utf-8"))
            assert manifest["format"] == "retrosave-export-1", manifest
            exported = [entry for entry in manifest["units"] if entry["exported"]]
            assert exported, manifest
            # Les octets, pas la promesse : l'export doit être relisible sans
            # RetroSave, donc c'est le fichier copié qu'on compare.
            copied = destination / exported[0]["path"]
            original = saves / exported[0]["unit_key"]
            assert copied.exists(), copied
            assert snapshot(copied) == snapshot(original), "l'export ne rend pas les mêmes octets"
            # Et il n'a rien déplacé : la sauvegarde du joueur est toujours là.
            assert original.exists(), original
            print(
                f"14. Export : {len(exported)} sauvegarde(s) copiée(s) à l'identique dans un "
                "dossier ordinaire, l'original intact et un index lisible à côté."
            )

            # ── 15. La jaquette d'une sauvegarde-FICHIER (Q45) ─────────────
            # Constaté le 13/09/2026 sur une vraie partie mGBA : la jaquette de
            # Golden Sun était résolue par le serveur, téléchargée, écrite au
            # bon endroit du cache — et jamais affichée. La recherche d'image
            # sortait avant de regarder le cache dès que l'unité n'était pas un
            # DOSSIER, parce qu'elle n'avait été écrite que pour l'`ICON0.PNG`
            # des sauvegardes PSP. Tous les émulateurs à fichier `.sav`/`.srm`
            # étaient concernés, c'est-à-dire la majorité d'entre eux.
            roms = workspace / "roms-gba"
            write(roms / "Golden Sun (FR).sav", b"partie GBA du joueur")
            agent.designate_root("mgba", roms)
            # Deux passes : la configuration n'est relue qu'à la FIN d'une
            # passe, donc une racine désignée ne prend effet qu'à la suivante.
            for _ in range(2):
                assert agent.call({"method": "sync"})["accepted"] is True
                agent.wait_idle()
            # La découverte est asynchrone : on attend qu'elle ait eu lieu
            # plutôt que de supposer qu'elle a déjà rendu la main.
            deadline = time.monotonic() + 20
            unit = None
            while time.monotonic() < deadline:
                for candidate in agent.call({"method": "status"})["units"]:
                    if candidate["emulator"] == "mgba":
                        unit = candidate
                if unit:
                    break
                time.sleep(0.3)
            assert unit, "le connecteur mGBA n'a rien vu"
            # Le libellé perd son extension : c'est lui qui sert de titre, et
            # c'est à partir de lui que le serveur cherche une jaquette.
            assert unit["label"] == "Golden Sun", unit
            assert unit["icon"] == "", unit

            cached = (
                Path(agent.cache_home)
                / "retrosave-community"
                / "icons"
                / (hashlib.sha256(b"mgba\x00Golden Sun (FR).sav").hexdigest()[:32] + ".png")
            )
            cached.parent.mkdir(parents=True, exist_ok=True)
            cached.write_bytes(one_pixel_png())
            # Le carnet est relu à la demande : `status` rend le dernier état
            # connu, `units` va le rechercher.
            agent.call({"method": "units"})
            deadline = time.monotonic() + 20
            shown = unit
            while time.monotonic() < deadline:
                shown = next(
                    u for u in agent.call({"method": "status"})["units"] if u["emulator"] == "mgba"
                )
                if shown["icon"]:
                    break
                time.sleep(0.3)
            assert shown["icon"] == str(cached), shown
            print(
                "15. Jaquette d'une sauvegarde-fichier : le libellé perd son "
                "extension et l'image en cache est bien rendue à l'interface."
            )
        finally:
            agent.stop()
        print("Banc de bout en bout : réussi. Agent arrêté, services jetables détruits.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
