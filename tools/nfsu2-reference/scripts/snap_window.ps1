# Save a PNG of the game window's content without activating it (PrintWindow
# with PW_RENDERFULLCONTENT works for D3D windows on Windows 8.1+).
#   powershell -File scripts/snap_window.ps1 -Out path.png
param([string]$Out)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class SW { [StructLayout(LayoutKind.Sequential)] public struct R { public int l,t,r,b; }
 [DllImport("user32.dll")] public static extern IntPtr FindWindow(string c, string t);
 [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out R r);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f); }
"@
$h = [SW]::FindWindow("GameFrame", [NullString]::Value)
if ($h -eq [IntPtr]::Zero) { "no window"; exit 1 }
$r = New-Object SW+R; [void][SW]::GetClientRect($h, [ref]$r)
$bmp = New-Object System.Drawing.Bitmap ([Math]::Max(1,$r.r)), ([Math]::Max(1,$r.b))
$g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
$ok = [SW]::PrintWindow($h, $dc, 3)   # PW_CLIENTONLY | PW_RENDERFULLCONTENT
$g.ReleaseHdc($dc); $bmp.Save($Out); "saved $Out ok=$ok $($r.r)x$($r.b)"
