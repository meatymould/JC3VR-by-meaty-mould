param([string]$Name = "gdump", [switch]$SkipLaunch)
# Unattended 2D numbers for the gun move (vr_weapons): with the move ON and
# then OFF, fire and take the one-shot placement dump (jc3vr_guns_dump) plus
# a screenshot mid-burst. Debug hand pose so the carry runs without a headset.
$ErrorActionPreference = 'Stop'
$h = $PSScriptRoot
$d = "$env:LOCALAPPDATA\JC3VR"
$log = "$d\bootstrap_worker.log"
function Wait([double]$t) { $w = (Get-Date).AddSeconds($t); while ((Get-Date) -lt $w) {} }
if (-not $SkipLaunch) { & "$h\auto_world.ps1" -Name $Name -Set @('vr_body_debug_pose=1') -MenuSeconds 200 | Select-Object -Last 1 }
$state = & "$h\ensure_world.ps1"
$state
if ($state -ne "in world") { return }
$mark = (Get-Content $log).Count
foreach ($phase in 'on', 'off') {
    Set-Content "$d\jc3vr_pad" "RT 4000" -NoNewline -Encoding ascii
    Wait 1.5
    New-Item -ItemType File "$d\jc3vr_guns_dump" -Force | Out-Null
    Wait 0.8
    & "$h\shot.ps1" -Name "$Name-$phase" | Out-Null
    Wait 3
    New-Item -ItemType File "$d\jc3vr_guns_toggle" -Force | Out-Null
    Wait 1
}
Get-Content $log | Select-Object -Skip $mark | Select-String "guns dump|guns_toggle" | ForEach-Object { $_.Line.Substring(15) }
