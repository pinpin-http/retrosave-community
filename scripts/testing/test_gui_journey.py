#!/usr/bin/env python3
"""Parcours graphique complet, piloté par Spix contre des services jetables.

Ce banc ne « lance pas l'application hors écran » : il **clique** dans la vraie
fenêtre. Chaque étape agit sur un contrôle désigné par son `objectName` et
attend une **condition observable** — jamais une coordonnée, jamais un délai
arbitraire. Ce qui est affiché ne suffit pas non plus : à chaque fois qu'une
promesse porte sur des données, elle est vérifiée là où elles vivent — fichiers
du joueur, copies de sécurité, versions du serveur.

Ce que ce banc NE pilote PAS, et pourquoi :

- **le choix du dossier de sauvegardes.** C'est un sélecteur natif du système,
  hors de portée de tout pilotage Qt. Il est donc écrit dans les réglages avant
  le lancement, exactement comme le ferait l'utilisateur avec sa souris. C'est
  la seule étape du parcours qui n'est pas cliquée, et elle est signalée comme
  telle dans le compte rendu.

Le dossier surveillé est la **racine générique** de l'application — le réglage
« Dossier de sauvegardes » — avec un contenu en forme de savedata PSP. La
découverte par le CONNECTEUR PPSSPP, elle, est prouvée séparément par
`test_agent_e2e.py` : mélanger les deux racines créerait des unités en double
et ne prouverait ni l'une ni l'autre.

Le canal de pilotage n'existe que dans une compilation dédiée
(`-DRSC_WITH_TEST_DRIVER=ON`) et n'écoute que sur `127.0.0.1`.
"""

from __future__ import annotations

import argparse
import base64
import configparser
import http.client
import json
import os
import secrets
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import uuid
import xmlrpc.client
from pathlib import Path

import httpx

ROOT = Path(__file__).resolve().parents[2]

# Un PNG minuscule mais VRAI : l'agent valide l'en-tête, donc un fichier bidon
# renommé `.png` serait refusé — et c'est bien ce qu'on veut de lui.
TINY_PNG = "iVBORw0KGgoAAAANSUhEUgAAAJAAAABQCAYAAAD2rvwMAAAAAElEQVQ1rwYeAAAAAElFTkSuQmCC"
sys.path.insert(0, str(Path(__file__).resolve().parent))

from test_api_interop import disposable_services
from test_engine_interop import Engine, register
from test_engine_interop import write as write_unit


def free_port() -> int:
    """Un port libre au moment du choix. La course résiduelle est acceptée :
    le canal est local et l'échec serait immédiat et bruyant."""
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return int(probe.getsockname()[1])


