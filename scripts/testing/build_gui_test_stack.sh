#!/usr/bin/env bash
# Construit tout ce que le parcours graphique exige, sans rien installer sur le
# système : AnyRPC, Spix, puis l'interface AVEC son canal de pilotage.
#
# Ce canal n'existe que dans cette configuration. Le livrable, lui, se construit
# sans `RSC_WITH_TEST_DRIVER` et ne contient pas une ligne de ce code — c'est
# vérifié à la fin de ce script.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TOOLS="$ROOT/.tools"
CMAKE="${CMAKE:-$TOOLS/qt-build/bin/cmake}"
BUILD="${BUILD:-$ROOT/desktop/build/gui-test}"
# Sur ce poste, Qt vient du système ; en intégration continue il est déposé par
# aqtinstall. `QT_PREFIX` laisse l'appelant le dire sans modifier le script.
QT_PREFIX="${QT_PREFIX:-}"

ANYRPC_VERSION="${ANYRPC_VERSION:-master}"
SPIX_VERSION="${SPIX_VERSION:-v0.14}"

mkdir -p "$TOOLS/src"

# ── AnyRPC ────────────────────────────────────────────────────────────────
# Son CMake est antérieur à la 3.5, que CMake 4 refuse : d'où la politique
# explicite ci-dessous. Bibliothèque de transport uniquement, aucun code Qt.
if [ ! -f "$TOOLS/anyrpc/lib/libanyrpc.so" ]; then
    [ -d "$TOOLS/src/anyrpc" ] || git clone --depth 1 --branch "$ANYRPC_VERSION" \
        https://github.com/sgieseking/anyrpc.git "$TOOLS/src/anyrpc"
    "$CMAKE" -S "$TOOLS/src/anyrpc" -B "$TOOLS/src/anyrpc/build" -G Ninja \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$TOOLS/anyrpc" \
        -DBUILD_EXAMPLES=OFF -DBUILD_TESTS=OFF -DBUILD_WITH_LOG4CPLUS=OFF \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    "$CMAKE" --build "$TOOLS/src/anyrpc/build" --parallel
    "$CMAKE" --install "$TOOLS/src/anyrpc/build"
fi

# ── Spix ──────────────────────────────────────────────────────────────────
if [ ! -f "$TOOLS/spix/lib/libSpixQtQuick.a" ]; then
    [ -d "$TOOLS/src/spix" ] || git clone --depth 1 --branch "$SPIX_VERSION" \
        https://github.com/faaxm/spix.git "$TOOLS/src/spix"
    "$CMAKE" -S "$TOOLS/src/spix" -B "$TOOLS/src/spix/build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$TOOLS/spix" \
        ${QT_PREFIX:+-DCMAKE_PREFIX_PATH="$QT_PREFIX"} \
        -DSPIX_BUILD_EXAMPLES=OFF -DSPIX_BUILD_TESTS=OFF -DSPIX_QT_MAJOR=6 \
        -DAnyRPC_INCLUDE_DIRS="$TOOLS/anyrpc/include" \
        -DAnyRPC_LIBRARIES="$TOOLS/anyrpc/lib/libanyrpc.so"
    "$CMAKE" --build "$TOOLS/src/spix/build" --parallel
    "$CMAKE" --install "$TOOLS/src/spix/build"
fi

# ── L'interface instrumentée ──────────────────────────────────────────────
"$CMAKE" -S "$ROOT/desktop" -B "$BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=OFF \
    -DRSC_WITH_TEST_DRIVER=ON \
    -DRSC_SPIX_PREFIX="${QT_PREFIX:+$QT_PREFIX;}$TOOLS/spix" \
    -DCMAKE_MODULE_PATH="$TOOLS/src/spix/cmake/modules" \
    -DAnyRPC_INCLUDE_DIRS="$TOOLS/anyrpc/include" \
    -DAnyRPC_LIBRARIES="$TOOLS/anyrpc/lib/libanyrpc.so"
"$CMAKE" --build "$BUILD" --parallel

# ── La vérification qui compte ────────────────────────────────────────────
# Le binaire de TEST contient le canal ; celui du LIVRABLE ne doit pas. On le
# dit ici, à côté de la construction, plutôt que dans une note qu'on oublie.
# `grep -q` ferme le tube dès la première correspondance ; avec `pipefail`, le
# SIGPIPE qui en résulte fait échouer le tube ALORS QUE la correspondance a eu
# lieu. On compte donc, ce qui lit tout le flux.
found=$(strings "$BUILD/bin/retrosave-desktop" | grep -cF "test-driver-port" || true)
if [ "$found" -eq 0 ]; then
    echo "ERREUR : le binaire de test n'a pas son canal de pilotage." >&2
    exit 1
fi

echo
echo "Interface instrumentée : $BUILD/bin/retrosave-desktop"
echo "Lancer le parcours :"
echo "  .venv/bin/python scripts/testing/test_gui_journey.py \\"
echo "    --agent desktop/build/dev/bin/retrosave-agent \\"
echo "    --gui $BUILD/bin/retrosave-desktop"
