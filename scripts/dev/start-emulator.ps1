# start-emulator.ps1 — démarre l'AVD Android 13 de test.
# Usage courant : .\scripts\dev\start-emulator.ps1 -WaitForBoot
# Usage CI      : .\scripts\dev\start-emulator.ps1 -Headless -WaitForBoot
[CmdletBinding()]
param(
    # -Headless : mode sans fenêtre pour la CI. Active swiftshader (rendu logiciel)
    # car les agents CI n'ont pas de GPU. Plus lent qu'avec WHPX mais fonctionnel.
    [switch]$Headless,
    # -ColdBoot : ignore le snapshot de démarrage rapide. À utiliser quand le
    # snapshot est corrompu ou après une mise à jour de l'image système.
    [switch]$ColdBoot,
    # -WaitForBoot : bloque jusqu'à ce que sys.boot_completed == 1.
    # Indispensable dans les scripts de CI pour ne pas lancer les tests trop tôt.
    [switch]$WaitForBoot,
    # Plage 5554-5682 : convention adb, ports pairs uniquement (impairs réservés
    # au port de contrôle de l'émulateur). Défaut 5556 pour ne pas écraser un
    # émulateur Android Studio éventuel sur 5554.
    [ValidateRange(5554, 5682)]
    [int]$Port = 5556
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'dev-env.ps1')

$serial = "emulator-$Port"
$adb = Join-Path $env:ANDROID_HOME 'platform-tools\adb.exe'
$emulator = Join-Path $env:ANDROID_HOME 'emulator\emulator.exe'

# Idempotent : si l'émulateur tourne déjà sur ce port, rien à faire.
$devices = (& $adb devices) -join "`n"
if ($devices -match [regex]::Escape($serial)) {
    Write-Host "$serial est deja lance." -ForegroundColor Yellow
    exit 0
}

$arguments = @(
    '-avd', 'RetroSave_Android13'
    '-port', "$Port"
    '-memory', '2048'   # 2 Go : minimum pour Android 13 + les émulateurs de jeux
)

if ($Headless) {
    $arguments += @(
        '-no-window'
        '-no-audio'
        '-no-boot-anim'
        # swiftshader_indirect = rendu OpenGL ES en logiciel, sans GPU.
        # `auto` choisirait WHPX s'il est disponible, mais en CI on force le mode
        # logiciel pour éviter les dépendances matérielles non portables.
        '-gpu', 'swiftshader_indirect'
    )
}

if ($ColdBoot) {
    $arguments += '-no-snapshot'
}

if ($Headless) {
    $process = Start-Process -FilePath $emulator -ArgumentList $arguments `
        -WindowStyle Hidden -PassThru
}
else {
    $process = Start-Process -FilePath $emulator -ArgumentList $arguments -PassThru
}

Write-Host "AVD lance (PID $($process.Id), $serial)." -ForegroundColor Green

if (-not $WaitForBoot) {
    Write-Host "Etat: adb -s $serial shell getprop sys.boot_completed"
    exit 0
}

# Android 13 met jusqu'à 3-4 minutes à démarrer sur swiftshader; on alloue 6 min
# pour les machines de CI les plus lentes. On vérifie sys.boot_completed plutôt
# qu'un simple sleep pour ne pas surestimer le délai sur une machine rapide.
$deadline = (Get-Date).AddMinutes(6)
$bootComplete = ''

do {
    Start-Sleep -Seconds 5
    $process.Refresh()
    if ($process.HasExited) {
        throw "L'emulateur s'est arrete avant la fin du boot."
    }

    # L'émulateur peut être visible dans `adb devices` en état "offline" avant d'être
    # "device". On attend d'abord le statut "device" avant de lire la propriété.
    $deviceList = (& $adb devices) -join "`n"
    if ($deviceList -match "$([regex]::Escape($serial))\s+device") {
        $bootComplete = ((& $adb -s $serial shell getprop sys.boot_completed 2>$null) |
            Out-String).Trim()
    }
} while ($bootComplete -ne '1' -and (Get-Date) -lt $deadline)

if ($bootComplete -ne '1') {
    throw "Android n'a pas fini de demarrer dans les 6 minutes."
}

Write-Host 'Android est completement demarre.' -ForegroundColor Green
