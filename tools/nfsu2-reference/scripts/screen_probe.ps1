# Screenshot probe: start an exe, optionally press keys at given seconds, and
# save a small grayscale-average signature of the screen every second.
#   powershell -File scripts/screen_probe.ps1 -Exe X -Out dir -Seconds 40 -KeyAt 14,17,20 [-Key 0x0D]
param([string]$Exe, [string]$Out, [int]$Seconds = 40, [string]$KeyAt = "", [int]$Key = 0x0D, [string]$WorkDir = "")
$KeyAt = @($KeyAt -split "[, ]+" | Where-Object { $_ } | ForEach-Object { [int]$_ })
Add-Type -AssemblyName System.Drawing, System.Windows.Forms
Add-Type @"
using System; using System.Runtime.InteropServices;
public class K2 {
 [DllImport("user32.dll")] public static extern IntPtr FindWindow(string c, string t);
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
 [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
 public static void Tap(byte vk) { byte sc = (byte)MapVirtualKey(vk, 0);
   keybd_event(vk, sc, 0, UIntPtr.Zero); System.Threading.Thread.Sleep(120); keybd_event(vk, sc, 2, UIntPtr.Zero); }
}
"@
New-Item -ItemType Directory -Force $Out | Out-Null
if (-not $WorkDir) { $WorkDir = Split-Path -Parent $Exe }
$p = Start-Process -FilePath $Exe -WorkingDirectory $WorkDir -PassThru
$b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$sig = @()
for ($t = 0; $t -lt $Seconds; $t++) {
  Start-Sleep -Milliseconds 1000
  if ($t -eq 8) { $h = [K2]::FindWindow("GameFrame", [NullString]::Value); [K2]::keybd_event(0x12,0,0,[UIntPtr]::Zero); [K2]::keybd_event(0x12,0,2,[UIntPtr]::Zero); [K2]::SetForegroundWindow($h) | Out-Null }
  if ($KeyAt -contains $t) { [K2]::Tap([byte]$Key) }
  $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
  $g = [System.Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
  $small = New-Object System.Drawing.Bitmap $bmp, 320, 180
  $sum = 0; for ($y = 0; $y -lt 180; $y += 10) { for ($x = 0; $x -lt 320; $x += 10) { $c = $small.GetPixel($x, $y); $sum += $c.R + $c.G + $c.B } }
  $small.Save((Join-Path $Out ("t{0:D2}.png" -f $t)))
  $sig += ("t={0,2} key={1} mean={2,4}" -f $t, ($KeyAt -contains $t), [int]($sum / (32*18*3)))
  $g.Dispose(); $bmp.Dispose(); $small.Dispose()
}
Get-Process NFSU2-Recompiled, SPEED2 -ErrorAction SilentlyContinue | Stop-Process -Force
$sig
