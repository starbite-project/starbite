# sb-click.ps1 FX FY (Track K T63): one left click at (FX, FY) of the primary screen, in physical pixels
# (DPI-aware), as a touch tap would land. Logs where it clicked.
param([double]$fx, [double]$fy)
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class SbClick {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, IntPtr e);
}
"@
[SbClick]::SetProcessDPIAware() | Out-Null
$w = [SbClick]::GetSystemMetrics(0); $h = [SbClick]::GetSystemMetrics(1)
$x = [int]($w * $fx); $y = [int]($h * $fy)
[SbClick]::SetCursorPos($x, $y) | Out-Null
Start-Sleep -Milliseconds 100
[SbClick]::mouse_event(2, 0, 0, 0, [IntPtr]::Zero)
Start-Sleep -Milliseconds 80
[SbClick]::mouse_event(4, 0, 0, 0, [IntPtr]::Zero)
"USR1 click at $x,$y of ${w}x${h}"
