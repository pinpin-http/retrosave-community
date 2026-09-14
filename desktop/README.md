# Desktop Qt

Cette application est le nouveau desktop Windows/Linux. **Elle synchronise** :
on relie un serveur, on désigne un dossier de sauvegardes, et l'agent publie et
reçoit les parties — avec copie de sécurité avant tout remplacement, conflits
conservés des deux côtés, et reprise après interruption.

La CI construit le même moteur C++ sous Linux et Windows. Les releases
Community produisent une archive Linux, une archive portable Windows et un
installateur Windows avec Qt et zstd.

- [Prise en main Qt et parcours du code](../docs/guides/QT_DESKTOP.md)
- [Identité visuelle partagée avec Android](../docs/guides/IDENTITE_VISUELLE.md)
- [Ajouter un émulateur — un fichier TOML suffit](../docs/guides/AJOUTER_UN_EMULATEUR.md)
- [Architecture générale et limites](../ARCHITECTURE.md)

Le composant contient aussi le moteur de synchronisation : identité, archive,
décisions, découverte, écritures locales protégées, carnet SQLite, **la passe
qui assemble tout cela** et son adaptateur vers le serveur (`retrosave_sync`,
au-dessus de `retrosave_api` : contrat `/v0` et transport S3 en flux). Il
est relié à l'agent et testé depuis l'interface contre PostgreSQL, un stockage
S3 jetable et le vrai serveur.

## Construire

Prérequis : CMake ≥ 3.24, Ninja, compilateur C++20, Qt ≥ 6.8 avec Core,
Network, Quick, QuickControls2, Widgets et Test. La CI et les paquets ciblent
Qt 6.8.3.

**zstd**, qu'exige le format d'archive (invariant I5), est trouvé de deux
façons, dans cet ordre :

1. **la bibliothèque du système**, via pkg-config — `apt install libzstd-dev`,
   `pacman -S zstd`. C'est la voie normale sous Linux : la distribution la met à
   jour, et la compilation fonctionne hors ligne ;
2. **compilée depuis ses sources** si le système n'en fournit pas. C'est la voie
   de Windows, qui n'a pas de gestionnaire de paquets système. L'archive est
   figée à la version 1.5.7 et vérifiée par empreinte SHA-256, comparée à celle
   publiée en amont.

`-DRSC_ZSTD_FROM_SOURCE=ON` force la seconde voie — c'est ainsi qu'on éprouve le
chemin de Windows depuis Linux. Pour une compilation **hors ligne** par cette
voie, déposer les sources et passer
`-DFETCHCONTENT_SOURCE_DIR_ZSTD=/chemin/vers/zstd-1.5.7`.

Le noyau de synchronisation est construit **sur toutes les plateformes**. Il ne
l'était pas sous Windows jusqu'au 8 septembre 2026 : l'agent et la fenêtre s'y
lançaient sans moteur, et ne synchronisaient rien.

Depuis `desktop/`, dans un terminal configuré pour votre compilateur et Qt :

```sh
cmake --preset dev
cmake --build --preset dev --parallel 2
ctest --preset dev
cmake --build build/dev --target all_qmllint
```

Si Qt n'est pas trouvé : `cmake --preset dev -DCMAKE_PREFIX_PATH=/chemin/vers/Qt/6.8.3/gcc_64`.
Sur Windows, utiliser le terminal développeur x64 de Visual Studio 2022 et le
kit Qt MSVC 2022 x64 ; remplacer le préfixe par le dossier `msvc2022_64`.
Les compilateurs et bibliothèques Qt doivent utiliser le même kit.

## Exécuter et arrêter

Depuis `desktop/` après un build Ninja :

```sh
./build/dev/bin/retrosave-desktop
```

Sur Windows : `./build/dev/bin/retrosave-desktop.exe`.
Le bouton **Démarrer l'agent** lance l'exécutable voisin en processus détaché.
Fermer la fenêtre ne l'arrête pas. Le PID affiché provient de sa réponse réelle.

Le bouton **Arrêter l'agent**, ou l'entrée correspondante du menu de la zone de
notification, demande son arrêt par le canal local : l'agent range son socket et
rend son verrou avant de quitter. `kill <PID>` sous Linux, ou terminer **ce PID**
dans le gestionnaire des tâches Windows, restent valables. Ne pas utiliser un
arrêt global par nom : un développeur peut faire tourner des agents de test
distincts.

