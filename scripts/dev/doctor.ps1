# doctor.ps1 — vérifie que l'environnement de développement est complet et opérationnel.
# Lancé automatiquement en fin de setup-dev.ps1, et manuellement à tout moment via :
#   .\scripts\dev\doctor.ps1
# Correspond à la commande `rsc doctor` du CLAUDE.md §12 pour le côté CLI.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Continue'
. (Join-Path $PSScriptRoot 'dev-env.ps1')

$checks = [System.Collections.Generic.List[object]]::new()

function Add-Check {
    param(
        [string]$Name,
        [bool]$Ok,
        [string]$Detail,
        # Required=$false : avertissement seulement, pas bloquant.
        # Ex. : mémoire Docker insuffisante = le build tourne mais lentement.
        [bool]$Required = $true
    )

    $status = if ($Ok) {
        'OK'
    }
    elseif ($Required) {
        'ERREUR'
    }
    else {
        'AVERT.'
    }

    $script:checks.Add([pscustomobject]@{
        'Statut' = $status
        'Element' = $Name
        'Detail' = $Detail
    })
}

function Get-CommandVersion {
    param(
        [string]$Command,
        [string[]]$Arguments = @('--version')
    )

    $resolved = Get-Command $Command -ErrorAction SilentlyContinue
    if (-not $resolved) {
        return [pscustomobject]@{ Ok = $false; Text = 'commande introuvable' }
    }

    $output = & $resolved.Source @Arguments 2>&1
    $code = $LASTEXITCODE
    $text = (($output | Select-Object -First 2) -join ' ').Trim()
    return [pscustomobject]@{ Ok = ($code -eq 0); Text = $text }
}

$git = Get-CommandVersion 'git'
Add-Check 'Git' $git.Ok $git.Text

$uv = Get-CommandVersion 'uv'
Add-Check 'uv' $uv.Ok $uv.Text

# `uv run --python 3.12` garantit qu'on teste l'interpréteur du projet, pas le
# Python système qui pourrait être une autre version.
$pythonOutput = & uv run --python 3.12 python --version 2>&1
$pythonOk = $LASTEXITCODE -eq 0 -and "$pythonOutput" -match '^Python 3\.12\.'
Add-Check 'Python 3.12' $pythonOk (($pythonOutput | Select-Object -First 1) -join '')

# On vérifie Java 21 spécifiquement : Gradle 8 + AGP 8 exigent JDK 17+ et nous
# ciblons 21 (LTS). Un JDK 17 laisserait passer le build mais planterait sur
# des fonctionnalités Java 21 utilisées par Kotlin 2.x.
$java = Get-CommandVersion 'java'
Add-Check 'JDK' ($java.Ok -and $java.Text -match '21\.') $java.Text

$adb = Get-CommandVersion 'adb'
Add-Check 'Android Platform Tools' $adb.Ok $adb.Text

$emulator = Get-CommandVersion 'emulator'
Add-Check 'Android Emulator' ($emulator.Ok -or $emulator.Text -match 'Android emulator version') `
    $emulator.Text

# WHPX (Windows Hypervisor Platform) = accélération matérielle de l'AVD sur Windows.
# Sans WHPX, l'émulateur tourne en pur logiciel → 10× plus lent, inutilisable.
$accelOutput = & emulator -accel-check 2>&1
$accelText = ($accelOutput -join ' ').Trim()
Add-Check 'Acceleration WHPX' ($LASTEXITCODE -eq 0 -and $accelText -match 'usable') $accelText

# Platform 35 = targetSdk : nécessaire pour compiler l'app, même si on tourne sur API 33.
$platform35 = Join-Path $env:ANDROID_HOME 'platforms\android-35\android.jar'
Add-Check 'SDK Platform 35' (Test-Path -LiteralPath $platform35) $platform35

# Image Android 13 (API 33) = minSdk de l'app, système de l'AVD de test.
$image33 = Join-Path $env:ANDROID_HOME 'system-images\android-33\google_apis\x86_64\system.img'
Add-Check 'Image Android 13' (Test-Path -LiteralPath $image33) $image33

$avdConfig = Join-Path $env:ANDROID_AVD_HOME 'RetroSave_Android13.avd\config.ini'
Add-Check 'AVD RetroSave_Android13' (Test-Path -LiteralPath $avdConfig) $avdConfig

$dockerVersion = & docker info --format '{{.ServerVersion}}' 2>&1
$dockerOk = $LASTEXITCODE -eq 0
Add-Check 'Docker Engine' $dockerOk (($dockerVersion | Select-Object -First 1) -join '')

if ($dockerOk) {
    $dockerMemoryRaw = & docker info --format '{{.MemTotal}}' 2>&1
    [long]$dockerMemory = 0
    [void][long]::TryParse("$dockerMemoryRaw".Trim(), [ref]$dockerMemory)
    $dockerGb = [math]::Round($dockerMemory / 1GB, 1)
    # 4 Go minimum : PostgreSQL + MinIO + serveur + Gradle daemon tiennent ensemble.
    # En dessous, le Gradle daemon se fait tuer par l'OOM du Docker Desktop.
    Add-Check 'Memoire Docker' ($dockerMemory -ge 4GB) "$dockerGb Go alloues" $false
}

$checks | Format-Table -AutoSize

$requiredFailures = @($checks | Where-Object { $_.Statut -eq 'ERREUR' })
if ($requiredFailures.Count -gt 0) {
    Write-Error "$($requiredFailures.Count) prerequis obligatoire(s) en echec."
    exit 1
}

Write-Host 'Environnement RetroSave operationnel.' -ForegroundColor Green