def write(path: Path, payload: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(payload)


class Journey:
    """La fenêtre réelle, son agent réel, et le canal qui les pilote."""

    def __init__(self, agent_binary: Path, gui_binary: Path, workspace: Path) -> None:
        self.workspace = workspace
        self.socket_name = "retrosave-gui-" + secrets.token_hex(6)
        self.port = free_port()
        runtime = workspace / "runtime"
        runtime.mkdir(parents=True, exist_ok=True)
        runtime.chmod(0o700)  # Qt exige un XDG_RUNTIME_DIR privé
        self.config_home = workspace / "config"
        self.data_home = workspace / "data"
        for directory in (self.config_home, self.data_home):
            directory.mkdir(parents=True, exist_ok=True)
        # Isolation des données : l'agent et la fenêtre partagent CES dossiers-ci
        # et aucun autre. Rien de ce que fait ce banc ne touche l'installation
        # réelle du poste.
        self.env = dict(
            os.environ,
            HOME=str(workspace),
            XDG_CONFIG_HOME=str(self.config_home),
            XDG_DATA_HOME=str(self.data_home),
            XDG_CACHE_HOME=str(workspace / "cache"),
            XDG_STATE_HOME=str(workspace / "state"),
            XDG_RUNTIME_DIR=str(runtime),
            RETROSAVE_ADAPTERS_DIR=str(ROOT / "adapters"),
            QT_QPA_PLATFORM="offscreen",
            QT_QUICK_BACKEND="software",
            QT_FORCE_STDERR_LOGGING="1",
        )
        # Le poste de développement exporte souvent `QT_QPA_PLATFORMTHEME=gtk3` :
        # le greffon GTK tente alors d'ouvrir un affichage X et l'application
        # s'arrête, même sous `offscreen`. Un banc sans écran ne doit hériter ni
        # du thème du bureau, ni de son affichage.
        for inherited in ("QT_QPA_PLATFORMTHEME", "QT_STYLE_OVERRIDE"):
            self.env.pop(inherited, None)
        # Ces deux journaux vivent aussi longtemps que les processus qu'ils
        # captent : un `with` les fermerait avant la fin du parcours. Ils sont
        # refermés dans `stop()`.
        self.agent_log = open(workspace / "agent.log", "w", encoding="utf-8")  # noqa: SIM115
        self.gui_log = open(workspace / "gui.log", "w", encoding="utf-8")  # noqa: SIM115
        self.agent = subprocess.Popen(
            [str(agent_binary), "--socket", self.socket_name],
            env=self.env,
            stdout=self.agent_log,
            stderr=subprocess.STDOUT,
        )
        self.channel = self._await_channel()
        self.gui_binary = gui_binary
        self.gui: subprocess.Popen | None = None
        self.rpc: xmlrpc.client.ServerProxy | None = None

    def _await_channel(self) -> Path:
        candidates = [
            self.workspace / "runtime" / self.socket_name,
            Path(tempfile.gettempdir()) / self.socket_name,
        ]
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            found = next((path for path in candidates if path.exists()), None)
            if found is not None:
                return found
            if self.agent.poll() is not None:
                raise RuntimeError("l'agent s'est arrêté avant d'ouvrir son canal")
            time.sleep(0.1)
        raise RuntimeError("l'agent n'a pas ouvert son canal local")

    # ── Le canal de l'agent : sert UNIQUEMENT à vérifier, jamais à agir ────
    def ask_agent(self, message: dict, timeout: float = 30.0) -> dict:
        message = dict(message, protocol=1, id=uuid.uuid4().hex)
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(timeout)
            client.connect(str(self.channel))
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

    def designate_folder(self, folder: Path) -> None:
        """Le sélecteur de dossier est NATIF : on écrit ce qu'il aurait écrit."""
        settings = self.config_home / "retrosave-community" / "retrosave-community.conf"
        settings.parent.mkdir(parents=True, exist_ok=True)
        parser = configparser.ConfigParser()
        parser.optionxform = str
        if settings.exists():
            parser.read(settings)
        # La clé est celle qu'écrit l'interface elle-même (`General/folder`) :
        # inventer un autre nom donnerait un banc vert sur un réglage que
        # l'application n'aurait jamais lu.
        if "General" not in parser:
            parser["General"] = {}
        parser["General"]["folder"] = str(folder)
        with settings.open("w", encoding="utf-8") as handle:
            parser.write(handle, space_around_delimiters=False)
        settings.chmod(0o600)

    # ── La fenêtre ────────────────────────────────────────────────────────
    def start_gui(self) -> None:
        self.gui = subprocess.Popen(
            [
                str(self.gui_binary),
                "--socket",
                self.socket_name,
                "--test-driver-port",
                str(self.port),
            ],
            env=self.env,
            stdout=self.gui_log,
            stderr=subprocess.STDOUT,
        )
        self.rpc = xmlrpc.client.ServerProxy(f"http://127.0.0.1:{self.port}", allow_none=True)
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            try:
                self.rpc.waitForItem("mainWindow/nav_sync", 500)
                return
            except (OSError, xmlrpc.client.Error, http.client.HTTPException):
                if self.gui.poll() is not None:
                    raise RuntimeError("la fenêtre s'est arrêtée avant d'ouvrir son canal")
                time.sleep(0.2)
        raise RuntimeError("le canal de pilotage n'a jamais répondu")

    def click(self, path: str) -> None:
        # Plusieurs actions lancent une requête locale puis désactivent les
        # boutons jusqu'à sa réponse. Attendre la propriété du vrai contrôle
        # évite qu'un événement souris envoyé pendant ce bref intervalle soit
        # correctement ignoré par Qt et perdu par le parcours.
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline and self.rpc.getString(path, "enabled") != "true":
            time.sleep(0.05)
        if self.rpc.getString(path, "enabled") != "true":
            self.rescue_screenshot()
            raise AssertionError(f"le contrôle n'est jamais devenu actif : {path}")
        self.rpc.click(path)

    def type_into(self, path: str, text: str) -> None:
        # Au lancement, le premier sondage de l'agent désactive brièvement les
        # champs. Une frappe envoyée pendant cette fenêtre est correctement
        # ignorée par Qt. On attend donc que le contrôle soit actif, puis on
        # envoie une seule vraie saisie. Spix poste l'événement clavier de façon
        # asynchrone : le renvoyer avant que Qt l'ait traité dupliquerait le
        # serveur, le jeton ou le nom de l'appareil.
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline and self.rpc.getString(path, "enabled") != "true":
            time.sleep(0.1)
        if self.rpc.getString(path, "enabled") != "true":
            self.rescue_screenshot()
            raise AssertionError(f"le champ n'est jamais devenu actif : {path}")
        self.rpc.click(path)
        # Le backend offscreen du runner ne propage pas toujours le focus après
        # les événements souris synthétiques. Cette commande de test est
        # volontairement bornée à forceActiveFocus(); le texte continue
        # d'arriver sous forme d'événement clavier dans le vrai TextField.
        self.rpc.focus(path)
        while time.monotonic() < deadline and self.rpc.getString(path, "activeFocus") != "true":
            time.sleep(0.05)
        if self.rpc.getString(path, "activeFocus") != "true":
            self.rescue_screenshot()
            raise AssertionError(f"le champ n'a jamais reçu le focus : {path}")
        self.rpc.inputText(path, text)
        while time.monotonic() < deadline:
            if self.rpc.getString(path, "text") == text:
                return
            time.sleep(0.1)
        self.rescue_screenshot()
        raise AssertionError(f"la saisie n'est jamais apparue dans {path}")

    def until(self, description: str, condition, timeout: float = 45.0) -> None:
        """Attendre une CONDITION, pas une durée. Le message d'échec dit ce
        qu'on attendait — un test qui expire sans le dire ne sert à rien."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if condition():
                return
            time.sleep(0.25)
        self.rescue_screenshot()
        raise AssertionError(f"jamais obtenu : {description}")

    def wait_idle(self) -> dict:
        """L'agent a fini sa passe. On interroge son canal plutôt que de dormir :
        une passe dure ce qu'elle dure, et une temporisation fixe finit toujours
        par être trop courte sur une machine chargée."""
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            status = self.ask_agent({"method": "status"})
            if status["state"] == "idle":
                return status
            time.sleep(0.3)
        raise AssertionError("la passe ne s'est jamais terminée")

    def visible(self, path: str) -> bool:
        try:
            return bool(self.rpc.exists(path))
        except (OSError, xmlrpc.client.Error, http.client.HTTPException):
            # Le canal peut être occupé une fraction de seconde : « pas encore »
            # n'est pas « faux », mais l'appelant réessaie de toute façon.
            return False

    def rescue_screenshot(self) -> None:
        """Une capture au moment de l'échec vaut mieux qu'un journal seul."""
        failure_directory = Path(os.environ.get("RSC_FAILURE_DIR", self.workspace))
        failure_directory.mkdir(parents=True, exist_ok=True)
        # Les fichiers sont encore ouverts à cet instant : vider les tampons,
        # puis en copier un instantané stable avant que le dossier jetable du
        # serveur ne disparaisse à la sortie du contexte.
        for handle, name in ((self.agent_log, "agent.log"), (self.gui_log, "gui.log")):
            handle.flush()
            shutil.copy2(handle.name, failure_directory / name)
        if self.rpc is None:
            return
        try:
            target = failure_directory / "echec.png"
            self.rpc.screenshot("mainWindow/page_sync", str(target))
            time.sleep(1.0)
            print(f"    capture de l'échec : {target}", file=sys.stderr)
        except (OSError, xmlrpc.client.Error, http.client.HTTPException) as problem:
            # Une capture manquée ne doit jamais masquer l'échec d'origine.
            print(f"    capture impossible : {problem}", file=sys.stderr)

    def stop(self) -> None:
        for process, closer in ((self.gui, self.gui_log), (self.agent, self.agent_log)):
            if process is None:
                continue
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
            closer.close()


def server_versions(url: str, token: str, observer: str, unit_key: str) -> list[dict]:
    """L'état du SERVEUR, lu directement.

    Ce que la fenêtre affiche n'est pas une preuve de ce qui a été publié : on
    interroge donc l'API avec un appareil « observateur » distinct, qui ne
    synchronise rien et ne peut donc pas influencer le résultat.
    """
    headers = {"Authorization": f"Bearer {token}", "X-Device-Id": observer}
    with httpx.Client(timeout=20, trust_env=False) as http:
        units = http.get(f"{url}/v0/units", headers=headers)
        units.raise_for_status()
        unit = next(
            (entry for entry in units.json()["units"] if entry["unit_key"] == unit_key), None
        )
        if unit is None:
            return []
        versions = http.get(f"{url}/v0/units/{unit['id']}/versions", headers=headers)
        versions.raise_for_status()
        return versions.json()["versions"]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--agent", type=Path, required=True, help="binaire retrosave-agent (compilation ordinaire)"
    )
    parser.add_argument(
        "--gui",
        type=Path,
        required=True,
        help="binaire retrosave-desktop compilé avec RSC_WITH_TEST_DRIVER=ON",
    )
    parser.add_argument(
        "--engine",
        type=Path,
        required=True,
        help="binaire retrosave-engine-interop, qui joue le SECOND appareil",
    )
    arguments = parser.parse_args()
    for binary in (arguments.agent, arguments.gui, arguments.engine):
        if not binary.is_file():
            raise SystemExit(f"binaire introuvable : {binary}")

    with disposable_services() as (url, token, _storage, directory):
        workspace = Path(directory) / "gui"
        workspace.mkdir(parents=True, exist_ok=True)
        saves = workspace / "sauvegardes"
        saves.mkdir(parents=True, exist_ok=True)
        journey = Journey(arguments.agent, arguments.gui, workspace)
        try:
            journey.designate_folder(saves)
            journey.start_gui()
            print(
                "0. Dossier de sauvegardes désigné (sélecteur natif, non pilotable) "
                "puis fenêtre lancée et canal de pilotage joignable."
            )

            # ── 1. Connexion, entièrement à la souris et au clavier ────────
            journey.click("mainWindow/nav_parametres")
            journey.until(
                "la page Paramètres visible", lambda: journey.visible("mainWindow/page_parametres")
            )
            journey.type_into("mainWindow/champ_serveur", url)
            journey.type_into("mainWindow/champ_jeton", token)
            journey.type_into("mainWindow/champ_appareil", "Poste de test")
            journey.click("mainWindow/bouton_relier")
            journey.until(
                "l'appareil enregistré auprès du serveur",
                lambda: journey.ask_agent({"method": "status"})["connected"],
            )
            print(
                "1. Connexion : trois champs saisis et « Relier ce poste » cliqué ; "
                "l'appareil est enregistré côté serveur."
            )

            # ── 2. Synchronisation ────────────────────────────────────────
            unit = "ULUS10041GAMEDATA"
            observer = register(url, token, "Observateur du banc")
            write(saves / unit / "DATA.BIN", b"progression du joueur")
            journey.click("mainWindow/nav_sync")
            journey.until(
                "la page Synchronisation visible", lambda: journey.visible("mainWindow/page_sync")
            )
            journey.click("mainWindow/bouton_synchroniser")
            first = journey.wait_idle()["last"]
            # Rien n'est capturé au premier regard : la stabilisation exige deux
            # observations espacées de dix secondes. Publier plus tôt serait
            # capturer une écriture peut-être encore en cours.
            assert first["pushed"] == 0, first
            time.sleep(11)
            journey.click("mainWindow/bouton_synchroniser")
            journey.until(
                "une version publiée sur le serveur",
                lambda: len(server_versions(url, token, observer, unit)) >= 1,
            )
            versions = server_versions(url, token, observer, unit)
            assert versions[0]["number"] == 1, versions
            print(
                f"2. Synchronisation lancée depuis la fenêtre : version "
                f"{versions[0]['number']} publiée, vérifiée sur le serveur."
            )

            # ── 3. Conflit provoqué par un AUTRE appareil ─────────────────
            # C'est le cas réel : la fenêtre ne fabrique pas le conflit, elle le
            # découvre. Le second appareil est une seconde instance du MÊME
            # moteur C++ — le client Python a été retiré le 09/09/2026, et ce
            # banc l'importait encore, ce qui le rendait inexécutable. Il publie
            # sa propre version de la même unité sans avoir vu celle du poste, et
            # le serveur range la seconde à part (invariant I3, aucun écrasement).
            other = Engine(
                arguments.engine,
                workspace,
                "autre",
                url,
                token,
                register(url, token, "Autre appareil"),
            )
            # D'abord il se met à jour : un appareil qui n'a JAMAIS vu l'unité
            # ne fabrique pas de conflit, il reçoit — c'est le comportement
            # attendu, et c'est ce que fait ici la première passe.
            received = other.sync(1000.0)
            assert received["pulled"] == 1, received

            # Puis les deux côtés divergent, chacun hors ligne de l'autre :
            # c'est LE scénario du produit, pas une situation de laboratoire.
            write_unit(other.root / unit / "DATA.BIN", "progression de l’autre appareil".encode())
            write(saves / unit / "DATA.BIN", b"progression du joueur, suite")
            journey.click("mainWindow/nav_sync")
            journey.until(
                "la page Synchronisation visible", lambda: journey.visible("mainWindow/page_sync")
            )
            journey.click("mainWindow/bouton_synchroniser")
            time.sleep(11)
            journey.click("mainWindow/bouton_synchroniser")
            journey.until(
                "la version 2 publiée par la fenêtre",
                lambda: len(server_versions(url, token, observer, unit)) >= 2,
            )

            # Le second appareil pousse sa propre version sur une base devenue
            # périmée : le serveur la range à part plutôt que d'écraser (I3).
            other.sync(1011.0)
            branched = other.sync(1022.0)
            assert branched["conflicts"] == 1, branched
            journey.until(
                "trois versions au serveur, dont une branche de conflit",
                lambda: len(server_versions(url, token, observer, unit)) >= 3,
            )

            journey.click("mainWindow/nav_conflits")
            journey.until(
                "la page Conflits visible", lambda: journey.visible("mainWindow/page_conflits")
            )
            journey.click("mainWindow/bouton_verifier_conflits")
            journey.until(
                "les deux branches présentées à l'écran",
                lambda: (
                    journey.visible("mainWindow/branche_a")
                    and journey.visible("mainWindow/branche_b")
                ),
            )
            print(
                "3. Conflit découvert : les deux branches sont présentées côte à côte "
                "dans la fenêtre, sans qu'aucune version ait été écrasée."
            )

            # ── 4. Résolution par un clic ─────────────────────────────────
            before = len(server_versions(url, token, observer, unit))
            journey.click("mainWindow/branche_a/bouton_garder")
            journey.until(
                "le conflit refermé",
                lambda: not journey.ask_agent({"method": "status"})["conflicts"],
            )
            after = server_versions(url, token, observer, unit)
            # I3 : la branche perdante RESTE dans l'historique.
            assert len(after) >= before, (before, after)
            print(
                f"4. Résolution cliquée : conflit refermé, {len(after)} versions toujours "
                "présentes au serveur — la branche perdante n'a pas été supprimée."
            )

            # ── 5. Historique ─────────────────────────────────────────────
            journey.click("mainWindow/nav_historique")
            journey.until(
                "la page Historique visible", lambda: journey.visible("mainWindow/page_historique")
            )
            journey.until(
                f"la ligne de l'unité {unit}", lambda: journey.visible(f"mainWindow/unite_{unit}")
            )
            journey.click(f"mainWindow/unite_{unit}")
            journey.until("la version 1 listée", lambda: journey.visible("mainWindow/version_1"))
            print("5. Historique : l'unité ouverte depuis la liste, ses versions affichées.")

            # ── 6. Restauration, et ce qu'elle laisse sur le disque ────────
            # On restaure la branche PERDANTE, pas la gagnante : restaurer un
            # contenu identique à celui déjà en place n'écrit rien, donc ne
            # prouve ni le remplacement ni la copie de sécurité. C'est
            # exactement le genre de test qui passe sans rien démontrer.
            # Les trois versions en présence : 1 le premier envoi, 2 la
            # modification du joueur gardée par l'arbitrage, 3 la branche du
            # second appareil — celle qui a perdu, et qui reste téléchargeable.
            target = saves / unit / "DATA.BIN"
            assert target.read_bytes() == b"progression du joueur, suite", target.read_bytes()
            journey.until("la version 3 listée", lambda: journey.visible("mainWindow/version_3"))
            # Le bouton est désactivé pendant qu'une opération tourne : cliquer
            # avant que l'agent soit au repos ne ferait rien du tout, et le banc
            # attendrait ensuite une version qui n'a jamais été demandée.
            journey.wait_idle()
            before_restore = len(server_versions(url, token, observer, unit))
            journey.click("mainWindow/version_3/bouton_restaurer")
            # Le serveur ne réécrit personne : il AJOUTE une version qui reprend
            # le contenu choisi. C'est cette nouvelle tête que le poste reçoit.
            journey.until(
                "une nouvelle version créée par la restauration",
                lambda: len(server_versions(url, token, observer, unit)) > before_restore,
            )
            journey.click("mainWindow/nav_sync")
            journey.until(
                "la page Synchronisation visible", lambda: journey.visible("mainWindow/page_sync")
            )
            journey.click("mainWindow/bouton_synchroniser")
            journey.until(
                "le contenu de la branche perdante rétabli sur le disque",
                lambda: target.read_bytes() == "progression de l’autre appareil".encode(),
            )

            # PRO-01 : la version remplacée reste récupérable à côté.
            backups = sorted(saves.glob(f"{unit}.rsc-bak*/DATA.BIN"))
            assert backups, "aucune copie de sécurité avant remplacement (PRO-01)"
            assert backups[0].read_bytes() == b"progression du joueur, suite", backups[
                0
            ].read_bytes()
            # PRO-05 : rien n'a été supprimé au serveur — la restauration ajoute.
            final = server_versions(url, token, observer, unit)
            assert len(final) >= 4, final
            print(
                f"6. Restauration cliquée : le fichier du joueur porte la version choisie, "
                f"la précédente est conservée en copie ({backups[0].parent.name}), et le "
                f"serveur compte {len(final)} versions — aucune n'a disparu."
            )

            # ── 7. L'image de la sauvegarde, retirée à la souris ──────────
            # Le CHOIX passe par un sélecteur de fichiers natif, non pilotable —
            # on le simule par le canal, exactement comme pour le dossier. Le
            # RETRAIT, lui, est un bouton de la fenêtre : c'est celui-là qu'on
            # clique, et c'est lui qui prouve le câblage de l'interface.
            picture = workspace / "vignette.png"
            picture.write_bytes(base64.b64decode(TINY_PNG))
            journey.ask_agent({"method": "artwork", "unit_key": unit, "path": str(picture)})
            # Le bouton vit dans la page Historique : l'étape précédente nous a
            # ramenés sur Synchronisation pour recevoir la restauration.
            journey.click("mainWindow/nav_historique")
            journey.until(
                "la page Historique visible", lambda: journey.visible("mainWindow/page_historique")
            )
            journey.until(
                f"la ligne de l'unité {unit}", lambda: journey.visible(f"mainWindow/unite_{unit}")
            )
            journey.click(f"mainWindow/unite_{unit}")
            journey.until(
                "le bouton « Retirer » apparu dans la fenêtre",
                lambda: journey.visible("mainWindow/bouton_retirer_image"),
            )
            journey.click("mainWindow/bouton_retirer_image")
            # On vérifie d'abord le DISQUE — c'est lui qui fait foi — puis
            # l'écran, qui doit suivre.
            journey.until(
                "l'image retirée du disque par l'agent",
                lambda: (
                    not any(
                        entry["key"] == unit and entry.get("custom_art")
                        for entry in journey.ask_agent({"method": "status"})["units"]
                    )
                ),
            )
            journey.until(
                "le bouton « Retirer » disparu de la fenêtre",
                lambda: not journey.visible("mainWindow/bouton_retirer_image"),
            )
            # Et la synchronisation n'a pas bougé d'un pouce : l'image est
            # décorative, elle ne change ni version ni décision.
            assert len(server_versions(url, token, observer, unit)) == len(final), (
                "l'image a touché aux versions"
            )
            print(
                "7. Image : posée puis RETIRÉE d'un clic dans la fenêtre, sans toucher "
                "aux versions du serveur."
            )

            # ── 8. Pause d'une sauvegarde (SYN-06) ────────────────────────
            # Ce qu'il faut prouver n'est pas « le bouton change de texte »
            # mais « plus rien ne part, et rien n'est perdu ». On modifie donc
            # le fichier du joueur pendant la pause, et on vérifie le SERVEUR.
            def mode_of(key: str) -> str:
                for entry in journey.ask_agent({"method": "status"})["units"]:
                    if entry["key"] == key:
                        return entry.get("local_mode", "")
                return ""

            journey.click(f"mainWindow/pause_{unit}")
            journey.until(
                "la sauvegarde en pause dans le carnet de l'agent",
                lambda: mode_of(unit) == "paused",
            )
            before_pause = len(server_versions(url, token, observer, unit))
            write(saves / unit / "DATA.BIN", b"progression pendant la pause")
            journey.click("mainWindow/nav_sync")
            journey.until(
                "la page Synchronisation visible", lambda: journey.visible("mainWindow/page_sync")
            )
            journey.click("mainWindow/bouton_synchroniser")
            time.sleep(11)
            journey.click("mainWindow/bouton_synchroniser")
            journey.wait_idle()
            assert len(server_versions(url, token, observer, unit)) == before_pause, (
                "une sauvegarde en pause a été publiée"
            )
            # Et le fichier du joueur est intact : une pause ne recopie rien.
            assert target.read_bytes() == b"progression pendant la pause", target.read_bytes()

            # Reprise : la modification faite pendant la pause part normalement.
            journey.click("mainWindow/nav_historique")
            journey.until(
                "la page Bibliothèque visible",
                lambda: journey.visible("mainWindow/page_historique"),
            )
            journey.click(f"mainWindow/pause_{unit}")
            journey.until("la sauvegarde de nouveau suivie", lambda: mode_of(unit) == "sync")
            journey.click("mainWindow/nav_sync")
            journey.until(
                "la page Synchronisation visible", lambda: journey.visible("mainWindow/page_sync")
            )
            # Deux passes, comme pour toute publication : une unité en pause
            # n'est même pas observée, donc la reprise repart de zéro côté
            # stabilisation. C'est voulu — une pause ne doit rien coûter.
            journey.click("mainWindow/bouton_synchroniser")
            time.sleep(11)
            journey.click("mainWindow/bouton_synchroniser")
            journey.until(
                "la version faite pendant la pause enfin publiée",
                lambda: len(server_versions(url, token, observer, unit)) > before_pause,
            )
            print(
                "8. Pause : rien n'est publié pendant la pause, le fichier du joueur est "
                "intact, et la reprise publie la version qui attendait."
            )

            print(
                "Parcours graphique : RÉUSSI — connexion, dossier, synchronisation, "
                "conflit, résolution, historique, restauration, image, pause."
            )
        except BaseException:
            journey.rescue_screenshot()
            print("--- journal de la fenêtre (fin) ---", file=sys.stderr)
            print((workspace / "gui.log").read_text(encoding="utf-8")[-2000:], file=sys.stderr)
            raise
        finally:
            journey.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
