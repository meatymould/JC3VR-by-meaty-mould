param([Parameter(Mandatory)][string]$Executable)
$resolved = (Resolve-Path -LiteralPath $Executable).Path
$bytes = [IO.File]::ReadAllBytes($resolved)
$sha = (Get-FileHash -LiteralPath $resolved -Algorithm SHA256).Hash
$reader = [BitConverter]::ToInt32($bytes, 0x3c)
$timestamp = [BitConverter]::ToUInt32($bytes, $reader + 8)
$imageSize = [BitConverter]::ToUInt32($bytes, $reader + 24 + 56)
[pscustomobject]@{ Path = $resolved; Size = $bytes.Length; Sha256 = $sha; PeTimestamp = ('0x{0:X8}' -f $timestamp); ImageSize = ('0x{0:X8}' -f $imageSize) } | Format-List
