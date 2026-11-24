param([string]$Key = 'Enter', [int]$Count = 1, [int]$HoldMs = 120, [int]$GapMs = 1500)
# Focus JC3 and send scan-code key presses (SendInput), for unattended 2D runs.
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class K {
  [StructLayout(LayoutKind.Sequential)] public struct KI { public ushort vk; public ushort scan; public uint flags; public uint time; public IntPtr extra; }
  [StructLayout(LayoutKind.Explicit, Size=40)] public struct IN { [FieldOffset(0)] public uint type; [FieldOffset(8)] public KI ki; }
  [DllImport("user32.dll", SetLastError=true)] public static extern uint SendInput(uint n, IN[] i, int cb);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  public static void Scan(ushort sc, bool up, bool ext) { var a = new IN[1]; a[0].type = 1; a[0].ki.scan = sc; a[0].ki.flags = 0x8u | (up ? 0x2u : 0u) | (ext ? 0x1u : 0u); if(SendInput(1, a, Marshal.SizeOf(typeof(IN)))!=1) throw new InvalidOperationException("Game key injection failed, error="+Marshal.GetLastWin32Error()); }
}
'@
$codes = @{ Enter = 0x1C; Esc = 0x01; Space = 0x39; W = 0x11; A = 0x1E; S = 0x1F; D = 0x20; E = 0x12; F = 0x21; R = 0x13; Q = 0x10; Tab = 0x0F; '1' = 0x02; '2' = 0x03; '3' = 0x04; '4' = 0x05; F3 = 0x3D; F8 = 0x42; Up = 0x48; Down = 0x50 }
$ext = @('Up', 'Down')
$p = Get-Process JustCause3 -ErrorAction Stop
if (@($p).Count -ne 1) {throw 'Expected exactly one JC3 process'}
if (!$codes.ContainsKey($Key)) {throw 'Unsupported game key'}
[K]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 300
for ($i = 0; $i -lt $Count; ++$i) {
    if ([K]::GetForegroundWindow() -ne $p.MainWindowHandle) {throw 'JC3 is not foreground; no input sent'}
    [K]::Scan([uint16]$codes[$Key], $false, $ext -contains $Key); Start-Sleep -Milliseconds $HoldMs
    [K]::Scan([uint16]$codes[$Key], $true, $ext -contains $Key); Start-Sleep -Milliseconds $GapMs
}
Write-Output "Game-only key $Key accepted by Windows ($Count press)"
