"""Vrai PPSSPP + deux agents : garde réception/envoi, avec saves synthétiques.

Aucun jeu n'est lancé. Ce banc ne prouve pas la relecture en jeu. Les services,
configurations et sauvegardes sont temporaires ; aucune ROM n'est consultée.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import time
from pathlib import Path

from test_agent_e2e import Agent
from test_api_interop import disposable_services


def sync(agent: Agent) -> dict:
    agent.wait_idle()
    response = agent.call({"method": "sync"})
    assert response["accepted"], response
    report = agent.wait_idle()["last"]
    assert report["errors"] == 0, report
    return report


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--agent", required=True, type=Path)
    parser.add_argument("--ppsspp", default="PPSSPPSDL")
    args = parser.parse_args()
    agents: list[Agent] = []
    emulator = None
    with disposable_services() as (url, token, _storage, workspace):
        workspace = Path(workspace)
        try:
            saves = []
            for name in ("a", "b"):
                root = workspace / name / "PSP" / "SAVEDATA"
                unit = root / "ULUS10041GAMEDATA"
                unit.mkdir(parents=True)
                save = unit / "DATA.BIN"
                save.write_bytes(b"synthetic baseline")
                saves.append(save)
                agent = Agent(args.agent.resolve(), workspace / name)
                agents.append(agent)
                agent.designate_root("ppsspp", root)
                agent.call(
                    {
                        "method": "connect",
                        "url": url,
                        "token": token,
                        "device_name": "process-guard-" + name,
                    }
                )
                deadline = time.monotonic() + 30
                while not agent.call({"method": "status"})["connected"]:
                    if time.monotonic() > deadline:
                        raise RuntimeError("connexion agent expirée")
                    time.sleep(0.2)
                sync(agent)
            time.sleep(11)
            sync(agents[0])
            sync(agents[1])
            saves[1].write_bytes(b"synthetic remote progress")
            sync(agents[1])
            time.sleep(11)
            assert sync(agents[1])["pushed"] == 1
            print("1. Deux agents PPSSPP synchronisés, puis tête distante avancée.", flush=True)

            env = dict(
                os.environ,
                XDG_CONFIG_HOME=str(workspace / "emulator-config"),
                XDG_DATA_HOME=str(workspace / "emulator-data"),
                XDG_CACHE_HOME=str(workspace / "emulator-cache"),
            )
            emulator = subprocess.Popen(
                [args.ppsspp, "--windowed", "--gamesettings"],
                env=env,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            deadline = time.monotonic() + 20
            while True:
                if emulator.poll() is not None:
                    raise RuntimeError("PPSSPP s'est arrêté au démarrage")
                clients = json.loads(subprocess.check_output(["hyprctl", "-j", "clients"]))
                if any(client.get("pid") == emulator.pid for client in clients):
                    break
                if time.monotonic() > deadline:
                    raise RuntimeError("fenêtre PPSSPP absente sous Hyprland")
                time.sleep(0.2)
            assert sync(agents[0])["pulled"] == 0
            assert saves[0].read_bytes() == b"synthetic baseline"
            saves[1].write_bytes(b"synthetic pending progress")
            sync(agents[1])
            time.sleep(11)
            assert sync(agents[1])["pushed"] == 0
            print(
                "2. Fenêtre PPSSPP réelle : réception et envoi bloqués, octets préservés.",
                flush=True,
            )
            emulator.terminate()
            emulator.wait(timeout=15)
            emulator = None
            assert sync(agents[0])["pulled"] == 1
            assert saves[0].read_bytes() == b"synthetic remote progress"
            backup = saves[0].parent.with_name(saves[0].parent.name + ".rsc-bak") / "DATA.BIN"
            assert backup.read_bytes() == b"synthetic baseline"
            assert sync(agents[1])["pushed"] == 1
            print(
                "3. PPSSPP fermé : réception avec backup, puis publication débloquées.", flush=True
            )
        finally:
            if emulator is not None:
                emulator.terminate()
                try:
                    emulator.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    emulator.kill()
                    emulator.wait()
            for agent in reversed(agents):
                agent.stop()


if __name__ == "__main__":
    main()