Fermer la fenêtre la réduit dans la zone de notification quand le bureau en
expose une ; **Quitter l'interface** dans le menu de l'icône ferme réellement la
fenêtre, sans toucher à l'agent. Si le bureau n'a pas de zone de notification,
aucune icône n'est créée et la fermeture ferme l'interface. Un bureau peut
déclarer une zone sans afficher l'icône : dans ce cas, relancer
`retrosave-desktop` ramène la fenêtre existante au premier plan — un second
lancement ne crée jamais une deuxième fenêtre.

Pour observer ses messages et l'arrêter simplement avec Ctrl+C, lancer plutôt
l'agent en premier plan dans un terminal, puis l'interface dans un autre :

```sh
./build/dev/bin/retrosave-agent
./build/dev/bin/retrosave-desktop
```

`--socket <nom-unique>` est accepté par les deux exécutables pour isoler un test.
Ne pas utiliser deux noms de socket comme mécanisme multi-compte : le contrat
de ce prototype n'est qu'un diagnostic sans données métier.

Le test graphique sans écran ni agent se lance ainsi sous Linux :

```sh
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software ./build/dev/bin/retrosave-desktop --smoke-test
```

## Découpage

| Chemin | Responsabilité |
|---|---|
| `src/core/identity.*` | Noyau : nom normalisé, empreinte rapide, identité de contenu. Fonctions pures, partagées avec Python et Kotlin par les vecteurs |
| `src/core/archive.*` | Noyau : `tar` déterministe et trame `zstd`. Seule partie du composant à dépendre d'une bibliothèque externe (libzstd) |
| `src/core/decisions.*` | Noyau : règles de décision pures — stabilisation, conflits, reprise, gardes d'écriture |
| `src/core/localfs.*` | **Le seul code qui touche aux sauvegardes réelles** : copie de sécurité, écriture atomique relue, remplacement d'unité |
| `src/state/store.*` | Carnet local SQLite : unités connues, observations, `last_synced`, conflits, journal d'application |
| `src/engine/ports.h` | Ce que la passe attend d'un serveur. Une interface, pour que la passe se teste sans serveur |
| `src/engine/pass.*` | **La passe** : `scan → reprise → PULL → PUSH → rapport`. Elle ne calcule rien elle-même, elle enchaîne les briques dans l'ordre du §9.3 |
| `src/engine/v0adapter.*` | Le pont vers le vrai serveur : le port ci-dessus, réalisé au-dessus des clients HTTP et S3 |
| `src/adapters/manifest.*` | Lecture stricte des manifestes `adapters/*.toml` ; refuse tout ce qu'elle ne comprend pas |
| `src/adapters/discovery.*` | Moteur de découverte générique piloté par le manifeste : **un seul**, pour tous les émulateurs |
| `src/adapters/registry.*` | Charge les manifestes et sert un adaptateur par identifiant ; point d'extension pour un cas hors vocabulaire |
| `src/core/gameart.*` | Noyau : teinte et initiales d'une vignette, validation d'en-tête PNG. Mêmes vecteurs que Python et Kotlin |
| `src/ipc/protocol.*` | Version, format JSON, vocabulaires des deux canaux et identité des adresses |
| `src/ipc/localservice.*` | Canal local générique : verrou d'instance, trames et bornes, sans vocabulaire |
| `src/agent/configuration.*` | Ce que l'agent doit savoir : serveur, jeton, dossier. Le jeton est rangé par lui, dans un fichier en 0600 |
| `src/agent/syncservice.*` | Déclenche les passes dans un **fil de travail** : le canal local ne fait jamais attendre l'interface |
| `src/agent/main.cpp` | Assemble le canal, le vocabulaire et le moteur ; aucune interface |
| `src/app/agentclient.*` | Client asynchrone et propriétés présentées à QML |
| `src/app/instanceguard.*` | Canal de la fenêtre : un second lancement rappelle celle qui existe |
| `src/app/trayshell.*` | Zone de notification, son menu, son icône peinte et son absence |
| `src/app/preferences.*` | Démarrage avec la session et dossier désigné, relus depuis le système |
| `src/app/installation.*` | Chemin de l'agent voisin et emplacement des réglages |
| `src/app/main.cpp` | Assemble ces objets et le moteur QML, contrôle périodique local |
| `src/app/qmlregistrations.h` | Déclare les types C++ aux outils QML sans déplacer leur création dans l'UI |
| `qml/Theme.qml` | Singleton des couleurs, matière, rayons, tailles et **durées d'animation** — miroir de `design/tokens.json`, partagé avec Android |
| `qml/StarMark.qml` | La marque : une étoile à sept branches, dessinée, même géométrie que sur Android |
| `qml/AuroraBackdrop.qml` | Le fond : dégradé, remontée verte qui respire, bords éteints, grille fine, éclats |
| `qml/Main.qml` | Présentation déclarative ; aucune logique de sauvegarde, aucune couleur en dur |
| `tests/tst_ipc.cpp` | Tests avec de vrais processus, erreurs, reprise après crash et réglages isolés |
| `tests/tst_vectors.cpp` | Rejoue `tests/vectors/*.json`, les mêmes fichiers que Python et Kotlin |
| `tests/tst_archive.cpp` | Déterminisme, structure tar, et refus des archives corrompues ou truquées |
| `tests/tst_adapters.cpp` | Découverte sur les **vrais** manifestes et les **vraies** fixtures du dépôt |
| `tests/tst_localfs.cpp` | Disque menteur, rotation des copies, liens symboliques : ce qui ne doit jamais casser |
| `tests/tst_store.cpp` | Ce qu'un scan n'a pas le droit d'oublier, et l'horloge de stabilité |
| `tests/tst_pass.cpp` | La passe entière contre un faux serveur et de **vrais** fichiers : conflit, archive corrompue, écriture interrompue, disparition massive |
| `tests/tst_v0adapter.cpp` | La traduction vers le contrat serveur, contre un vrai serveur HTTP local |
| `tests/engine_interop.cpp` | Une passe complète contre un vrai serveur, pilotée par `scripts/testing/test_engine_interop.py` |
| `src/network/v0client.*` | Routes API et transport Qt, credentials, délais et annulation |
| `src/network/v0catalog.*` | Données typées et codecs des listes/appareils/unités/historique |
| `src/network/v0json_p.h` | Validations privées partagées entre codecs HTTP |
| `src/network/s3transfer.*` | PUT/GET asynchrones, staging temporaire et intégrité |
| `tests/s3_interop.cpp` | Outil piloté par le banc Docker `scripts/testing/test_s3_interop.py` |
| `tests/archive_interop.cpp` | Outil piloté par `scripts/testing/test_archive_interop.py` pour la lecture croisée |

