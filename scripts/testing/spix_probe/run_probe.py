import os
import subprocess
import sys
import time
import xmlrpc.client

plat = sys.argv[1]
env = dict(os.environ, QT_QPA_PLATFORM=plat)
if plat != "wayland":
    env["QT_QUICK_BACKEND"] = "software"
p = subprocess.Popen(
    ["/tmp/spix-probe/build/spixprobe"],
    env=env,
    stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL,
)
try:
    rpc = xmlrpc.client.ServerProxy("http://127.0.0.1:9111", allow_none=True)
    for _ in range(60):
        try:
            rpc.waitForItem("mainWindow/resultat", 500)
            break
        except (OSError, xmlrpc.client.Error):
            time.sleep(0.2)
    time.sleep(0.8)
    print(f"[{plat}]")
    for cible in (
        "cible_taphandler",
        "cible_tap_release",
        "cible_tap_within",
        "cible_mousearea",
        "cible_button",
    ):
        rpc.setStringProperty("mainWindow", "touched", "—")
        rpc.mouseClick(f"mainWindow/{cible}")
        time.sleep(0.5)
        got = rpc.getStringProperty("mainWindow/resultat", "text")
        verdict = "RÉPOND" if got != "—" else "muet"
        print(f"  {cible:22} {verdict:7} ({got!r})")
    rpc.quit()
finally:
    try:
        p.wait(timeout=8)
    except subprocess.TimeoutExpired:
        p.kill()
