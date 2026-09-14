# Contribuer à RetroSave

Merci de regarder. Ce projet manipule des fichiers irremplaçables — cent heures
de jeu tiennent parfois dans un `.sav` de 512 Ko. Les règles ci-dessous
viennent toutes de là.

## Les trois règles non négociables

Elles priment sur le style, la performance et l'élégance.

**1. Ne jamais détruire.** Aucune écriture ne remplace un fichier sans copie de
sécurité préalable, et aucune écriture n'est visible à moitié : on écrit à côté,
on relit ce que le disque rend, puis on publie d'un seul geste. Un conflit
conserve **les deux** versions ; il n'existe aucune fusion automatique, et
« le plus récent gagne » est interdit.

**2. Un vecteur avant toute logique de noyau.** Les décisions de synchronisation
existent en double — C++ pour le desktop, Kotlin pour Android — et les deux
rejouent les **mêmes** fichiers de `tests/vectors/`. Si vous changez une règle,
le vecteur partagé vient d'abord, sinon les deux implémentations divergeront
sans que personne ne le voie.

**3. Un test avant tout correctif.** Un bogue corrigé sans test qui le reproduit
reviendra. Écrivez le test, voyez-le échouer, puis corrigez.

Une quatrième, propre au domaine : **jamais de ROM**. RetroSave n'ouvre, ne
hache et ne transfère aucun fichier de jeu. Si votre changement fait lire quoi
que ce soit hors d'un dossier de sauvegarde, il est refusé.

## Sur quelle branche travailler

**`main`.** C'est la branche open source, et c'est là que va tout le code du
produit : moteur, serveur, connecteurs, applications desktop et Android.

Créer une branche courte depuis `main`, puis ouvrir une pull request. Les tags
et les paquets sont produits uniquement par le workflow de release. La règle
complète se trouve dans [docs/BRANCHES.md](docs/BRANCHES.md).

## Construire et vérifier

```sh
# Environnement (une fois) — rien n'est installé sur le système
uv sync --all-packages

# Le desktop
cmake -S desktop -B desktop/build/dev -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build desktop/build/dev --parallel 4
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
  ctest --test-dir desktop/build/dev --output-on-failure
cmake --build desktop/build/dev --target all_qmllint

# Le reste
python -m pytest server/tests tests -q
python scripts/testing/check_docs.py     # aucun lien mort dans la doc
ruff check .
```

Les **bancs** demandent Docker. Ils montent PostgreSQL, MinIO et l'API en
conteneurs jetables, et les détruisent ensuite :

```sh
python scripts/testing/test_engine_interop.py --cpp desktop/build/dev/bin/retrosave-engine-interop
python scripts/testing/test_agent_e2e.py \
  --agent desktop/build/dev/bin/retrosave-agent \
  --engine desktop/build/dev/bin/retrosave-engine-interop
python scripts/testing/test_adapters_e2e.py \
  --agent desktop/build/dev/bin/retrosave-agent
```

Ce sont eux qui comptent : deux clients qui convergent à travers un vrai
serveur, l'agent réel piloté comme le fait l'interface, et les neuf connecteurs
menés jusqu'à une version serveur.

## Android — la chaîne d'outils, et le piège du JDK

Ce que lance la CI, et donc ce qu'il faut voir vert avant de proposer quoi que
ce soit :

```sh
android/gradlew -p android ktlintCheck testDebugUnitTest assembleDebug
```

Le projet cible **JDK 17** (`JavaVersion.VERSION_17`). Vérifiez `java -version`
et `JAVA_HOME` avant de lancer Gradle : le JDK fourni par une version récente
d'Android Studio peut être trop récent pour Gradle 8.13.

Installation complète sur un poste vierge, sans rien mettre dans le système à
part le JDK :

