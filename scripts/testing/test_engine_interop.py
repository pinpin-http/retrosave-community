"""Banc deux clients : deux moteurs contre un serveur réel jetable.

C'est la preuve que les tests unitaires ne peuvent pas donner. Le moteur a ses
propres tests contre un faux serveur en mémoire ; ils prouvent qu'il est
cohérent avec l'idée qu'il se fait du monde. Ici, **deux instances
indépendantes** se parlent à travers un vrai PostgreSQL, un vrai MinIO et la
vraie API, chacune avec son carnet, sa racine et son identité d'appareil, et
écrivent de vrais fichiers.

Ce que ce banc vérifie, dans l'ordre du parcours joueur :

1. le poste A publie une sauvegarde, le poste B la reçoit — octet pour octet ;
2. B la modifie, A reçoit la suite, avec copie de sécurité avant remplacement ;
3. les deux modifient hors ligne : **exactement un** conflit, aucune perte,
   les deux branches conservées ;
4. l'utilisateur tranche : les deux postes convergent sur la version choisie,
   et la branche perdante reste dans l'historique.

L'empreinte de contenu attendue est **recalculée ici**, en Python, d'après
§5.4 de l'architecture — et non empruntée au produit. Une preuve qui réutilise
le code qu'elle vérifie ne prouve rien.

Services jetables uniquement : ports loopback aléatoires, données en tmpfs,
détruits dans `finally`. Aucun volume Compose, aucune configuration
utilisateur, aucune sauvegarde réelle.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
import uuid
from pathlib import Path

import httpx

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts" / "testing"))

from test_api_interop import disposable_services


def register(url: str, token: str, name: str) -> str:
    """Enregistrer un appareil. Seule route qui n'exige pas déjà un appareil."""

    with httpx.Client(timeout=10, trust_env=False) as http:
        response = http.post(
            url + "/v0/devices",
            headers={"Authorization": f"Bearer {token}", "Idempotency-Key": uuid.uuid4().hex},
            json={"name": name, "os": "linux", "app_version": "banc"},
        )
        response.raise_for_status()
        return response.json()["device_id"]


def directory_content_sha256(files: dict[str, bytes]) -> str:
    """L'identité de contenu d'une unité-dossier, recalculée d'après §5.4.

    Le manifeste est trié sur les **octets UTF-8** du chemin — pas sur les
    caractères — puis haché. Une ligne par fichier :
    `relpath\\0taille\\0sha256\\n`.

    Réimplémenté ici volontairement : ce banc doit pouvoir contredire le
    produit, donc il ne peut pas emprunter son code.
    """

    manifest = bytearray()
    for rel_path in sorted(files, key=lambda path: path.encode("utf-8")):
        content = files[rel_path]
        manifest.extend(rel_path.encode("utf-8"))
        manifest.extend(b"\0")
        manifest.extend(str(len(content)).encode("ascii"))
        manifest.extend(b"\0")
        manifest.extend(hashlib.sha256(content).hexdigest().encode("ascii"))
        manifest.extend(b"\n")
    return hashlib.sha256(manifest).hexdigest()


class Engine:
    """Un poste. Un processus par passe : c'est ainsi qu'un vrai agent redémarre."""

    def __init__(
        self, binary: Path, workspace: Path, name: str, url: str, token: str, device: str
    ) -> None:
        self.binary = binary
        self.name = name
        self.root = workspace / f"{name}-saves"
        self.root.mkdir(parents=True, exist_ok=True)
        # Carnet et dossier de travail séparés : deux postes ne partagent
        # jamais leur état, même quand ils tournent sur la même machine.
        self.config = {
            "url": url,
            "token": token,
            "device": device,
            "root": str(self.root),
            "db": str(workspace / f"{name}-state.db"),
            "staging": str(workspace / f"{name}-staging"),
        }

    def sync(self, now: float, *, emulator_running: bool = False) -> dict:
        payload = dict(self.config, now=now, emulator_running=emulator_running)
        # Le jeton passe par stdin, jamais par argv : `ps` est lisible par tous.
        result = subprocess.run(
            [str(self.binary)],
            input=json.dumps(payload),
            capture_output=True,
            text=True,
            timeout=120,
            check=False,
        )
        if not result.stdout:
            raise RuntimeError(f"le moteur « {self.name} » n'a rien rendu")
        report = json.loads(result.stdout)
        if not report.get("ok"):
            raise RuntimeError(f"passe « {self.name} » en échec : {report.get('error')}")
        return report

    def settle(self, tick) -> dict:
        """Une passe qui observe, puis une qui publie.

        La stabilisation exige deux observations espacées : une seule passe ne
        capture jamais un contenu qui vient de bouger, et c'est voulu.
        """

        self.sync(tick(0))
        return self.sync(tick())


def write(path: Path, content: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content)


def snapshot(root: Path) -> dict[str, bytes]:
    """Le contenu réel sur le disque, copies de sécurité exclues."""

    files = {}
    for path in sorted(root.rglob("*")):
        if path.is_file() and not any(
            part.startswith(".rsc-") or ".rsc-bak" in part for part in path.relative_to(root).parts
        ):
            files[str(path.relative_to(root))] = path.read_bytes()
    return files


def units(url: str, token: str, device: str) -> list[dict]:
    with httpx.Client(timeout=10, trust_env=False) as http:
        response = http.get(
            url + "/v0/units",
            headers={"Authorization": f"Bearer {token}", "X-Device-Id": device},
        )
        response.raise_for_status()
        return response.json()["units"]


def conflicts(url: str, token: str, device: str) -> list[dict]:
    with httpx.Client(timeout=10, trust_env=False) as http:
        response = http.get(
            url + "/v0/conflicts?open=1",
            headers={"Authorization": f"Bearer {token}", "X-Device-Id": device},
        )
        response.raise_for_status()
        return response.json()["conflicts"]


