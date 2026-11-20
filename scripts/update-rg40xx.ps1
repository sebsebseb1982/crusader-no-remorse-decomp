<#
.SYNOPSIS
  Compile le port aarch64 et le copie sur la RG40XX-H branchée en USB.

.DESCRIPTION
  Met à jour le binaire (SD2\ports\crusader-no-remorse\crusader-no-remorse), le lanceur (SD2\ROMS\Ports\Crusader No Remorse.sh)
  et les sauvegardes : data\SAVEGAME\SAVEGAME.<n> du PC écrasent celles de ports\crusader-no-remorse\data\SAVEGAME
  (les emplacements absents du PC restent tels quels sur la console).
  Copie aussi l'artwork du menu Ports de muOS (cover.png, screenshot.png, description) dans
  MUOS\info\catalogue\External - Ports\{box,preview,text}\Crusader No Remorse.*, sur la carte qui contient MUOS.
  Le reste des données du jeu (CRUSADER.CFG, iso, fichiers du jeu) n'est jamais touché.
  Par défaut la console est lue en MTP (appareil portable "RG40XX-H", sans lettre de lecteur).
  Avec -Root, copie dans un dossier normal à la place (carte SD montée, lecteur réseau...).

.PARAMETER Root
  Racine de la carte (contenant ports\ et ROMS\Ports\), ex. E:\. Désactive le mode MTP.

.PARAMETER Card
  Carte à utiliser en MTP : SD1 ou SD2 (défaut SD2, celle qui contient ports\crusader-no-remorse).

.PARAMETER Device
  Nom de l'appareil MTP (défaut RG40XX-H).

.PARAMETER SkipBuild
  Ne recompile pas, copie build-portmaster\crusader-no-remorse tel quel.

.PARAMETER SkipLauncher
  Ne met pas à jour le lanceur.

.PARAMETER SkipArtwork
  Ne copie pas l'artwork dans le catalogue muOS.

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
    [switch]$SkipArtwork,
    [switch]$SkipSaves,
    [string]$Saves
)
$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
$portSrc = Join-Path $repo 'tools\portmaster\port'
$outDir = Join-Path $repo 'build-portmaster'
$binary = Join-Path $outDir 'crusader-no-remorse'
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
$launcher = Join-Path $tmp 'Crusader No Remorse.sh'
$text = [IO.File]::ReadAllText((Join-Path $portSrc 'Crusader No Remorse.sh')) -replace "`r`n", "`n"
[IO.File]::WriteAllText($launcher, $text, (New-Object Text.UTF8Encoding $false))

# Artwork du menu Ports de muOS : le catalogue nomme les fichiers comme le lanceur (sans .sh).
# Un dossier par destination (CopyHere garde le nom du fichier source).
$artName = 'Crusader No Remorse'
$art = @{}
if (-not $SkipArtwork) {
    $art = [ordered]@{
        box     = Join-Path $portSrc 'cover.png'
        preview = Join-Path $portSrc 'screenshot.png'
        text    = $null
    }
    foreach ($kind in @($art.Keys)) {
        $d = Join-Path $tmp "art-$kind"
        New-Item -ItemType Directory -Force $d | Out-Null
        if ($kind -eq 'text') {
            $desc = ([xml][IO.File]::ReadAllText((Join-Path $portSrc 'gameinfo.xml'))).gameList.game.desc.Trim()
            $f = Join-Path $d "$artName.txt"
            [IO.File]::WriteAllText($f, "$desc`n", (New-Object Text.UTF8Encoding $false))
        } else {
            $f = Join-Path $d "$artName.png"
            Copy-Item $art[$kind] $f -Force
        }
        $art[$kind] = $f
    }
}

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
    $dest = Join-Path $Root 'ports\crusader-no-remorse'
    New-Item -ItemType Directory -Force (Join-Path $dest 'licenses') | Out-Null
    Copy-Item $binary (Join-Path $dest 'crusader-no-remorse') -Force
    Copy-Item (Join-Path $portSrc 'crusader-no-remorse\licenses\*') (Join-Path $dest 'licenses') -Force
    if (-not $SkipLauncher) {
        $rp = Join-Path $Root 'ROMS\Ports'
        New-Item -ItemType Directory -Force $rp | Out-Null
        Copy-Item $launcher (Join-Path $rp 'Crusader No Remorse.sh') -Force
    }
    if ($saveFiles) {
        $sd = Join-Path $dest 'data\SAVEGAME'
        New-Item -ItemType Directory -Force $sd | Out-Null
        foreach ($f in $saveFiles) { Copy-Item $f.FullName (Join-Path $sd $f.Name) -Force }
        Write-Host ("  {0} sauvegardes -> data\SAVEGAME" -f $saveFiles.Count)
    }
    if ($art.Count) {
        $cat = Join-Path $Root 'MUOS\info\catalogue'
        if (Test-Path $cat) {
            foreach ($kind in $art.Keys) {
                $d = Join-Path $cat "External - Ports\$kind"
                New-Item -ItemType Directory -Force $d | Out-Null
                Copy-Item $art[$kind] $d -Force
            }
            Write-Host "  artwork -> MUOS\info\catalogue\External - Ports"
        } else {
            Write-Warning "Pas de MUOS\info\catalogue dans $Root : artwork non copié (MUOS est sur l'autre carte ?)."
        }
    }
} else {
    $shell = New-Object -ComObject Shell.Application
    $dev = $shell.Namespace(17).Items() | Where-Object Name -eq $Device | Select-Object -First 1
    if (-not $dev) { throw "'$Device' introuvable dans Ce PC : branche la console en USB (mode stockage/MTP) ou utilise -Root." }
    Write-Host "== Installation sur $Device\$Card ==" -ForegroundColor Cyan
    Copy-ToMtp (Get-MtpFolder $dev @($Card, 'ports', 'crusader-no-remorse') -Create) $binary
    if (-not $SkipLauncher) {
        Copy-ToMtp (Get-MtpFolder $dev @($Card, 'ROMS', 'Ports')) $launcher
    }
    if ($saveFiles) {
        $sd = Get-MtpFolder $dev @($Card, 'ports', 'crusader-no-remorse', 'data', 'SAVEGAME') -Create
        foreach ($f in $saveFiles) { Copy-ToMtp $sd $f.FullName -Settle 3 }
    }
    if ($art.Count) {
        # MUOS est en général sur SD1 (carte système), les ports sur SD2.
        $muosCard = $null
        foreach ($c in @('SD1', 'SD2')) {
            try { Get-MtpFolder $dev @($c, 'MUOS', 'info', 'catalogue') | Out-Null; $muosCard = $c; break } catch { }
        }
        if ($muosCard) {
            foreach ($kind in $art.Keys) {
                $d = Get-MtpFolder $dev @($muosCard, 'MUOS', 'info', 'catalogue', 'External - Ports', $kind) -Create
                Copy-ToMtp $d $art[$kind] -Settle 3
            }
        } else {
            Write-Warning "MUOS\info\catalogue introuvable sur SD1 et SD2 : artwork non copié."
        }
    }
}
if ($saveFiles) {
    Write-Host ("OK : mise à jour terminée ({0} sauvegardes copiées, reste des données intact)." -f $saveFiles.Count) -ForegroundColor Green
} else {
    Write-Host 'OK : mise à jour terminée (données du jeu intactes).' -ForegroundColor Green
}
