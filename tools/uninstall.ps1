param([Parameter(Mandatory)][string]$GameDir)
$game = (Resolve-Path -LiteralPath $GameDir).Path
Remove-Item -LiteralPath (Join-Path $game 'dinput8.dll') -ErrorAction SilentlyContinue
Remove-Item -LiteralPath (Join-Path $game 'mods\jc3vr.dll') -ErrorAction SilentlyContinue
Write-Host 'Removed only the JC3VR deployment files.'
