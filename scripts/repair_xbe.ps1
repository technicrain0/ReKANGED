# Undoes the one byte patches some xiso tools make to default.xbe (the media check bypass), when that gives back the
# exact xbe this port is built from. The file is only written when the result's SHA-1 is the expected one; the patched
# file is kept beside it as default.xbe.xiso. Exit code 0 when repaired, 1 otherwise.
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\repair_xbe.ps1 -Xbe assets\default.xbe -Sha1 <sha1>
param(
    [Parameter(Mandatory = $true)][string]$Xbe,
    [Parameter(Mandatory = $true)][string]$Sha1
)
$ErrorActionPreference = 'Stop'

# file offset, patched byte, original byte
$Known = @(
    @{ Offset = 0x1DBFB9; Patched = 0xEB; Original = 0x7D }  # True Crime: Streets of LA (PAL) xiso: jmp for jge
)

$bytes = [System.IO.File]::ReadAllBytes($Xbe)
$hasher = [System.Security.Cryptography.SHA1]::Create()
function Hex([byte[]]$data) { ([BitConverter]::ToString($hasher.ComputeHash($data)) -replace '-', '').ToLower() }

foreach ($p in $Known) {
    if ($p.Offset -ge $bytes.Length -or $bytes[$p.Offset] -ne $p.Patched) { continue }
    $bytes[$p.Offset] = [byte]$p.Original
    if ((Hex $bytes) -eq $Sha1.ToLower()) {
        Copy-Item -LiteralPath $Xbe -Destination "$Xbe.xiso" -Force
        [System.IO.File]::WriteAllBytes($Xbe, $bytes)
        Write-Host ("      default.xbe had an xiso tool's media check patch at 0x{0:X}, put back: it is the European release" -f $p.Offset)
        exit 0
    }
    $bytes[$p.Offset] = [byte]$p.Patched
}
exit 1
