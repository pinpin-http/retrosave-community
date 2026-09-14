# Environnement de développement

Ce guide décrit l'environnement Python/Kotlin existant. Pour le nouveau
desktop C++/Qt, suivre les [commandes de construction](../../desktop/README.md)
et le [guide Qt](QT_DESKTOP.md) ; les outils ci-dessous ne suffisent pas à eux
seuls à construire la cible desktop V1.

## Ce qui est installé

- Python 3.12, isolé dans `.venv`, piloté par `uv`
- `pytest` et `ruff`, verrouillés dans `uv.lock`
- Git et GitHub CLI
- Docker Desktop avec Docker Compose
- JDK 21 fourni par Android Studio
- Android SDK Platform 35 et Build Tools 35
- Android Platform Tools (`adb`)
- Android Emulator avec accélération WHPX
- image Google APIs Android 13 / API 33
- AVD projet-local `RetroSave_Android13`

Les binaires téléchargés, caches et données de l’AVD restent ignorés par Git.
Le SDK Android commun est situé dans `%LOCALAPPDATA%\Android\Sdk`.

## Ouvrir un terminal prêt

Le script doit être *dot-sourcé* pour modifier le terminal courant :

```powershell
. .\scripts\dev\dev-env.ps1
```

Il configure `JAVA_HOME`, `ANDROID_HOME`, `ANDROID_SDK_ROOT`,
`ANDROID_USER_HOME`, `ANDROID_AVD_HOME`, `GRADLE_USER_HOME`, le cache `uv` et
le `PATH`, sans modifier les variables utilisateur globales de Windows.

## Diagnostic

```powershell
.\scripts\dev\doctor.ps1
```

Un avertissement sur la mémoire Docker n’empêche pas le développement. La
machine lui alloue actuellement environ 2 Go, ce qui devrait suffire au petit
stack POC mais sera serré si Docker et l’émulateur tournent simultanément.

## Émulateur Android 13

Lancement graphique :

```powershell
.\scripts\dev\start-emulator.ps1
```

Lancement headless pour les tests :

```powershell
.\scripts\dev\start-emulator.ps1 -Headless -WaitForBoot
```

Premier démarrage observé sur cette machine : environ 4 minutes. Les suivants
peuvent utiliser le quick boot et sont généralement plus courts.

Arrêt propre :

```powershell
.\scripts\dev\stop-emulator.ps1
```

L’AVD reproduit Android 13, la version du Thor. Il ne reproduit pas exactement
le matériel double-écran AYN ; les tests finaux SAF et double-écran devront
donc être faits sur le Thor réel.

## Python

```powershell
uv sync
uv run pytest
uv run ruff check .
```

Le projet exige explicitement Python 3.12 afin de ne pas utiliser par accident
le Python 3.13 également installé sur la machine.

## Android Studio

Ouvrir le module `android/` dans Android Studio. Le projet utilise son wrapper
`gradlew` ; aucun Gradle global n’est nécessaire.

Si `android/local.properties` est requis, utiliser :

```properties
sdk.dir=C\:\\Users\\votre-nom\\AppData\\Local\\Android\\Sdk
```

Ce fichier est volontairement ignoré par Git.

## Réinstallation idempotente

Le script suivant revalide les prérequis, réinstalle les paquets manquants,
recrée l’AVD s’il manque et synchronise l’environnement Python :

```powershell
.\scripts\dev\setup-dev.ps1
```
