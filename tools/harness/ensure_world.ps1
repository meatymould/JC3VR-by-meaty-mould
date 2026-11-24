param([int]$Tries = 6)
# Gets JC3 from the main menu into the world for unattended runs. The menu is
# recognised on a screenshot by the red "3" of the JUST CAUSE 3 logo (right
# of centre, upper third, on the 960x540 shot); while it is there, press the
# virtual pad's A (Continue) and wait for the load. Prints "in world" or
# "still in menu".
$s = $PSScriptRoot
$d = "$env:LOCALAPPDATA\JC3VR"
Add-Type -AssemblyName System.Drawing
function InMenu {
    $png = & "$s\shot.ps1" -Name "ensure-world"
    $bitmap = [System.Drawing.Bitmap]::FromFile($png)
    try {
        $red = 0
        $sx = $bitmap.Width / 960.0; $sy = $bitmap.Height / 540.0
        for ($x = 895; $x -lt 945; $x += 2) {
            for ($y = 160; $y -lt 200; $y += 2) {
                $c = $bitmap.GetPixel([int]($x * $sx), [int]($y * $sy))
                if ($c.R -gt 180 -and $c.G -lt 70 -and $c.B -lt 70) { ++$red }
            }
        }
        return $red -gt 20
    } finally { $bitmap.Dispose() }
}
if (-not (Get-Process JustCause3 -ErrorAction SilentlyContinue)) { "game not running"; return }
for ($i = 0; $i -lt $Tries; ++$i) {
    if (-not (InMenu)) { "in world"; return }
    for ($k = 0; $k -lt 3; ++$k) {
        New-Item -ItemType File "$d\jc3vr_press_a" -Force | Out-Null
        $w = (Get-Date).AddSeconds(4); while ((Get-Date) -lt $w) {}
    }
    $w = (Get-Date).AddSeconds(40); while ((Get-Date) -lt $w) {}
}
if (InMenu) { "still in menu" } else { "in world" }
