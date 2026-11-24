# Close a "SteamVR initialization failed!" message box (it blocks unattended
# 2D runs when SteamVR is mid-shutdown), then give JC3 the foreground.
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class D {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string t);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
}
'@
$n = 0
while ($true) {
    $h = [D]::FindWindow($null, 'SteamVR initialization failed!')
    if ($h -eq [IntPtr]::Zero) { break }
    [D]::PostMessage($h, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 500; ++$n; if ($n -gt 5) { break }
}
$p = Get-Process JustCause3 -ErrorAction SilentlyContinue
if ($p) { [D]::SetForegroundWindow($p.MainWindowHandle) | Out-Null }
if ($n) { "dismissed $n dialog(s)" }
