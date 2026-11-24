# Copies only default.xbe out of an Xbox disc image (.iso) into a folder, nothing else.
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\get_xbe.ps1 -Iso assets\game.iso -Out assets
param(
    [Parameter(Mandatory = $true)][string]$Iso,
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Name = 'default.xbe'
)
$ErrorActionPreference = 'Stop'
$SectorSize = 2048
$Magic = 'MICROSOFT*XBOX*MEDIA'
$stream = [System.IO.File]::Open($Iso, 'Open', 'Read', 'Read')
try {
    function Read-At([int64]$at, [int]$len) {
        $buf = New-Object byte[] $len
        $stream.Position = $at
        $got = 0
        while ($got -lt $len) { $n = $stream.Read($buf, $got, $len - $got); if ($n -le 0) { break }; $got += $n }
        if ($got -ne $len) { throw "the image ends early at $at" }
        return ,$buf
    }
    $base = -1
    foreach ($p in @([int64]0, [int64]0x18300000)) {
        if ($stream.Length -lt $p + 33 * $SectorSize) { continue }
        $vol = Read-At ($p + 32 * $SectorSize) $SectorSize
        if ([Text.Encoding]::ASCII.GetString($vol, 0, 20) -eq $Magic -and [Text.Encoding]::ASCII.GetString($vol, 0x7EC, 20) -eq $Magic) {
            $base = $p; $rootSector = [BitConverter]::ToUInt32($vol, 0x14); $rootSize = [BitConverter]::ToUInt32($vol, 0x18); break
        }
    }
    if ($base -lt 0) { throw "$Iso is no Xbox disc image" }
    $table = Read-At ($base + [int64]$rootSector * $SectorSize) ([int]$rootSize)
    $todo = New-Object System.Collections.Generic.Stack[int]; $todo.Push(0); $seen = @{}
    while ($todo.Count -gt 0) {
        $at = $todo.Pop()
        if ($at + 0xE -gt $rootSize -or $seen.ContainsKey($at)) { continue }
        $seen[$at] = $true
        $left = [BitConverter]::ToUInt16($table, $at); $right = [BitConverter]::ToUInt16($table, $at + 2)
        if ($left -eq 0xFFFF) { continue }
        if ($left) { $todo.Push([int]$left * 4) }; if ($right) { $todo.Push([int]$right * 4) }
        $nameLen = $table[$at + 0xD]
        $entry = [Text.Encoding]::ASCII.GetString($table, $at + 0xE, $nameLen)
        if ($entry -ieq $Name) {
            $start = [BitConverter]::ToUInt32($table, $at + 4); $len = [BitConverter]::ToUInt32($table, $at + 8)
            New-Item -ItemType Directory -Force -Path $Out | Out-Null
            $path = Join-Path $Out $Name
            [IO.File]::WriteAllBytes($path, (Read-At ($base + [int64]$start * $SectorSize) ([int]$len)))
            $sha = (Get-FileHash -Algorithm SHA1 -LiteralPath $path).Hash.ToLower()
            Write-Host "      $path ($len bytes)"
            Write-Host "      sha1 $sha"
            exit 0
        }
    }
    throw "$Name is not in the root of $Iso"
} finally { $stream.Dispose() }
