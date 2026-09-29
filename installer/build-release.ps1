<#
  Construit l'installateur complet (Setup.exe) en une commande.

  Prerequis (une seule fois) :
    - Git
    - Node.js 22 ou plus
    - pnpm            (npm install -g pnpm)
    - Inno Setup 6.3+ (https://jrsoftware.org/isdl.php)

  Utilisation, depuis le dossier installer\ :
    .\build-release.ps1 -OverlayExe "C:\chemin\vers\ton_overlay.exe"

  Structure attendue du projet :
    plugin\index.ts, plugin\native.ts    (le plugin Vencord)
    installer\...                        (ce dossier)
#>
param(
    [Parameter(Mandatory = $true)][string]$OverlayExe,
    [string]$PluginDir = "",
    [string]$Version = "1.0.0",
    # Branche ou commit Vencord a embarquer (mets un commit pour figer la version)
    [string]$VencordRef = "main",
    # Chemin complet de ISCC.exe si Inno Setup est installe dans un endroit inhabituel
    [string]$IsccPath = ""
)

$ErrorActionPreference = "Stop"

# Dossier de ce script. $PSScriptRoot peut etre vide selon la version de
# PowerShell, donc on prevoit un plan B.
$here = $PSScriptRoot
if (-not $here) { $here = Split-Path -Parent $MyInvocation.MyCommand.Path }
if (-not $here) { $here = (Get-Location).Path }

if (-not $PluginDir) { $PluginDir = Join-Path $here "..\plugin" }

function Require([string]$Command, [string]$Hint) {
    if (-not (Get-Command $Command -ErrorAction SilentlyContinue)) {
        throw "'$Command' est introuvable. $Hint"
    }
}

# Lance une commande native et s'arrete si elle echoue.
function Run([string]$Label, [scriptblock]$Block) {
    Write-Host "==> $Label" -ForegroundColor Cyan
    $global:LASTEXITCODE = 0
    & $Block
    if ($LASTEXITCODE -ne 0) {
        throw "Echec : $Label (code $LASTEXITCODE)"
    }
}

Require git  "Installe Git : https://git-scm.com"
Require node "Installe Node.js 22 ou plus : https://nodejs.org"
Require pnpm "Installe pnpm : npm install -g pnpm"

$iscc = $null

if ($IsccPath -and (Test-Path $IsccPath)) {
    $iscc = (Resolve-Path $IsccPath).Path
}

if (-not $iscc) {
    $cmd = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($cmd) { $iscc = $cmd.Source }
}

if (-not $iscc) {
    $roots = @("$env:LOCALAPPDATA\Programs", ${env:ProgramFiles(x86)}, $env:ProgramFiles) |
        Where-Object { $_ -and (Test-Path $_) }

    foreach ($root in $roots) {
        $found = Get-ChildItem -Path $root -Directory -Filter "Inno Setup*" -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending |
            ForEach-Object { Join-Path $_.FullName "ISCC.exe" } |
            Where-Object { Test-Path $_ } |
            Select-Object -First 1

        if ($found) { $iscc = $found; break }
    }
}

if (-not $iscc) {
    throw "Inno Setup est introuvable. Installe-le avec : winget install JRSoftware.InnoSetup  (puis rouvre le terminal), ou passe -IsccPath ""C:\...\ISCC.exe"""
}

Write-Host "Inno Setup : $iscc"

if (-not (Test-Path $OverlayExe)) { throw "Overlay introuvable : $OverlayExe" }
if (-not (Test-Path $PluginDir))  { throw "Dossier plugin introuvable : $PluginDir (attendu a cote du dossier installer)" }

$overlay   = (Resolve-Path $OverlayExe).Path
$pluginDir = (Resolve-Path $PluginDir).Path

foreach ($f in "index.ts", "native.ts") {
    if (-not (Test-Path (Join-Path $pluginDir $f))) { throw "Fichier manquant : $pluginDir\$f" }
}

$work = Join-Path $here "build"
$vc   = Join-Path $work "Vencord"
$stg  = Join-Path $work "staging"

New-Item -ItemType Directory -Path $work -Force | Out-Null

# --- 1. Vencord -------------------------------------------------------------
if (-not (Test-Path (Join-Path $vc ".git"))) {
    Run "Clone de Vencord" { git clone --depth 1 https://github.com/Vendicated/Vencord.git $vc }
}
Run "Recuperation de $VencordRef" { git -C $vc fetch --depth 1 origin $VencordRef }
Run "Checkout"                    { git -C $vc checkout --force FETCH_HEAD }

# --- 2. Plugin --------------------------------------------------------------
$pluginDest = Join-Path $vc "src\userplugins\TaskbarTyping"
if (Test-Path $pluginDest) { Remove-Item $pluginDest -Recurse -Force }
New-Item -ItemType Directory -Path $pluginDest -Force | Out-Null
Copy-Item (Join-Path $pluginDir "index.ts"), (Join-Path $pluginDir "native.ts") -Destination $pluginDest

# --- 3. Build ---------------------------------------------------------------
$distDir = Join-Path $vc "dist"
if (Test-Path $distDir) { Remove-Item $distDir -Recurse -Force }

Push-Location $vc
try {
    Run "pnpm install" { pnpm install --frozen-lockfile }
    # --disable-updater : l'updater integre de Vencord ne doit pas ecraser ce build
    Run "pnpm build"   { pnpm build --disable-updater }
}
finally {
    Pop-Location
}

foreach ($f in "patcher.js", "preload.js", "renderer.js", "renderer.css") {
    if (-not (Test-Path (Join-Path $distDir $f))) { throw "Build incomplet : dist\$f absent" }
}
if (-not (Select-String -Path (Join-Path $distDir "renderer.js") -Pattern "TaskbarTyping" -Quiet)) {
    throw "Le plugin TaskbarTyping n'apparait pas dans le build. Verifie le dossier src\userplugins."
}

# --- 4. Staging -------------------------------------------------------------
if (Test-Path $stg) { Remove-Item $stg -Recurse -Force }
New-Item -ItemType Directory -Path (Join-Path $stg "vencord\dist") -Force | Out-Null

Copy-Item $overlay (Join-Path $stg "DiscordTaskbarTyping.exe")
Get-ChildItem $distDir -File | Copy-Item -Destination (Join-Path $stg "vencord\dist")
Write-Host ""
Write-Host "=== VERIFICATION DU STAGING ===" -ForegroundColor Yellow

Get-ChildItem (Join-Path $stg "vencord\dist") |
    Select-Object Name, Length |
    Format-Table -AutoSize

foreach ($f in "patcher.js", "preload.js", "renderer.js", "renderer.css") {
    $path = Join-Path $stg "vencord\dist\$f"

    if (-not (Test-Path $path)) {
        throw "ERREUR STAGING : $path est absent"
    }

    Write-Host "OK : $f" -ForegroundColor Green
}

# package.json vide : evite que Node remonte l'arborescence et tombe sur un
# package.json "type: module" qui casserait patcher.js
Set-Content -Path (Join-Path $stg "vencord\dist\package.json") -Value "{}" -Encoding Ascii

Write-Host "==> Telechargement de VencordInstallerCli.exe" -ForegroundColor Cyan
Invoke-WebRequest -UseBasicParsing `
    -Uri "https://github.com/Vencord/Installer/releases/latest/download/VencordInstallerCli.exe" `
    -OutFile (Join-Path $stg "vencord\VencordInstallerCli.exe")

Copy-Item (Join-Path $vc "LICENSE") (Join-Path $stg "LICENSE-Vencord-GPL3.txt")

$commit = (git -C $vc rev-parse HEAD).Trim()
@(
    "Discord Taskbar Typing $Version",
    "Vencord commit : $commit",
    "Source Vencord : https://github.com/Vendicated/Vencord/tree/$commit",
    "Date du build  : $(Get-Date -Format s)"
) | Set-Content -Path (Join-Path $stg "BUILD-INFO.txt") -Encoding Utf8

# --- 5. Installateur --------------------------------------------------------
Run "Compilation de l'installateur" {
    & $iscc "/DAppVersion=$Version" "/DStagingDir=$stg" (Join-Path $here "TaskbarTyping.iss")
}

Write-Host ""
Write-Host "Termine : $(Join-Path $here 'output\DiscordTaskbarTyping-Setup.exe')" -ForegroundColor Green
Write-Host "Vencord embarque : $commit"
