param([string]$Name = "wwatch", [int]$FireMs = 6000)
# Unattended 2D weapon experiment: deploy (auto_world backs up the DLL), get
# in-world with the debug hand pose (so the carry runs without a headset),
# fire, and during the burst run the weapon probe (barrel axis vs the game's
# aim point) and the hardware watch on the gun matrix. Prints the results.
$ErrorActionPreference = 'Stop'
$s = $PSScriptRoot
$d = "$env:LOCALAPPDATA\JC3VR"
$log = "$d\bootstrap_worker.log"
& "$s\auto_world.ps1" -Name $Name -Set @('vr_body_debug_pose=1', 'guns_to_controller=0') -MenuSeconds 200 | Select-Object -Last 1
# Make sure we are in the world: rendering has started when the lighting
# layout check has run in this session.
$start = (Get-Process JustCause3).StartTime
for ($i = 0; $i -lt 6; ++$i) {
    $inWorld = Select-String -Path $log -Pattern "PerspectiveFovInverse layout check" | Where-Object { [datetime]::ParseExact($_.Line.Substring(1, 12), 'HH:mm:ss.fff', $null) -gt $start }
    if ($inWorld) { break }
    New-Item -ItemType File "$d\jc3vr_press_a" -Force | Out-Null
    $w = (Get-Date).AddSeconds(25); while ((Get-Date) -lt $w) {}
}
$mark = (Get-Content $log).Count
Set-Content "$d\jc3vr_pad" "RT $FireMs" -NoNewline -Encoding ascii
$w = (Get-Date).AddSeconds(1.2); while ((Get-Date) -lt $w) {}
New-Item -ItemType File "$d\jc3vr_weapon_probe" -Force | Out-Null
$w = (Get-Date).AddSeconds(1.3); while ((Get-Date) -lt $w) {}
New-Item -ItemType File "$d\jc3vr_weapon_watch" -Force | Out-Null
$w = (Get-Date).AddSeconds(4); while ((Get-Date) -lt $w) {}
& "$s\shot.ps1" -Name "$Name-fire"
Get-Content $log | Select-Object -Skip $mark | Select-String "weapon probe|weapon watch" | ForEach-Object { $_.Line.Substring(15) } | Select-Object -First 60
