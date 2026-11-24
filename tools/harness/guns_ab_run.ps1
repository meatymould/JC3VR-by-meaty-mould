param([string]$Name = "gunsab", [switch]$SkipLaunch)
# Unattended 2D A/B of moving the game's gun to the drawn hand (vr_weapons).
# The debug pose's alternate drops the right hand low and outward, away from
# where the game's aim animation holds the gun, so in screenshots the muzzle
# flash shows which gun fired: ON should flash at the drawn (low, outward)
# gun, OFF at the animated one. Also runs the probe for the barrel axis.
$ErrorActionPreference = 'Stop'
$s = $PSScriptRoot
$d = "$env:LOCALAPPDATA\JC3VR"
$log = "$d\bootstrap_worker.log"
function Wait([double]$seconds) { $w = (Get-Date).AddSeconds($seconds); while ((Get-Date) -lt $w) {} }
if (-not $SkipLaunch) { & "$s\auto_world.ps1" -Name $Name -Set @('vr_body_debug_pose=1') -MenuSeconds 200 | Select-Object -Last 1 }
& "$s\ensure_world.ps1"
$mark = (Get-Content $log).Count
New-Item -ItemType File "$d\jc3vr_debug_pose_alt" -Force | Out-Null
Wait 2
# ON (default): fire, probe, screenshots mid-burst.
Set-Content "$d\jc3vr_pad" "RT 5000" -NoNewline -Encoding ascii
Wait 1.2
New-Item -ItemType File "$d\jc3vr_weapon_probe" -Force | Out-Null
Wait 0.6
& "$s\shot.ps1" -Name "$Name-on-1"
Wait 0.4
& "$s\shot.ps1" -Name "$Name-on-2"
Wait 4
# OFF: same again.
New-Item -ItemType File "$d\jc3vr_guns_toggle" -Force | Out-Null
Wait 1
Set-Content "$d\jc3vr_pad" "RT 5000" -NoNewline -Encoding ascii
Wait 1.8
& "$s\shot.ps1" -Name "$Name-off-1"
Wait 0.4
& "$s\shot.ps1" -Name "$Name-off-2"
Wait 4
New-Item -ItemType File "$d\jc3vr_guns_toggle" -Force | Out-Null
Wait 1
Get-Content $log | Select-Object -Skip $mark | Select-String "weapon probe|guns_toggle|VR weapons" | ForEach-Object { $_.Line.Substring(15) } | Select-Object -First 30
