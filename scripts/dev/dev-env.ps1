# dev-env.ps1 — source ce script (. .\scripts\dev\dev-env.ps1) depuis n'importe quel
# terminal PowerShell pour obtenir l'environnement de développement RetroSave complet.
# Tous les autres scripts du dossier le sourcent via `. (Join-Path $PSScriptRoot 'dev-env.ps1')`.
[CmdletBinding()]
param()

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$androidSdk = Join-Path $env:LOCALAPPDATA 'Android\Sdk'
$androidStudio = 'C:\Program Files\Android\Android Studio'
$javaHome = Join-Path $androidStudio 'jbr'  # JDK embarqué dans Android Studio, toujours aligné avec le SDK
$gitCmd = 'C:\Program Files\Git\cmd'

# Vérification précoce : mieux vaut un message clair qu'une erreur cryptique de gradle ou adb.
$requiredPaths = @{
    'Android SDK' = $androidSdk
    'Android Studio JDK' = $javaHome
    'Git' = Join-Path $gitCmd 'git.exe'
    'uv' = Join-Path $projectRoot '.tools\uv\uv.exe'
}

foreach ($entry in $requiredPaths.GetEnumerator()) {
    if (-not (Test-Path -LiteralPath $entry.Value)) {
        throw "$($entry.Key) introuvable: $($entry.Value). Lancez .\scripts\dev\setup-dev.ps1."
    }
}

$env:RETROSAVE_ROOT = $projectRoot
$env:JAVA_HOME = $javaHome
$env:ANDROID_HOME = $androidSdk
$env:ANDROID_SDK_ROOT = $androidSdk  # alias historique, certains plugins Gradle ne lisent que celui-là

# Redirigés dans le projet pour que chaque dépôt cloné reste isolé :
# un `adb kill-server` sur un autre projet ne casse pas nos AVD.
$env:ANDROID_USER_HOME = Join-Path $projectRoot '.android'
$env:ANDROID_AVD_HOME = Join-Path $env:ANDROID_USER_HOME 'avd'

# Gradle écrit son cache global dans GRADLE_USER_HOME. On le pointe dans le projet
# pour éviter des conflits de version de plugins avec d'autres projets Android sur
# la même machine, et pour que `git clean -fdx` puisse tout nettoyer d'un coup.
$env:GRADLE_USER_HOME = Join-Path $projectRoot '.gradle-user-home'

# UV_PROJECT_ENVIRONMENT force uv à utiliser le .venv du projet même si un venv
# système est actif — pas de risque de polluer l'interpréteur global.
$env:UV_CACHE_DIR = Join-Path $projectRoot '.cache\uv'
$env:UV_PROJECT_ENVIRONMENT = Join-Path $projectRoot '.venv'

# L'ordre de $prepend est important : System32 en premier pour les commandes Windows
# de base, puis les outils projet. Select-Object -Unique déduplique sans changer l'ordre.
$prepend = @(
    'C:\Windows\System32'
    $gitCmd
    (Join-Path $javaHome 'bin')
    (Join-Path $androidSdk 'platform-tools')   # adb
    (Join-Path $androidSdk 'emulator')          # emulator.exe
    (Join-Path $androidSdk 'cmdline-tools\latest\bin')  # sdkmanager, avdmanager
    (Join-Path $projectRoot '.tools\uv')
    (Join-Path $projectRoot '.venv\Scripts')    # rsc, pytest, ruff, etc.
)

$existing = @($env:PATH -split ';' | Where-Object { $_ })
$env:PATH = (@($prepend + $existing) |
    Select-Object -Unique) -join ';'

Write-Host "RetroSave pret dans $projectRoot" -ForegroundColor Green
Write-Host 'Python: uv run python   Android: adb / emulator   Infra: docker compose'
