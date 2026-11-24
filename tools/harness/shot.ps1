param([string]$Name = 'shot')
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
$b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
$out = if ($env:JC3VR_HARNESS_OUT) { $env:JC3VR_HARNESS_OUT } else { Join-Path $env:TEMP 'jc3vr-harness' }
New-Item -ItemType Directory -Force $out | Out-Null
$p = Join-Path $out "$Name.png"
(New-Object System.Drawing.Bitmap $bmp, ($b.Width / 2), ($b.Height / 2)).Save($p)
$p