def resolve(url: str, token: str, device: str, conflict_id: str, winner: int) -> None:
    with httpx.Client(timeout=10, trust_env=False) as http:
        response = http.post(
            f"{url}/v0/conflicts/{conflict_id}/resolve",
            headers={
                "Authorization": f"Bearer {token}",
                "X-Device-Id": device,
                "Idempotency-Key": uuid.uuid4().hex,
            },
            json={"winner": winner},
        )
        response.raise_for_status()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cpp", type=Path, required=True)
    arguments = parser.parse_args()
    if not arguments.cpp.is_file():
        raise SystemExit(f"outil moteur introuvable : {arguments.cpp}")

    with disposable_services() as (url, token, _storage, directory):
        workspace = Path(directory)
        # Deux appareils distincts du point de vue du serveur : c'est ce qui
        # rend le conflit possible.
        alpha = Engine(
            arguments.cpp, workspace, "alpha", url, token, register(url, token, "Poste A")
        )
        beta = Engine(arguments.cpp, workspace, "beta", url, token, register(url, token, "Poste B"))
        device = alpha.config["device"]
        clock = 1000.0

        def tick(step: float = 11.0) -> float:
            nonlocal clock
            clock += step
            return clock

        # ── 1. A publie, B reçoit ─────────────────────────────────────────
        write(alpha.root / "aventure" / "SAVE.BIN", b"chapitre 1")
        write(alpha.root / "aventure" / "meta" / "INDEX.BIN", b"index 1")
        report = alpha.settle(tick)
        assert report["pushed"] == 1, report
        assert report["errors"] == 0, report

        beta.sync(tick())
        assert snapshot(beta.root) == snapshot(alpha.root), "le poste B n'a pas reçu à l'identique"
        print("1. A → serveur → B : contenu identique octet pour octet.")

        # ── 2. B modifie, A reçoit avec copie de sécurité ──────────────────
        write(beta.root / "aventure" / "SAVE.BIN", b"chapitre 2")
        beta.settle(tick)
        report = alpha.sync(tick())
        assert report["pulled"] == 1, report
        assert (alpha.root / "aventure" / "SAVE.BIN").read_bytes() == b"chapitre 2"
        backup = alpha.root / "aventure.rsc-bak" / "SAVE.BIN"
        assert backup.read_bytes() == b"chapitre 1", "aucune copie de sécurité avant remplacement"
        # La copie de sécurité ne doit jamais devenir une unité de plus.
        assert len(units(url, token, device)) == 1, units(url, token, device)
        print("2. B → A : reçu, ancienne version conservée en .rsc-bak, aucune unité fantôme.")

        # ── 3. Divergence hors ligne : un seul conflit, rien de perdu ─────
        write(alpha.root / "aventure" / "SAVE.BIN", b"chapitre 3 cote A")
        write(beta.root / "aventure" / "SAVE.BIN", b"chapitre 3 cote B")
        report = alpha.settle(tick)
        assert report["pushed"] == 1, report

        beta.settle(tick)
        open_conflicts = conflicts(url, token, device)
        assert len(open_conflicts) == 1, open_conflicts
        # Les deux contenus existent encore localement : l'invariant I3 vaut
        # aussi sur le disque, pas seulement côté serveur.
        assert (alpha.root / "aventure" / "SAVE.BIN").read_bytes() == b"chapitre 3 cote A"
        assert (beta.root / "aventure" / "SAVE.BIN").read_bytes() == b"chapitre 3 cote B"
        # Insister ne doit pas fabriquer un second conflit.
        alpha.sync(tick())
        beta.sync(tick())
        assert len(conflicts(url, token, device)) == 1, "un second conflit a été ouvert"
        print("3. Divergence hors ligne : exactement un conflit, les deux côtés intacts.")

        # ── 4. L'utilisateur tranche : les deux convergent ────────────────
        conflict = open_conflicts[0]
        winner = conflict["version_a"]["number"]
        resolve(url, token, device, conflict["id"], winner)
        assert conflicts(url, token, device) == [], "le conflit est resté ouvert"

        for _ in range(2):
            alpha.sync(tick())
            beta.sync(tick())
        alpha_files = snapshot(alpha.root)
        beta_files = snapshot(beta.root)
        assert alpha_files == beta_files, (
            f"divergence après résolution :\n{alpha_files}\n{beta_files}"
        )

        history = httpx.get(
            f"{url}/v0/units/{units(url, token, device)[0]['id']}/versions",
            headers={"Authorization": f"Bearer {token}", "X-Device-Id": device},
            timeout=10,
        ).json()
        # Une résolution ne crée AUCUNE version : elle déplace la tête. Le
        # perdant reste donc une version de l'historique, consultable et
        # restaurable — c'est l'invariant I3, et c'est ce qui distingue un
        # arbitrage d'une suppression.
        assert history["head_version"] == winner, history
        branches = [v for v in history["versions"] if v["kind"] == "conflict_branch"]
        assert len(branches) == 1, history
        assert len(history["versions"]) == 4, history

        # Et le contenu local des deux postes est bien celui de la version
        # gagnante — pas seulement « le même des deux côtés », ce qui serait
        # aussi vrai s'ils avaient convergé vers une bouillie commune.
        expected = next(v for v in history["versions"] if v["number"] == winner)
        local = directory_content_sha256(
            {name.removeprefix("aventure/"): data for name, data in alpha_files.items()}
        )
        assert local == expected["content_sha256"], (local, expected)
        print(
            "4. Après résolution : les deux postes portent exactement la version "
            f"gagnante (v{winner}), et la branche perdante reste dans l'historique."
        )

        print("Banc deux clients : réussi. Services jetables détruits.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
