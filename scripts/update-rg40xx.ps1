<#
.SYNOPSIS
  Compile le port aarch64 et le copie sur la RG40XX-H branchée en USB.

.DESCRIPTION
  Met à jour le binaire (SD2\ports\crusader\crusader), le lanceur (SD2\ROMS\Ports\Crusader.sh)
  et les sauvegardes : data\SAVEGAME\SAVEGAME.<n> du PC écrasent celles de ports\crusader\data\SAVEGAME
  (les emplacements absents du PC restent tels quels sur la console).
  Le reste des données du jeu (CRUSADER.CFG, iso, fichiers du jeu) n'est jamais touché.
  Par défaut la console est lue en MTP (appareil portable "RG40XX-H", sans lettre de lecteur).
  Avec -Root, copie dans un dossier normal à la place (carte SD montée, lecteur réseau...).

.PARAMETER Root
  Racine de la carte (contenant ports\ et ROMS\Ports\), ex. E:\. Désactive le mode MTP.

.PARAMETER Card
  Carte à utiliser en MTP : SD1 ou SD2 (défaut SD2, celle qui contient ports\crusader).

.PARAMETER Device
  Nom de l'appareil MTP (défaut RG40XX-H).

.PARAMETER SkipBuild
  Ne recompile pas, copie build-portmaster\crusader tel quel.

.PARAMETER SkipLauncher
  Ne met pas à jour le lanceur.

.PARAMETER SkipSaves
  Ne copie pas les sauvegardes.

.PARAMETER Saves
  Dossier des sauvegardes à copier (défaut data\SAVEGAME du dépôt).

.EXAMPLE
  scripts\update-rg40xx.ps1
  scripts\update-rg40xx.ps1 -SkipBuild
  scripts\update-rg40xx.ps1 -Root E:\
  scripts\update-rg40xx.ps1 -SkipBuild -SkipLauncher   # binaire + sauvegardes
  scripts\update-rg40xx.ps1 -SkipSaves                 # garde les sauvegardes de la console
#>
[CmdletBinding()]
param(
    [string]$Root,
    [string]$Card = 'SD2',
    [string]$Device = 'RG40XX-H',
    [switch]$SkipBuild,
    [switch]$SkipLauncher,
    [switch]$SkipSaves,
    [string]$Saves
)
$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
$portSrc = Join-Path $repo 'tools\portmaster\port'
$outDir = Join-Path $repo 'build-portmaster'
$binary = Join-Path $outDir 'crusader'
$tmp = Join-Path ([IO.Path]::GetTempPath()) 'update-rg40xx'
if (-not $Saves) { $Saves = Join-Path $repo 'data\SAVEGAME' }

# Seuls les emplacements SAVEGAME.<n> sont copiés (pas les copies renommées type SAVEGAME.10.avant-bombe).
$saveFiles = @()
if (-not $SkipSaves) {
    if (-not (Test-Path $Saves)) { throw "Dossier de sauvegardes absent : $Saves (ou utilise -SkipSaves)." }
    $saveFiles = @(Get-ChildItem $Saves -File | Where-Object Name -match '^SAVEGAME\.\d+$' |
        Sort-Object { [int]($_.Name -replace '^SAVEGAME\.', '') })
}

if (-not $SkipBuild) {
    Write-Host '== Compilation aarch64 (Zig) ==' -ForegroundColor Cyan
    & python (Join-Path $repo 'tools\portmaster\build.py') --out $outDir
    if ($LASTEXITCODE -ne 0) { throw 'La compilation a échoué.' }
}
if (-not (Test-Path $binary)) { throw "Binaire absent : $binary (relance sans -SkipBuild)." }

# Le lanceur doit rester en fins de ligne LF (bash sur la console).
New-Item -ItemType Directory -Force $tmp | Out-Null
$launcher = Join-Path $tmp 'Crusader.sh'
$text = [IO.File]::ReadAllText((Join-Path $portSrc 'Crusader.sh')) -replace "`r`n", "`n"
[IO.File]::WriteAllText($launcher, $text, (New-Object Text.UTF8Encoding $false))

