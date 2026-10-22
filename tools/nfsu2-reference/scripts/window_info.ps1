# Start the recompiled game, report its window geometry/style after N seconds, stop it.
param([int]$After = 20, [string]$Args = "")
$root = Split-Path -Parent $PSScriptRoot
Add-Type @"
using System; using System.Runtime.InteropServices;
public class W4 { [StructLayout(LayoutKind.Sequential)] public struct R { public int l,t,r,b; }
 [DllImport("user32.dll")] public static extern IntPtr FindWindow(string c, string t);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
 [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out R r);
 [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr h, int i); }
"@
$exe = Join-Path $root "build\NFSU2-Recompiled.exe"
$p = if ($Args) { Start-Process $exe -ArgumentList $Args -WorkingDirectory $root -PassThru } else { Start-Process $exe -WorkingDirectory $root -PassThru }
Start-Sleep -Seconds $After
$h = [W4]::FindWindow("GameFrame", [NullString]::Value)
$r = New-Object W4+R; [void][W4]::GetWindowRect($h, [ref]$r)
$c = New-Object W4+R; [void][W4]::GetClientRect($h, [ref]$c)
"hwnd=$h window=($($r.l),$($r.t))-($($r.r),$($r.b)) client=$($c.r)x$($c.b) style=0x{0:X8} exstyle=0x{1:X8}" -f [W4]::GetWindowLong($h, -16), [W4]::GetWindowLong($h, -20)
Get-Process NFSU2-Recompiled -ErrorAction SilentlyContinue | Stop-Process -Force
