# Extracts an Xbox disc image (.iso) into a folder: default.xbe and the game's folders.
#
# Reads the same image kinds as the runtime (sdk\runtime\kernel\disc.cpp): an xiso (the game partition alone) or a
# whole original disc dump with the game partition at 0x18300000. The volume descriptor is sector 32 of the
# partition: "MICROSOFT*XBOX*MEDIA", the root table's sector and size, and the magic again at +0x7EC.
# A folder is a table of entries in a binary tree: +0 left and +2 right entry (in 4-byte units, 0 for none),
# +4 sector, +8 size, +0xC attributes (0x10 folder), +0xD name length, +0xE name. A table of 0xFF is empty.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File extract_iso.ps1 -Iso game.iso -Out assets
param(
    [Parameter(Mandatory = $true)][string]$Iso,
    [Parameter(Mandatory = $true)][string]$Out
)
$ErrorActionPreference = 'Stop'

$SectorSize = 2048
$Magic = 'MICROSOFT*XBOX*MEDIA'
$Partitions = @([int64]0, [int64]0x18300000)

$stream = [System.IO.File]::Open($Iso, 'Open', 'Read', 'Read')
try {
    function Read-At([int64]$at, [int]$len) {
        $buf = New-Object byte[] $len
        $stream.Position = $at
        $got = 0
        while ($got -lt $len) {
            $n = $stream.Read($buf, $got, $len - $got)
            if ($n -le 0) { break }
            $got += $n
        }
        if ($got -ne $len) { throw "the image ends early at $at" }
        return ,$buf
    }

    # the game partition
    $base = -1
    foreach ($p in $Partitions) {
        if ($stream.Length -lt $p + 33 * $SectorSize) { continue }
        $vol = Read-At ($p + 32 * $SectorSize) $SectorSize
        $a = [System.Text.Encoding]::ASCII.GetString($vol, 0, $Magic.Length)
        $b = [System.Text.Encoding]::ASCII.GetString($vol, 0x7EC, $Magic.Length)
        if ($a -eq $Magic -and $b -eq $Magic) {
            $base = $p
            $rootSector = [BitConverter]::ToUInt32($vol, 0x14)
            $rootSize = [BitConverter]::ToUInt32($vol, 0x18)
            break
        }
    }
    if ($base -lt 0) { throw "$Iso is no Xbox disc image" }

    $script:files = 0
    $script:bytes = [int64]0
    $copy = New-Object byte[] (4MB)

    function Extract-Table([int64]$tableSector, [uint32]$size, [string]$dir, [int]$depth) {
        if ($size -eq 0) { return }
        if ($depth -gt 32 -or $size -gt 16MB) { throw "a folder table under $dir cannot be read" }
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
        $table = Read-At ($base + $tableSector * $SectorSize) ([int]$size)
        $seen = @{}
        $todo = New-Object System.Collections.Generic.Stack[int]
        $todo.Push(0)
        while ($todo.Count -gt 0) {
            $at = $todo.Pop()
            if ($at + 0xE -gt $size -or $seen.ContainsKey($at)) { continue }
            $seen[$at] = $true
            $left = [BitConverter]::ToUInt16($table, $at)
            $right = [BitConverter]::ToUInt16($table, $at + 2)
            if ($left -eq 0xFFFF) { continue }
            $start = [BitConverter]::ToUInt32($table, $at + 4)
            $len = [BitConverter]::ToUInt32($table, $at + 8)
            $attr = $table[$at + 0xC]
            $nameLen = $table[$at + 0xD]
            if ($at + 0xE + $nameLen -gt $size) { continue }
            if ($left) { $todo.Push([int]$left * 4) }
            if ($right) { $todo.Push([int]$right * 4) }
            $name = [System.Text.Encoding]::ASCII.GetString($table, $at + 0xE, $nameLen)
            if ($name -eq '' -or $name -eq '.' -or $name -eq '..' -or $name.IndexOfAny([char[]]'\/:*?"<>|') -ge 0) { continue }
            $path = Join-Path $dir $name
            if ($attr -band 0x10) {
                Extract-Table $start $len $path ($depth + 1)
                continue
            }
            $outFile = [System.IO.File]::Create($path)
            try {
                $stream.Position = $base + [int64]$start * $SectorSize
                $left2 = [int64]$len
                while ($left2 -gt 0) {
                    $want = [int][Math]::Min($copy.Length, $left2)
                    $n = $stream.Read($copy, 0, $want)
                    if ($n -le 0) { throw "the image ends early in $path" }
                    $outFile.Write($copy, 0, $n)
                    $left2 -= $n
                }
            } finally { $outFile.Dispose() }
            $script:files++
            $script:bytes += $len
            if ($script:files % 200 -eq 0) {
                Write-Host ("      {0} files, {1:N0} MB ..." -f $script:files, ($script:bytes / 1MB))
            }
        }
    }

    Extract-Table $rootSector $rootSize $Out 0
    Write-Host ("      {0} files, {1:N0} MB extracted" -f $script:files, ($script:bytes / 1MB))
} finally {
    $stream.Dispose()
}