# --- Copie vers un dossier MTP (shell Windows) ---------------------------------------------
function Get-MtpFolder($root, [string[]]$path, [switch]$Create) {
    $item = $root
    foreach ($name in $path) {
        $parent = $item.GetFolder
        $item = $parent.Items() | Where-Object Name -eq $name | Select-Object -First 1
        if (-not $item -and $Create) {
            $parent.NewFolder($name)
            for ($i = 0; -not $item -and $i -lt 20; $i++) {
                Start-Sleep -Milliseconds 250
                $item = $parent.Items() | Where-Object Name -eq $name | Select-Object -First 1
            }
        }
        if (-not $item) { throw "Dossier introuvable sur la console : $($path -join '\') (manque '$name')" }
    }
    $item.GetFolder
}

# $Settle : délai au bout duquel un fichier de même taille et même date que l'ancien est accepté
# (fichier inchangé). Les sauvegardes sont petites : 3 s suffisent.
function Copy-ToMtp($folder, [string]$file, [double]$Settle = 10) {
    $name = Split-Path -Leaf $file
    $size = (Get-Item $file).Length
    # Size / ModifyDate sont à 0 en MTP : on lit la taille et la date via ExtendedProperty / GetDetailsOf.
    $stat = {
        $it = $folder.Items() | Where-Object Name -eq $name | Select-Object -First 1
        if ($it) { [pscustomobject]@{ Size = [int64]$it.ExtendedProperty('System.Size'); Date = $folder.GetDetailsOf($it, 3) } }
    }
    $before = & $stat
    $start = Get-Date
    # 0x10 = répondre oui à tout (écrase), 0x04 = pas de boîte de progression
    $folder.CopyHere($file, 0x14)
    do {
        Start-Sleep -Milliseconds 500
        $now = & $stat
        $age = ((Get-Date) - $start).TotalSeconds
        # fichier identique à l'ancien (même date) : on accepte après $Settle s
        $done = $now -and $now.Size -eq $size -and (-not $before -or $now.Date -ne $before.Date -or $age -gt $Settle)
    } until ($done -or $age -gt 180)
    if (-not $done) { throw "Copie de $name non confirmée (taille attendue $size octets)." }
    Write-Host ("  {0}  {1:N2} Mo" -f $name, ($size / 1MB))
}

if ($Root) {
    Write-Host "== Installation dans $Root ==" -ForegroundColor Cyan
    $dest = Join-Path $Root 'ports\crusader'
    New-Item -ItemType Directory -Force (Join-Path $dest 'licenses') | Out-Null
    Copy-Item $binary (Join-Path $dest 'crusader') -Force
    Copy-Item (Join-Path $portSrc 'crusader\licenses\*') (Join-Path $dest 'licenses') -Force
    if (-not $SkipLauncher) {
        $rp = Join-Path $Root 'ROMS\Ports'
        New-Item -ItemType Directory -Force $rp | Out-Null
        Copy-Item $launcher (Join-Path $rp 'Crusader.sh') -Force
    }
    if ($saveFiles) {
        $sd = Join-Path $dest 'data\SAVEGAME'
        New-Item -ItemType Directory -Force $sd | Out-Null
        foreach ($f in $saveFiles) { Copy-Item $f.FullName (Join-Path $sd $f.Name) -Force }
        Write-Host ("  {0} sauvegardes -> data\SAVEGAME" -f $saveFiles.Count)
    }
} else {
    $shell = New-Object -ComObject Shell.Application
    $dev = $shell.Namespace(17).Items() | Where-Object Name -eq $Device | Select-Object -First 1
    if (-not $dev) { throw "'$Device' introuvable dans Ce PC : branche la console en USB (mode stockage/MTP) ou utilise -Root." }
    Write-Host "== Installation sur $Device\$Card ==" -ForegroundColor Cyan
    Copy-ToMtp (Get-MtpFolder $dev @($Card, 'ports', 'crusader')) $binary
    if (-not $SkipLauncher) {
        Copy-ToMtp (Get-MtpFolder $dev @($Card, 'ROMS', 'Ports')) $launcher
    }
    if ($saveFiles) {
        $sd = Get-MtpFolder $dev @($Card, 'ports', 'crusader', 'data', 'SAVEGAME') -Create
        foreach ($f in $saveFiles) { Copy-ToMtp $sd $f.FullName -Settle 3 }
    }
}
if ($saveFiles) {
    Write-Host ("OK : mise à jour terminée ({0} sauvegardes copiées, reste des données intact)." -f $saveFiles.Count) -ForegroundColor Green
} else {
    Write-Host 'OK : mise à jour terminée (données du jeu intactes).' -ForegroundColor Green
}
