# stop-emulator.ps1 — arrête proprement l'AVD via la commande adb emu kill
# (équivalent à File > Quit dans l'émulateur). Idempotent : si l'AVD n'est pas
# lancé, le script sort proprement sans erreur.
[CmdletBinding()]
param(
    [ValidateRange(5554, 5682)]
    [int]$Port = 5556
)

# Continue plutôt que Stop : on veut pouvoir rapporter l'erreur et sortir avec
# un code d'erreur non nul, plutôt que de lever une exception PowerShell.
$ErrorActionPreference = 'Continue'
. (Join-Path $PSScriptRoot 'dev-env.ps1')

$serial = "emulator-$Port"
$adb = Join-Path $env:ANDROID_HOME 'platform-tools\adb.exe'
$devices = (& $adb devices) -join "`n"

if ($devices -notmatch [regex]::Escape($serial)) {
    Write-Host "$serial n'est pas lance." -ForegroundColor Yellow
    exit 0
}

# `emu kill` envoie une commande de fermeture propre via le port de contrôle de
# l'émulateur. Préféré à un kill du processus (qui ne sauvegarderait pas le snapshot).
& $adb -s $serial emu kill
if ($LASTEXITCODE -ne 0) {
    Write-Error "Impossible d'arreter $serial proprement."
    exit 1
}

# On attend que l'émulateur disparaisse de `adb devices` pour confirmer l'arrêt.
$deadline = (Get-Date).AddSeconds(30)
do {
    Start-Sleep -Seconds 1
    $devices = (& $adb devices) -join "`n"
} while ($devices -match [regex]::Escape($serial) -and (Get-Date) -lt $deadline)

if ($devices -match [regex]::Escape($serial)) {
    Write-Error "$serial repond encore apres la demande d'arret."
    exit 1
}

Write-Host "$serial arrete." -ForegroundColor Green
