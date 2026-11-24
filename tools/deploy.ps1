param([Parameter(Mandatory)][string]$GameDir, [string]$BuildDir = (Join-Path $PSScriptRoot '..\build'))
$game = (Resolve-Path -LiteralPath $GameDir).Path
if (-not (Test-Path (Join-Path $game 'JustCause3.exe'))) { throw 'JustCause3.exe was not found in GameDir.' }
if (Test-Path (Join-Path $game 'dinput8.dll')) { throw 'Refusing to overwrite an existing dinput8.dll. Back it up or configure proxy chaining first.' }
$proxy = Join-Path $BuildDir 'dinput8.dll'; $mod = Join-Path $BuildDir 'jc3vr.dll'
if (-not (Test-Path $proxy) -or -not (Test-Path $mod)) { throw 'Build outputs are missing. Build JC3VR first.' }
New-Item -ItemType Directory -Force (Join-Path $game 'mods') | Out-Null
Copy-Item -LiteralPath $proxy -Destination (Join-Path $game 'dinput8.dll')
Copy-Item -LiteralPath $mod -Destination (Join-Path $game 'mods\jc3vr.dll')
Write-Host 'Installed JC3VR proxy and DLL. Run fingerprint.ps1 before testing engine-specific features.'
