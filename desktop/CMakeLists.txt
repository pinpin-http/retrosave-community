cmake_minimum_required(VERSION 3.24)

# VERSION reste la source commune à Python, Android et au nouveau desktop.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" RETROSAVE_VERSION LIMIT_COUNT 1)
project(RetroSaveDesktop VERSION ${RETROSAVE_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Widgets n'est pas une nouvelle dépendance externe : c'est le module Qt qui
# porte QSystemTrayIcon et son menu, seule voie portable vers la zone de
# notification Windows/Linux. Aucune vue Widgets n'est utilisée par ailleurs.
# Sql : le pilote SQLite est fourni par Qt lui-même (module de qtbase), donc
# le carnet local n'ajoute AUCUNE dépendance externe.
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Network Quick QuickControls2 Widgets Sql)
# AUTOMOC génère la plomberie des signaux/propriétés Q_OBJECT. Ne jamais
# modifier les fichiers *_autogen : leur source est dans nos en-têtes C++.
qt_standard_project_setup(REQUIRES 6.8)

# Le canal local est commun à l'agent et à la fenêtre : une seule
# implémentation du verrou, des trames et des bornes, deux vocabulaires.
# Noyau de synchronisation : fonctions pures, partagées avec Python et Kotlin
# par les vecteurs de tests/vectors/. Il ne dépend QUE de Qt Core — ni réseau,
# ni interface, ni disque — pour rester comparable d'un langage à l'autre.
# zstd est la seule dépendance externe du noyau, et elle n'est pas négociable :
# l'invariant I5 impose des archives `tar.zst`, et le serveur comme Android
# stockent déjà ce format. Qt n'expose pas zstd publiquement.
#
# DEUX VOIES, dans cet ordre :
#
#   1. la bibliothèque DU SYSTÈME, via pkg-config. C'est la voie normale sous
#      Linux : la distribution la met à jour, et une compilation hors ligne
#      fonctionne. Un paquet distribué par une distribution ne doit jamais
#      embarquer sa propre copie d'une bibliothèque système.
#
#   2. sinon, zstd est COMPILÉ DEPUIS SES SOURCES. C'est la voie de Windows,
#      qui n'a pas de gestionnaire de paquets système : sans elle, le noyau de
#      synchronisation n'y était tout simplement pas construit — l'agent et la
#      fenêtre se lançaient et ne synchronisaient rien.
#
# L'archive est TÉLÉCHARGÉE À UNE VERSION FIGÉE et vérifiée par empreinte. Sans
# `URL_HASH`, la chaîne de compilation exécuterait ce que le réseau lui donne.
# L'empreinte ci-dessous a été comparée à celle publiée en amont
# (`zstd-1.5.7.tar.gz.sha256`).
option(RSC_ZSTD_FROM_SOURCE
    "Compiler zstd depuis ses sources même si le système en fournit une copie" OFF)

if(NOT RSC_ZSTD_FROM_SOURCE)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(ZSTD IMPORTED_TARGET libzstd)
    endif()
endif()

if(TARGET PkgConfig::ZSTD)
    set(RSC_ZSTD_TARGET PkgConfig::ZSTD)
    set(RSC_ZSTD_ORIGIN "système (pkg-config)")
else()
    include(FetchContent)
    # Options lues par le CMake de zstd. Uniquement la bibliothèque statique :
    # ni programmes, ni tests, ni formats hérités, et pas de DLL à déployer.
    set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(zstd
        URL https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
        URL_HASH SHA256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    # L'archive de zstd n'a PAS de CMakeLists.txt à sa racine : `MakeAvailable`
    # se contente donc de la déposer, et c'est nous qui ajoutons le bon
    # sous-dossier. `EXCLUDE_FROM_ALL` est ce qui empêche ses règles
    # d'installation (en-têtes, .pc, .lib) de se déverser dans notre arbre
    # livrable — nous voulons son code compilé, pas son paquet.
    FetchContent_MakeAvailable(zstd)
    add_subdirectory("${zstd_SOURCE_DIR}/build/cmake" "${zstd_BINARY_DIR}" EXCLUDE_FROM_ALL)
    set(RSC_ZSTD_TARGET libzstd_static)
    set(RSC_ZSTD_ORIGIN "sources v1.5.7, vérifiées par empreinte")
    # zstd est du C : ses avertissements ne sont pas les nôtres, et `-Wpedantic`
    # sur du code tiers ne rend service à personne.
    if(NOT MSVC)
        target_compile_options(libzstd_static PRIVATE -w)
    endif()
endif()

message(STATUS "RetroSave : zstd fourni par ${RSC_ZSTD_ORIGIN}")

add_library(retrosave_core STATIC
    src/core/identity.cpp src/core/identity.h
    src/core/archive.cpp src/core/archive.h
    src/core/decisions.cpp src/core/decisions.h
    src/core/localfs.cpp src/core/localfs.h
    src/core/gameart.cpp src/core/gameart.h
    src/state/store.cpp src/state/store.h
    src/engine/pass.cpp src/engine/pass.h src/engine/ports.h
    src/engine/folderscan.cpp src/engine/folderscan.h
    src/adapters/manifest.cpp src/adapters/manifest.h
    src/adapters/discovery.cpp src/adapters/discovery.h
    src/adapters/registry.cpp src/adapters/registry.h)
target_include_directories(retrosave_core PUBLIC src)
# `PRIVATE` : zstd ne remonte pas dans l'interface du noyau. Ce que cette
# variable désigne — bibliothèque système ou copie compilée — se décide plus
# haut, et le reste du fichier n'a pas à le savoir.
target_link_libraries(retrosave_core PUBLIC Qt6::Core Qt6::Sql PRIVATE ${RSC_ZSTD_TARGET})

# Adaptateur serveur distinct du noyau : aucune dépendance réseau ne remonte
# dans retrosave_core. Pas encore relié à l'agent tant que M-4 reste partiel.
add_library(retrosave_api STATIC src/network/v0client.cpp src/network/v0client.h
    src/network/s3transfer.cpp src/network/s3transfer.h
    src/network/v0catalog.cpp src/network/v0catalog.h src/network/v0json_p.h)
target_include_directories(retrosave_api PUBLIC src)
target_link_libraries(retrosave_api PUBLIC Qt6::Core Qt6::Network)

# Le pont entre le moteur et le serveur : il est le SEUL à connaître les deux.
# Le noyau ne gagne donc aucune dépendance réseau, et le client réseau ignore
# tout des décisions de synchronisation.
add_library(retrosave_sync STATIC src/engine/v0adapter.cpp src/engine/v0adapter.h)
target_link_libraries(retrosave_sync PUBLIC retrosave_core retrosave_api)

add_library(retrosave_ipc STATIC
    src/ipc/protocol.cpp src/ipc/protocol.h
    src/ipc/localservice.cpp src/ipc/localservice.h)
target_include_directories(retrosave_ipc PUBLIC src)
target_link_libraries(retrosave_ipc PUBLIC Qt6::Core Qt6::Network)
target_compile_definitions(retrosave_ipc PUBLIC RETROSAVE_VERSION="${PROJECT_VERSION}")

# Le cœur de l'agent : la configuration et le déclenchement des passes. En
# bibliothèque pour que les tests l'exercent sans lancer un processus.
qt_add_library(retrosave_agentcore STATIC
    src/agent/configuration.cpp src/agent/configuration.h
    src/agent/processwatch.cpp src/agent/processwatch.h
    src/agent/syncservice.cpp src/agent/syncservice.h)
target_link_libraries(retrosave_agentcore PUBLIC retrosave_sync retrosave_ipc)
# L'`os` transmis au serveur appartient à un vocabulaire fermé
# (android|windows|linux) : il est figé à la compilation, pas deviné.
target_compile_definitions(retrosave_agentcore PRIVATE
    RETROSAVE_PLATFORM="$<IF:$<BOOL:${WIN32}>,windows,linux>")

qt_add_executable(retrosave-agent src/agent/main.cpp)
target_link_libraries(retrosave-agent PRIVATE retrosave_agentcore)

# La bibliothèque de présentation est testable sans charger QML. Elle ne
# dépend que du protocole local, jamais des implémentations de l'agent.
# qt_add_library (et non add_library) extrait aussi les métadonnées du type
# AgentClient pour que qmllint connaisse ses propriétés via QML_FOREIGN.
qt_add_library(retrosave_client STATIC
    src/app/agentclient.cpp src/app/agentclient.h
    src/app/installation.cpp src/app/installation.h
    src/app/instanceguard.cpp src/app/instanceguard.h
    src/app/preferences.cpp src/app/preferences.h
    src/app/trayshell.cpp src/app/trayshell.h)
target_link_libraries(retrosave_client PUBLIC retrosave_ipc Qt6::Widgets)

qt_add_executable(retrosave-desktop src/app/main.cpp)
target_include_directories(retrosave-desktop PRIVATE src/app)
target_link_libraries(retrosave-desktop PRIVATE retrosave_client Qt6::Quick Qt6::QuickControls2)
set_target_properties(retrosave-desktop PROPERTIES WIN32_EXECUTABLE TRUE)
# L'alias retire le préfixe `qml/` du chemin de ressource. Il doit être posé
# sur CHAQUE fichier : sans lui, le `qmldir` généré désigne `qml/Theme.qml`
# alors que le module est monté à la racine, et le type reste introuvable —
# panne silencieuse au chargement.
set_source_files_properties(qml/Main.qml PROPERTIES QT_RESOURCE_ALIAS Main.qml)
set_source_files_properties(qml/StarMark.qml PROPERTIES QT_RESOURCE_ALIAS StarMark.qml)
set_source_files_properties(qml/NavItem.qml PROPERTIES QT_RESOURCE_ALIAS NavItem.qml)
set_source_files_properties(qml/AdapterRow.qml PROPERTIES QT_RESOURCE_ALIAS AdapterRow.qml)
set_source_files_properties(qml/Chip.qml PROPERTIES QT_RESOURCE_ALIAS Chip.qml)
set_source_files_properties(qml/Pill.qml PROPERTIES QT_RESOURCE_ALIAS Pill.qml)
set_source_files_properties(qml/EmulatorMark.qml PROPERTIES QT_RESOURCE_ALIAS EmulatorMark.qml)
set_source_files_properties(qml/SaveArtwork.qml PROPERTIES QT_RESOURCE_ALIAS SaveArtwork.qml)
set_source_files_properties(qml/UnitRow.qml PROPERTIES QT_RESOURCE_ALIAS UnitRow.qml)
set_source_files_properties(qml/VersionRow.qml PROPERTIES QT_RESOURCE_ALIAS VersionRow.qml)
# `Theme` est un singleton : une seule instance pour toute l'interface, créée
# par le moteur QML à la première utilisation. Sans cette propriété, le type
# serait instanciable et chaque écran aurait sa propre copie des couleurs.
set_source_files_properties(qml/Theme.qml PROPERTIES
    QT_RESOURCE_ALIAS Theme.qml QT_QML_SINGLETON_TYPE TRUE)
qt_add_qml_module(retrosave-desktop
    URI RetroSave.Desktop
    VERSION 1.0
    SOURCES src/app/qmlregistrations.h
    QML_FILES qml/Main.qml qml/Theme.qml qml/StarMark.qml qml/NavItem.qml qml/Chip.qml qml/Pill.qml qml/AdapterRow.qml qml/EmulatorMark.qml qml/SaveArtwork.qml qml/UnitRow.qml qml/VersionRow.qml
)

# ─── Canal de pilotage des tests d'interface ──────────────────────────────
# **Absent du livrable.** Cette option est OFF par défaut : le binaire distribué
# ne contient pas une ligne du canal, pas seulement un drapeau désactivé. Elle
# n'est activée que par la configuration de test dédiée (`scripts/testing/`).
#
# Spix et AnyRPC ne sont PAS de nouvelles dépendances du produit : ils
# n'existent que dans cette configuration, et `RSC_SPIX_PREFIX` doit désigner
# une installation locale — rien n'est téléchargé à la configuration.
option(RSC_WITH_TEST_DRIVER
    "Compiler le canal de pilotage des tests d'interface (jamais dans un livrable)" OFF)
set(RSC_SPIX_PREFIX "" CACHE PATH "Préfixe d'installation de Spix et AnyRPC")

if(RSC_WITH_TEST_DRIVER)
    if(RSC_SPIX_PREFIX)
        list(APPEND CMAKE_PREFIX_PATH "${RSC_SPIX_PREFIX}")
    endif()
    find_package(Spix REQUIRED)
    find_package(SpixQtQuick REQUIRED)
    find_package(AnyRPC REQUIRED)
    target_sources(retrosave-desktop PRIVATE
        src/testdriver/localtestserver.cpp src/testdriver/localtestserver.h)
    target_link_libraries(retrosave-desktop PRIVATE
        Spix::SpixCore Spix::SpixQtQuick AnyRPC::anyrpc)
    target_compile_definitions(retrosave-desktop PRIVATE RETROSAVE_TEST_DRIVER)
    message(WARNING
        "Canal de pilotage des tests COMPILÉ dans retrosave-desktop. "
        "Cette configuration ne doit jamais servir à produire un livrable.")
endif()

set(RSC_WARNED_TARGETS retrosave_ipc retrosave_client retrosave_api retrosave-agent retrosave-desktop)
list(APPEND RSC_WARNED_TARGETS retrosave_core retrosave_sync retrosave_agentcore)
foreach(target ${RSC_WARNED_TARGETS})
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 6.8 REQUIRED COMPONENTS Test)
    qt_add_executable(retrosave-s3-interop tests/s3_interop.cpp)
    target_link_libraries(retrosave-s3-interop PRIVATE retrosave_api)
    qt_add_executable(retrosave-api-tests tests/tst_v0client.cpp)
    target_link_libraries(retrosave-api-tests PRIVATE retrosave_api Qt6::Test)
    # La garde des identifiants d'émulateur est vérifiée contre les manifestes
    # RÉELLEMENT livrés, pas contre une liste recopiée dans le test.
    target_compile_definitions(retrosave-api-tests PRIVATE
        RETROSAVE_ADAPTERS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../adapters")
    add_test(NAME desktop-api COMMAND retrosave-api-tests)
    set_tests_properties(desktop-api PROPERTIES TIMEOUT 30)
    qt_add_executable(retrosave-ipc-tests tests/tst_ipc.cpp)
    target_link_libraries(retrosave-ipc-tests PRIVATE retrosave_client Qt6::Test)
    add_dependencies(retrosave-ipc-tests retrosave-agent retrosave-desktop)
    add_test(NAME desktop-ipc COMMAND retrosave-ipc-tests
        -o "${CMAKE_BINARY_DIR}/Testing/Temporary/desktop-ipc-qtest.txt,txt")
    set_tests_properties(desktop-ipc PROPERTIES TIMEOUT 45)
    # Les vecteurs vivent hors du composant : ils appartiennent aux trois
    # implémentations, pas au desktop.
    qt_add_executable(retrosave-api-interop tests/api_interop.cpp)
    target_link_libraries(retrosave-api-interop PRIVATE retrosave_api retrosave_core)
    # Une passe complète contre un vrai serveur : piloté par le harnais Python,
    # jamais exécuté seul par ctest — il lui faut PostgreSQL, MinIO et l'API.
    qt_add_executable(retrosave-engine-interop tests/engine_interop.cpp)
    target_link_libraries(retrosave-engine-interop PRIVATE retrosave_sync)
    qt_add_executable(retrosave-vector-tests tests/tst_vectors.cpp)
    target_link_libraries(retrosave-vector-tests PRIVATE retrosave_core Qt6::Test)
    target_compile_definitions(retrosave-vector-tests PRIVATE
        RETROSAVE_VECTORS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tests/vectors")
    add_test(NAME desktop-vectors COMMAND retrosave-vector-tests)
    set_tests_properties(desktop-vectors PROPERTIES TIMEOUT 30)

# Le rangement des images choisies par l'utilisateur : décoratif à l'écran, mais
# une collision ferait apparaître la vignette d'une sauvegarde sur une autre.
    qt_add_executable(retrosave-artwork-tests tests/tst_artwork.cpp)
    target_link_libraries(retrosave-artwork-tests PRIVATE retrosave_agentcore Qt6::Test)
    add_test(NAME desktop-artwork COMMAND retrosave-artwork-tests)
    set_tests_properties(desktop-artwork PROPERTIES TIMEOUT 30)

# La détection d'émulateur en cours protège la capture : elle a son propre banc
# plutôt que d'être supposée par les autres.
    qt_add_executable(retrosave-processwatch-tests tests/tst_processwatch.cpp)
    target_link_libraries(retrosave-processwatch-tests PRIVATE retrosave_agentcore Qt6::Test)
    add_test(NAME desktop-processwatch COMMAND retrosave-processwatch-tests)
    set_tests_properties(desktop-processwatch PROPERTIES TIMEOUT 30)
    qt_add_executable(retrosave-adapter-tests tests/tst_adapters.cpp)
    target_link_libraries(retrosave-adapter-tests PRIVATE retrosave_core Qt6::Test)
# Les manifestes et leurs fixtures vivent hors du composant : ils sont
# partagés avec Python, et c'est bien le but.
    target_compile_definitions(retrosave-adapter-tests PRIVATE
        RETROSAVE_ADAPTERS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../adapters")
    add_test(NAME desktop-adapters COMMAND retrosave-adapter-tests)
    set_tests_properties(desktop-adapters PROPERTIES TIMEOUT 30)

    qt_add_executable(retrosave-store-tests tests/tst_store.cpp)
    target_link_libraries(retrosave-store-tests PRIVATE retrosave_core Qt6::Test)
    add_test(NAME desktop-store COMMAND retrosave-store-tests)
    set_tests_properties(desktop-store PROPERTIES TIMEOUT 30)

# La passe assemble toutes les briques : c'est le seul test qui puisse
# prouver qu'une réception ne détruit pas la sauvegarde du joueur.
    qt_add_executable(retrosave-pass-tests tests/tst_pass.cpp)
    target_link_libraries(retrosave-pass-tests PRIVATE retrosave_core Qt6::Test)
    add_test(NAME desktop-pass COMMAND retrosave-pass-tests)
    set_tests_properties(desktop-pass PROPERTIES TIMEOUT 60)

# Le pont vers le vrai serveur, contre un VRAI serveur HTTP local.
    qt_add_executable(retrosave-adapter-net-tests tests/tst_v0adapter.cpp)
    target_link_libraries(retrosave-adapter-net-tests PRIVATE retrosave_sync Qt6::Test)
    add_test(NAME desktop-sync-adapter COMMAND retrosave-adapter-net-tests
        -o "${CMAKE_BINARY_DIR}/Testing/Temporary/desktop-sync-adapter-qtest.txt,txt")
    set_tests_properties(desktop-sync-adapter PROPERTIES TIMEOUT 60)

    qt_add_executable(retrosave-localfs-tests tests/tst_localfs.cpp)
    target_link_libraries(retrosave-localfs-tests PRIVATE retrosave_core Qt6::Test)
    add_test(NAME desktop-localfs COMMAND retrosave-localfs-tests)
    set_tests_properties(desktop-localfs PROPERTIES TIMEOUT 30)

    qt_add_executable(retrosave-archive-tests tests/tst_archive.cpp)
    target_link_libraries(retrosave-archive-tests PRIVATE retrosave_core Qt6::Test)
    add_test(NAME desktop-archive COMMAND retrosave-archive-tests)
    set_tests_properties(desktop-archive PROPERTIES TIMEOUT 30)

# Outil piloté par le harnais Python : il ne s'exécute pas seul en ctest,
# puisqu'il lui faut une archive produite par l'autre implémentation.
    qt_add_executable(retrosave-archive-interop tests/archive_interop.cpp)
    target_link_libraries(retrosave-archive-interop PRIVATE retrosave_core)
    target_compile_definitions(retrosave-archive-interop PRIVATE
        RETROSAVE_VECTORS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tests/vectors")

    # qt_add_executable crée par défaut des binaires sans console sous Windows.
    # Les exécutables Qt Test doivent garder stdout/stderr pour rendre leurs
    # assertions visibles dans CTest et les journaux CI.
    set_target_properties(
        retrosave-api-tests retrosave-ipc-tests retrosave-vector-tests
        retrosave-artwork-tests retrosave-processwatch-tests retrosave-adapter-tests
        retrosave-store-tests retrosave-pass-tests retrosave-adapter-net-tests
        retrosave-localfs-tests retrosave-archive-tests
        PROPERTIES WIN32_EXECUTABLE FALSE)

    add_test(NAME desktop-qml-smoke COMMAND retrosave-desktop --smoke-test)
    set_tests_properties(desktop-qml-smoke PROPERTIES
        TIMEOUT 15 ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
endif()

# Les manifestes d'émulateurs voyagent avec l'application : sans eux, aucun
# connecteur ne fonctionne chez l'utilisateur, et l'agent ne saurait scanner
# qu'un dossier générique.
# Les manifestes, et EUX SEULS. `install(DIRECTORY ... FILES_MATCHING)` recréait
# l'arborescence vide de `adapters/fixtures/` dans le livrable : aucun fichier,
# mais une dizaine de dossiers de test dans le paquet du joueur. On énumère donc
# les fichiers. `CONFIGURE_DEPENDS` fait relire le dossier à chaque construction,
# pour qu'un connecteur ajouté n'exige pas de reconfigurer à la main.
file(GLOB RSC_ADAPTER_MANIFESTS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/../adapters/*.toml")
install(FILES ${RSC_ADAPTER_MANIFESTS} DESTINATION share/retrosave/adapters)

# — Arborescence installée (V1-0C) —————————————————————————————
# Windows cherche ses DLL à côté de l'exécutable ; Linux range les
# bibliothèques sous lib/. On suit ces deux conventions plutôt que d'en
# inventer une troisième que les outils Qt ne connaîtraient pas.
if(WIN32)
    set(RSC_BIN_DIR ".")
else()
    set(RSC_BIN_DIR "bin")
endif()

# $ORIGIN rend l'arborescence déplaçable : elle fonctionne depuis n'importe
# quel dossier, sans LD_LIBRARY_PATH ni installation système.
set_target_properties(retrosave-agent retrosave-desktop PROPERTIES
    INSTALL_RPATH "$ORIGIN/../lib")

install(TARGETS retrosave-agent retrosave-desktop RUNTIME DESTINATION ${RSC_BIN_DIR})

# Une installation ordinaire ne copie QUE nos exécutables : un développeur
# qui compile avec le Qt de sa distribution ne doit pas se retrouver avec un
# arbre à moitié dupliqué. L'arbre autonome est un geste explicite.
option(RSC_DEPLOY_QT "Copier bibliothèques, plugins et modules QML de Qt dans l'installation" OFF)
# Même valeur par défaut que les scripts de déploiement de Qt : nos copies
# manuelles doivent atterrir au même endroit qu'eux.
set(QT_DEPLOY_PLUGINS_DIR "plugins" CACHE STRING "Sous-dossier des plugins Qt dans l'installation")

if(RSC_DEPLOY_QT)
    # Les Qt empaquetés par les distributions livrent des plugins sans entrée
    # RUNPATH ; l'outil de déploiement ne peut alors pas les réadresser et
    # l'installation casse en cours de route. On le dit à la configuration,
    # pas au milieu d'une copie de fichiers.
    if(QT6_INSTALL_PREFIX STREQUAL "/usr" OR QT6_INSTALL_PREFIX STREQUAL "/usr/local")
        message(FATAL_ERROR
            "RSC_DEPLOY_QT exige un Qt officiel (installateur Qt ou aqtinstall). "
            "Qt trouvé dans ${QT6_INSTALL_PREFIX}, qui est un Qt de distribution. "
            "Reconfigurer avec -DCMAKE_PREFIX_PATH=<chemin>/6.8.3/gcc_64.")
    endif()

    # Qt n'embarque que le plugin de plateforme retenu à la configuration.
    # « offscreen » est demandé sur les deux OS : c'est ce qui permet de
    # vérifier l'arbre RÉELLEMENT LIVRÉ sans écran, en CI comme sur un poste.
    set(RSC_EXTRA_PLUGINS Qt6::QOffscreenIntegrationPlugin)

    if(LINUX)
        # Sans plugin Wayland, l'arbre livré passerait par XWayland sur une
        # session Wayland — c'est-à-dire la majorité des bureaux visés.
        find_package(Qt6 6.8 QUIET COMPONENTS WaylandClient)
        if(Qt6WaylandClient_FOUND)
            list(APPEND RSC_EXTRA_PLUGINS
                Qt6::QWaylandIntegrationPlugin
                Qt6::QWaylandEglPlatformIntegrationPlugin
                Qt6::QWaylandXdgShellIntegrationPlugin)
        else()
            # Q38 n'a pas encore fixé les environnements couverts : on avertit
            # plutôt que d'échouer, mais on ne prétend pas gérer Wayland.
            message(WARNING
                "Qt WaylandClient introuvable : l'arbre déployé n'aura pas de "
                "plugin Wayland et passera par XWayland.")
        endif()
    endif()

    qt_import_plugins(retrosave-desktop INCLUDE ${RSC_EXTRA_PLUGINS})

    if(LINUX AND Qt6WaylandClient_FOUND)
        # Ces deux plugins échappent à qt_import_plugins, et sans le premier le
        # plugin Wayland refuse de créer la moindre fenêtre — vérifié : « No
        # shell integration named xdg-shell found », puis abandon. Leur RUNPATH
        # vaut déjà "$ORIGIN/../../lib", ce qui correspond exactement à
        # l'arborescence installée : une copie suffit, sans réadressage.
        # On ne prend que le côté client : les variantes « server » servent à
        # écrire un compositeur, pas à en utiliser un.
        install(FILES
            "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_PLUGINS}/wayland-shell-integration/libxdg-shell.so"
            DESTINATION "${QT_DEPLOY_PLUGINS_DIR}/wayland-shell-integration")
        install(FILES
            "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_PLUGINS}/wayland-graphics-integration-client/libqt-plugin-wayland-egl.so"
            DESTINATION "${QT_DEPLOY_PLUGINS_DIR}/wayland-graphics-integration-client")
    endif()

    # Deux scripts de déploiement : l'interface entraîne Qt Quick, Widgets et
    # ses modules QML ; l'agent est vérifié séparément pour qu'il reste
    # lançable même si l'interface cessait un jour de dépendre de Qt Network.
    # NO_TRANSLATIONS : les traductions de Qt couvrent ses propres dialogues,
    # dans une quarantaine de langues. Depuis le 13/09/2026 la langue SOURCE de
    # l'interface est l'anglais — convention Qt, et la seule qui tienne pour un
    # projet open source. Les `qsTr()` sont restés en place : une traduction
    # française se pose dans `translations/` sans toucher au code. Tant qu'il
    # n'y en a aucune, embarquer celles de Qt donnerait une fenêtre à moitié
    # traduite selon le dialogue. Les deux arriveront ensemble.
    # Les scripts Qt sont exécutés à l'installation, dans un processus CMake
    # distinct. Leur destination doit donc être fixée juste avant leur `include`
    # plutôt qu'avec une variable de configuration perdue entre les deux phases.
    # Sous Windows, le chargeur ne cherche les DLL qu'à côté de l'exécutable.
    if(WIN32)
        install(CODE "set(QT_DEPLOY_BIN_DIR \".\")\nset(QT_DEPLOY_LIB_DIR \".\")")
        # QtCore charge ICU au démarrage avec les distributions officielles de
        # Qt. windeployqt ne l'ajoute pas toujours à un arbre CMake ; sans ces
        # DLL l'agent échoue avant même de pouvoir afficher son diagnostic.
        file(GLOB RSC_ICU_DLLS "${QT6_INSTALL_PREFIX}/bin/icu*.dll")
        install(FILES ${RSC_ICU_DLLS} DESTINATION ".")
    endif()

    qt_generate_deploy_qml_app_script(
        TARGET retrosave-desktop
        OUTPUT_SCRIPT RSC_DEPLOY_DESKTOP
        NO_UNSUPPORTED_PLATFORM_ERROR
        NO_TRANSLATIONS
    )
    install(SCRIPT ${RSC_DEPLOY_DESKTOP})

    # L'agent n'affiche rien : aucune traduction ne le concerne.
    qt_generate_deploy_app_script(
        TARGET retrosave-agent
        OUTPUT_SCRIPT RSC_DEPLOY_AGENT
        NO_UNSUPPORTED_PLATFORM_ERROR
        NO_TRANSLATIONS
    )
    install(SCRIPT ${RSC_DEPLOY_AGENT})
endif()
