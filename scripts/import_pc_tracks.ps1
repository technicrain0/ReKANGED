# Imports the PC version's 32 extra soundtrack songs (084 to 115) into the port, from a PC install the player has.
#   Data\Music\084.bik ... 115.bik  ->  assets\extras\Data\Music\084.WMA ... (the Xbox's song format, with ffmpeg)
#   Data\Shell\TCPC??.txt           ->  assets\extras\pc_tracks.txt (the songs' lines from the PC's track list)
# assets\extras lies over the game's D:, so this works with an .iso as well as with extracted files. Nothing of the
# game is changed, and nothing here ships with the port: it all comes from the player's own PC copy.
param([string]$PcDir = "", [string]$Assets = "")

$ErrorActionPreference = "Stop"
$latin1 = [Text.Encoding]::GetEncoding(28591)  # the language files' 8-bit text, byte for byte

function Fail($msg) {
    Write-Host ""
    Write-Host "  $msg" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "  True Crime: Streets of LA - import the PC soundtrack"
Write-Host "  ===================================================="
Write-Host ""

if (-not $Assets) { $Assets = Join-Path (Split-Path $PSScriptRoot -Parent) "assets" }
if (-not (Test-Path -LiteralPath $Assets -PathType Container)) { Fail "The assets folder was not found: $Assets" }

if (-not $PcDir) {
    Write-Host "  Drag your PC True Crime: Streets of LA folder (the one with TrueCrime.exe) into this window,"
    $PcDir = Read-Host "  then press Enter"
}
$PcDir = $PcDir.Trim().Trim('"')
$music = Join-Path $PcDir "Data\Music"
if (-not (Test-Path -LiteralPath (Join-Path $music "084.bik"))) {
    Fail "No Data\Music\084.bik in $PcDir. Pick the PC version's folder, the one with TrueCrime.exe and Data."
}

# ffmpeg: beside the port, on PATH, or where winget puts it
$ffmpeg = $null
$root = Split-Path $PSScriptRoot -Parent
foreach ($c in @((Join-Path $root "ffmpeg.exe"), (Join-Path $root "compiler\ffmpeg.exe"),
                 (Join-Path $env:LOCALAPPDATA "Microsoft\WinGet\Links\ffmpeg.exe"))) {
    if (Test-Path -LiteralPath $c) { $ffmpeg = $c; break }
}
if (-not $ffmpeg) {
    $cmd = Get-Command ffmpeg -ErrorAction SilentlyContinue
    if ($cmd) { $ffmpeg = $cmd.Source }
}
if (-not $ffmpeg) {
    $pkgs = Join-Path $env:LOCALAPPDATA "Microsoft\WinGet\Packages"
    if (Test-Path -LiteralPath $pkgs) {
        $found = Get-ChildItem -LiteralPath $pkgs -Recurse -Filter ffmpeg.exe -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($found) { $ffmpeg = $found.FullName }
    }
}
if (-not $ffmpeg) {
    Fail "ffmpeg was not found. Install it (in a command prompt: winget install ffmpeg), then run this again."
}
Write-Host "  ffmpeg: $ffmpeg"

# the songs' lines from the PC's track list: "084Them Bones|Alice In Chains"
$shell = Join-Path $PcDir "Data\Shell"
$list = @("TCPCUK.txt", "TCPCUS.txt") | ForEach-Object { Join-Path $shell $_ } |
        Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $list) {
    $any = Get-ChildItem -LiteralPath $shell -Filter "TCPC*.txt" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($any) { $list = $any.FullName }
}
if (-not $list) { Fail "No track list (Data\Shell\TCPCUK.txt) in $PcDir." }
$lines = @{}
foreach ($line in ($latin1.GetString([IO.File]::ReadAllBytes($list)) -split "`n")) {
    $line = $line.TrimEnd("`r", " ")
    if ($line -match '^(\d{3})(.+\|.+)$') {
        $n = [int]$Matches[1]
        if ($n -ge 84 -and $n -le 127 -and -not $lines.ContainsKey($n)) { $lines[$n] = $line }
    }
}
if ($lines.Count -eq 0) { Fail "The PC track list has no songs past 083 in it." }

$out = Join-Path $Assets "extras\Data\Music"
New-Item -ItemType Directory -Force -Path $out | Out-Null
$done = @()
foreach ($n in ($lines.Keys | Sort-Object)) {
    $name = "{0:D3}" -f $n
    $src = Join-Path $music "$name.bik"
    $dst = Join-Path $out "$name.WMA"
    if (-not (Test-Path -LiteralPath $src)) { Write-Host "  $name.bik is missing, skipped"; continue }
    $title = ($lines[$n].Substring(3) -split '\|')[0]
    if ((Test-Path -LiteralPath $dst) -and (Get-Item -LiteralPath $dst).Length -gt 0) {
        Write-Host "  $name  $title (already imported)"
    } else {
        Write-Host "  $name  $title"
        & $ffmpeg -hide_banner -loglevel error -y -i $src -vn -c:a wmav2 -b:a 128k -ar 44100 -ac 2 $dst
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $dst)) {
            Write-Host "    converting failed, skipped" -ForegroundColor Yellow
            Remove-Item -LiteralPath $dst -ErrorAction SilentlyContinue
            continue
        }
    }
    $done += $lines[$n]
}
if ($done.Count -eq 0) { Fail "No song was imported." }
[IO.File]::WriteAllBytes((Join-Path $Assets "extras\pc_tracks.txt"), $latin1.GetBytes(($done -join "`r`n") + "`r`n"))

Write-Host ""
Write-Host "  Done: $($done.Count) PC tracks imported into $Assets\extras" -ForegroundColor Green
Write-Host "  They play while cruising and are listed under SETTINGS > AUDIO > SOUNDTRACK > CRUISING,"
Write-Host "  below the console's tracks. Delete the extras folder to remove them."
exit 0