```sh
sudo pacman -S jdk17-openjdk            # ou l'équivalent de votre distribution
export JAVA_HOME=/usr/lib/jvm/java-17-openjdk

# Le SDK vit dans le home : aucun droit d'administration nécessaire.
mkdir -p ~/Android/Sdk/cmdline-tools
curl -sSLo /tmp/cmdline-tools.zip \
  https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip
unzip -q /tmp/cmdline-tools.zip -d /tmp
mv /tmp/cmdline-tools ~/Android/Sdk/cmdline-tools/latest
export ANDROID_HOME=~/Android/Sdk

yes | $ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager --licenses
$ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager \
  platform-tools "platforms;android-35" "build-tools;35.0.0"

echo "sdk.dir=$HOME/Android/Sdk" > android/local.properties
```

`android/local.properties` n'est pas versionné : il désigne un chemin propre à
votre poste.

Pour **voir** l'application, ajoutez l'émulateur (~2 Go de plus) :

```sh
$ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager \
  emulator "system-images;android-35;default;x86_64"
$ANDROID_HOME/cmdline-tools/latest/bin/avdmanager create avd \
  -n retrosave-test -k "system-images;android-35;default;x86_64" -d pixel_6
$ANDROID_HOME/emulator/emulator -avd retrosave-test -no-snapshot -no-boot-anim &
adb install -r android/app/build/outputs/apk/debug/app-debug.apk
```

Depuis l'émulateur, le serveur de votre poste se joint à `http://10.0.2.2:8000`
et non `localhost` — `localhost` y désigne l'appareil virtuel lui-même.

## Ajouter un émulateur

C'est la contribution la plus utile, et elle ne demande pas d'écrire de code :
un fichier TOML dans `adapters/`, une fixture, un test. Tout est expliqué dans
[le guide](docs/guides/AJOUTER_UN_EMULATEUR.md).

Un connecteur qui ramasse **trop** est plus dangereux qu'un connecteur qui
ramasse trop peu : il ferait remonter des fichiers que personne n'a demandé de
synchroniser. Testez ce que le vôtre doit ignorer.

## Style

- Le code et les commentaires sont **en français**, comme le reste du dépôt.
  Les commentaires expliquent *pourquoi*, pas *quoi*.
- C++ : `clang-format` (`.clang-format` à la racine de `desktop/`).
- Python : `ruff check` et `ruff format`.
- Commits : `type(portée): sujet` — `feat`, `fix`, `refactor`, `docs`, `test`.

## Avant d'ouvrir une pull request

- les commandes de vérification ci-dessus passent ;
- les invariants tiennent, et vous pouvez dire lequel votre changement touche ;
- ce que vous n'avez **pas** pu vérifier est écrit dans la description — c'est
  plus utile qu'une affirmation optimiste ;
- une ambiguïté qui touche aux données ou au protocole est signalée dans la
  pull request et résolue avant de modifier le contrat.

## Licence des contributions

Le projet est sous [AGPL-3.0](LICENSE). En proposant une contribution, vous
acceptez qu'elle soit distribuée sous cette licence.

## Une base de test, jamais celle de développement

`server/tests` **vide les tables** de la base désignée par `DATABASE_URL` — c'est
le travail de la fixture `clean_database`, et `RETROSAVE_TEST_DB=1` existe pour
que personne ne le fasse par accident. Le piège est qu'une base de dev et une
base de test portent souvent le même nom.

Utiliser une base séparée :

```sh
docker compose exec -T db psql -U retrosave -d postgres \
  -c "CREATE DATABASE retrosave_test OWNER retrosave;"

cd server
DATABASE_URL="postgresql+asyncpg://retrosave:retrosave@<hôte>:5432/retrosave_test" \
  RETROSAVE_TEST_DB=1 ../.venv/bin/python -m alembic upgrade head
DATABASE_URL="postgresql+asyncpg://retrosave:retrosave@<hôte>:5432/retrosave_test" \
  RETROSAVE_TEST_DB=1 ../.venv/bin/python -m pytest -q
```

La garde `RETROSAVE_TEST_DB=1` ne remplace pas l'isolation : les fixtures
vident les tables ciblées, y compris comptes, appareils et unités. N'utilisez
jamais l'URL d'une base de développement ou de production pour les tests.
