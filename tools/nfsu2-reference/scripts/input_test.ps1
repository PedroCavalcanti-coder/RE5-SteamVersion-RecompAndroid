# Automated input test: start the game (recompiled by default, or any exe via
# -Exe), bring its window to the foreground, press keys, and stop it.
#   powershell -File scripts/input_test.ps1 [-Exe path] [-Keys 0x0D,0x0D] [-StartDelay 12] [-Total 40]
param([string]$Exe = "", [int[]]$Keys = @(0x0D, 0x0D, 0x0D, 0x1B), [int]$StartDelay = 12, [int]$Total = 40, [string]$Args = "")
$root = Split-Path -Parent $PSScriptRoot
if (-not $Exe) { $Exe = Join-Path $root "build\NFSU2-Recompiled.exe" }
$wd = if ($Exe -like "*NFSU2-Recompiled*") { $root } else { Split-Path -Parent $Exe }
Add-Type @"
using System; using System.Runtime.InteropServices;
public class K {
 [DllImport("user32.dll")] public static extern IntPtr FindWindow(string c, string t);
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
 [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
 [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
 public static void Tap(byte vk, int ms) { byte sc = (byte)MapVirtualKey(vk, 0);
   keybd_event(vk, sc, 0, UIntPtr.Zero); System.Threading.Thread.Sleep(ms); keybd_event(vk, sc, 2, UIntPtr.Zero); }
}
"@
$p = if ($Args) { Start-Process -FilePath $Exe -ArgumentList $Args -WorkingDirectory $wd -PassThru } else { Start-Process -FilePath $Exe -WorkingDirectory $wd -PassThru }
Start-Sleep -Seconds $StartDelay
$h = [K]::FindWindow("GameFrame", [NullString]::Value)
[K]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero); [K]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)   # ALT: lets SetForegroundWindow succeed
$ok = [K]::SetForegroundWindow($h)
"window=$h foreground_set=$ok isfg=$([K]::GetForegroundWindow() -eq $h) t=$(Get-Date -Format HH:mm:ss.fff)"
Start-Sleep -Seconds 2
foreach ($k in $Keys) { "key 0x{0:X2} at {1}" -f $k, (Get-Date -Format HH:mm:ss.fff); [K]::Tap([byte]$k, 120); Start-Sleep -Seconds 3 }
Start-Sleep -Seconds ([Math]::Max(1, $Total - $StartDelay - 2 - 3 * $Keys.Count))
Get-Process NFSU2-Recompiled, SPEED2 -ErrorAction SilentlyContinue | Stop-Process -Force