## Produire l'arbre livrable

`cmake --install build/dev --prefix <destination>` installe **nos exécutables
seuls**. Pour l'arbre autonome, il faut un **Qt officiel** (installateur Qt ou
`aqtinstall`) : les Qt empaquetés par les distributions livrent des plugins que
l'outil de déploiement ne peut pas réadresser, et CMake le refuse en le disant.

```sh
cmake -S desktop -B desktop/build/pkg -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DRSC_DEPLOY_QT=ON \
  -DCMAKE_PREFIX_PATH=<chemin>/6.8.3/gcc_64
cmake --build desktop/build/pkg --parallel 2
cmake --install desktop/build/pkg --prefix /tmp/rsc-tree
```

Le contrôle qui compte est de **déplacer** l'arbre avant de le lancer : c'est ce
qui prouve qu'il ne dépend ni de son chemin de construction ni du Qt de la
machine.

```sh
mv /tmp/rsc-tree /tmp/rsc-ailleurs
env -i HOME=$HOME QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
  /tmp/rsc-ailleurs/bin/retrosave-desktop --smoke-test
```

La CI transforme cet arbre en archive portable Linux/Windows et en installateur
Windows. La mise à jour automatique n'est pas encore fournie.

Les réglages vivent dans `retrosave-community/retrosave-community.conf` sous la
configuration utilisateur ; les verrous des canaux sous
`retrosave-community/ipc` dans les données utilisateur. Le démarrage de session
écrit une unité systemd user sous Linux, une tâche planifiée sous Windows —
jamais une clé de registre `Run`. Les tests isolent ces emplacements par
`XDG_CONFIG_HOME` et `XDG_DATA_HOME` ; la tâche Windows relève de la recette
manuelle, faute de pouvoir l'isoler du poste.
