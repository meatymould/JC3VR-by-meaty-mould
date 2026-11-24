param([Parameter(Mandatory=$true)][string]$Output)
# Read-only single-game-window fallback. PrintWindow can return black for DX11;
# do not treat such a capture as evidence about the rendered game.
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Jc3WindowShot {
    [StructLayout(LayoutKind.Sequential)] public struct Rect {public int l,t,r,b;}
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out Rect r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h,IntPtr dc,uint flags);
}
'@
$game=Get-Process JustCause3 -ErrorAction Stop | Select-Object -First 1
$rect=New-Object Jc3WindowShot+Rect
if(!$game.MainWindowHandle -or ![Jc3WindowShot]::GetWindowRect($game.MainWindowHandle,[ref]$rect)) {throw 'No JC3 window'}
$w=$rect.r-$rect.l;$h=$rect.b-$rect.t
if($w -lt 1 -or $h -lt 1 -or $w -gt 8192 -or $h -gt 8192) {throw 'Invalid window bounds'}
$bitmap=New-Object System.Drawing.Bitmap $w,$h
$graphics=[System.Drawing.Graphics]::FromImage($bitmap)
$dc=$graphics.GetHdc()
try {$captured=[Jc3WindowShot]::PrintWindow($game.MainWindowHandle,$dc,2)} finally {$graphics.ReleaseHdc($dc)}
try {
    if(!$captured) {throw 'PrintWindow failed'}
    $bitmap.Save($Output,[System.Drawing.Imaging.ImageFormat]::Png)
    Write-Output ("JC3 window capture: {0}x{1} {2}" -f $w,$h,$Output)
} finally {$graphics.Dispose();$bitmap.Dispose()}
