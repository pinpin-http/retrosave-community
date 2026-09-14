# setup-dev.ps1 — à lancer UNE SEULE FOIS sur une machine neuve (ou après un
# `git clean -fdx`). Installe uv, les Android command-line tools, le SDK Android,
# et crée l'AVD de test. Android Studio doit déjà être installé (JDK embarqué).
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$toolsDir = Join-Path $projectRoot '.tools'
$downloadsDir = Join-Path $toolsDir 'downloads'
$uvDir = Join-Path $toolsDir 'uv'
$androidSdk = Join-Path $env:LOCALAPPDATA 'Android\Sdk'
$javaHome = 'C:\Program Files\Android\Android Studio\jbr'
$cmdlineTools = Join-Path $androidSdk 'cmdline-tools\latest'

# Versions verrouillées : toute mise à jour doit aussi mettre à jour les checksums.
$androidToolsVersion = '15859902'
$androidToolsSha256 = '90ae805d20434428bffcb699c290860f19bb5f66a67e6b330067e3de801fb04a'
$androidToolsUrl =
    "https://dl.google.com/android/repository/commandlinetools-win-$($androidToolsVersion)_latest.zip"
$uvVersion = '0.12.0'
$uvBaseUrl = "https://github.com/astral-sh/uv/releases/download/$uvVersion"

New-Item -ItemType Directory -Force -Path $downloadsDir | Out-Null

# Prérequis manuels : on vérifie leur présence avant de télécharger quoi que ce soit.
if (-not (Test-Path -LiteralPath $javaHome)) {
    throw 'Android Studio avec son JDK integre est requis.'
}

if (-not (Test-Path -LiteralPath $androidSdk)) {
    throw "SDK Android introuvable: $androidSdk"
}

if (-not (Test-Path -LiteralPath 'C:\Program Files\Git\cmd\git.exe')) {
    throw 'Git for Windows est requis.'
}

if (-not (Test-Path -LiteralPath (Join-Path $uvDir 'uv.exe'))) {
    $uvZip = Join-Path $downloadsDir 'uv-x86_64-pc-windows-msvc.zip'
    $uvChecksum = "$uvZip.sha256"
    Invoke-WebRequest -UseBasicParsing -Uri "$uvBaseUrl/uv-x86_64-pc-windows-msvc.zip" `
        -OutFile $uvZip
    # Le fichier .sha256 est téléchargé depuis le même dépôt GitHub que l'archive :
    # protection minimale contre un téléchargement corrompu (réseau) ou modifié
    # (si le CDN est compromis). Ce n'est pas une garantie cryptographique forte,
    # mais c'est mieux que rien pour un outil de build.
    Invoke-WebRequest -UseBasicParsing `
        -Uri "$uvBaseUrl/uv-x86_64-pc-windows-msvc.zip.sha256" `
        -OutFile $uvChecksum

    $expected = ((Get-Content -Raw $uvChecksum).Trim() -split '\s+')[0]
    $actual = (Get-FileHash -Algorithm SHA256 $uvZip).Hash
    if ($actual -ne $expected) {
        throw "Checksum uv invalide: $actual"
    }

    Expand-Archive -LiteralPath $uvZip -DestinationPath $uvDir
}

if (-not (Test-Path -LiteralPath (Join-Path $cmdlineTools 'bin\sdkmanager.bat'))) {
    $androidZip = Join-Path $downloadsDir 'android-command-line-tools.zip'
    $extractDir = Join-Path $toolsDir 'android-command-line-extracted'
    Invoke-WebRequest -UseBasicParsing -Uri $androidToolsUrl -OutFile $androidZip

    $actual = (Get-FileHash -Algorithm SHA256 $androidZip).Hash.ToLowerInvariant()
    if ($actual -ne $androidToolsSha256) {
        throw "Checksum Android command-line tools invalide: $actual"
    }

    Expand-Archive -LiteralPath $androidZip -DestinationPath $extractDir
    New-Item -ItemType Directory -Force -Path (Split-Path $cmdlineTools) | Out-Null
    Move-Item -LiteralPath (Join-Path $extractDir 'cmdline-tools') `
        -Destination $cmdlineTools
}

$env:JAVA_HOME = $javaHome
$env:PATH = "C:\Windows\System32;$javaHome\bin;$env:PATH"
$sdkManager = Join-Path $cmdlineTools 'bin\sdkmanager.bat'

# Paquets installés :
# - platforms;android-35 : targetSdk de l'app (API de compilation)
# - build-tools;35.0.0   : aapt2, d8, zipalign — même version que build.gradle
# - platform-tools        : adb
# - emulator              : emulator.exe
# - system-images;android-33;google_apis;x86_64 : Android 13 (minSdk de l'app) en
#   architecture x86_64 pour l'AVD. `google_apis` plutôt que `google_apis_playstore`
#   car Play Store n'est pas utile pour les tests et verrouille davantage le système.
& $sdkManager --sdk_root=$androidSdk `
    'platforms;android-35' `
    'build-tools;35.0.0' `
    'platform-tools' `
    'emulator' `
    'system-images;android-33;google_apis;x86_64'

if ($LASTEXITCODE -ne 0) {
        throw "Echec de l'installation des paquets Android."
}

$env:ANDROID_HOME = $androidSdk
$env:ANDROID_SDK_ROOT = $androidSdk
$env:ANDROID_USER_HOME = Join-Path $projectRoot '.android'
$env:ANDROID_AVD_HOME = Join-Path $env:ANDROID_USER_HOME 'avd'
New-Item -ItemType Directory -Force -Path $env:ANDROID_AVD_HOME | Out-Null

$avdConfig = Join-Path $env:ANDROID_AVD_HOME 'RetroSave_Android13.avd\config.ini'
if (-not (Test-Path -LiteralPath $avdConfig)) {
    # 'no' | avdmanager répond automatiquement à la question interactive
    # "Do you wish to create a custom hardware profile?" sans bloquer le script.
    # pixel_5 est le profil le plus proche du Thor en résolution et densité.
    'no' | & (Join-Path $cmdlineTools 'bin\avdmanager.bat') create avd `
        --name 'RetroSave_Android13' `
        --package 'system-images;android-33;google_apis;x86_64' `
        --device 'pixel_5'
    if ($LASTEXITCODE -ne 0) {
        throw "Echec de la creation de l'AVD."
    }
}

& (Join-Path $uvDir 'uv.exe') sync --project $projectRoot --python 3.12
if ($LASTEXITCODE -ne 0) {
    throw "Echec de la synchronisation de l'environnement Python."
}

# Doctor en fin de setup : valide que tout ce qu'on vient d'installer est réellement
# utilisable (Java trouvé, WHPX actif, AVD créé…).
& (Join-Path $PSScriptRoot 'doctor.ps1')
