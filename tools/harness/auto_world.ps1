param([string]$Name, [string[]]$Set = @(), [string]$Base = "jc3vr-direct-headlook.ini", [int]$SettleSeconds = 80, [int]$MenuSeconds = 150)
# Unattended: deploy profile + overrides, launch JC3 via Steam (Dropzone VFS),
# get through LOW MEMORY/CONTINUE with JC3VR's virtual-pad A (trigger file),
# settle in-world, screenshot, and print key trace lines.
$ErrorActionPreference = 'Stop'
$r = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$g = 'D:\SteamLibrary\steamapps\common\Just Cause 3'
$s = $PSScriptRoot
$log = "$env:LOCALAPPDATA\JC3VR\bootstrap_worker.log"
$trigger = "$env:LOCALAPPDATA\JC3VR\jc3vr_press_a"
$p = Get-Process JustCause3 -ErrorAction SilentlyContinue
if ($p) { $null = $p.CloseMainWindow(); if (-not $p.WaitForExit(25000)) { Stop-Process -Id $p.Id -Force; $p.WaitForExit(10000) | Out-Null } }
# Steam needs a moment to register the exit, or the relaunch fails with
# "Game already running" (seen 2026-10-01).
if ($p) { $w = (Get-Date).AddSeconds(15); while ((Get-Date) -lt $w) {} }
$ini = Get-Content "$r\profiles\$Base" -Raw
foreach ($pair in $Set) {
    $key = $pair.Split('=')[0]
    if ($ini -match "(?m)^$key=") { $ini = $ini -replace "(?m)^$key=.*$", $pair } else { $ini = $ini.TrimEnd() + "`n$pair`n" }
}
Set-Content "$g\jc3vr.ini" $ini -NoNewline -Encoding ascii
$bk = "$g\backups\auto-harness"; New-Item -ItemType Directory -Force $bk | Out-Null; $h = (Get-FileHash "$g\mods\jc3vr.dll").Hash.Substring(0,12); if (-not (Test-Path "$bk\jc3vr.dll.$h")) { Copy-Item "$g\mods\jc3vr.dll" "$bk\jc3vr.dll.$h" }; Copy-Item "$r\build\jc3vr.dll" "$g\mods\jc3vr.dll" -Force
$start = Get-Date
Start-Process -FilePath 'c:/program files (x86)/steam/steam.exe' -ArgumentList '-applaunch 225540 --vfs-fs dropzone --vfs-archive patch_win64 --vfs-archive archives_win64 --vfs-fs .'
$w = (Get-Date).AddSeconds($MenuSeconds); while ((Get-Date) -lt $w) { Start-Sleep 5; & "$s\dismiss.ps1" }
for ($i = 0; $i -lt 4; ++$i) { & "$s\dismiss.ps1"
    New-Item -ItemType File -Path $trigger -Force | Out-Null
    $w = (Get-Date).AddSeconds(6); while ((Get-Date) -lt $w) {}
}
$w = (Get-Date).AddSeconds($SettleSeconds); while ((Get-Date) -lt $w) {}
"$Name pid=$((Get-Process JustCause3 -ErrorAction SilentlyContinue).Id) overrides=$($Set -join ' ')"
& "$s\shot.ps1" -Name $Name
