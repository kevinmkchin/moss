# The window path, checked without getting in the way: starts `moss --test` (its own throwaway server) in a
# window off screen that never takes the foreground (MOSS_TEST_WINDOW), types into it with posted messages,
# captures it with PrintWindow, and closes it.
#   powershell -File tools/window_test.ps1 [out.png] [text to type]
param([string]$Out = "window.png", [string]$Text = "echo window-ok", [int]$LatencyKeys = 0, [string]$Clicks = "", [string]$Command = "", [int]$Wait = 0)
#   -LatencyKeys N: then N key presses 80 ms apart (run with MOSS_LATENCY=1, read with tools/latency.py)
#   -Clicks "x,y;x,y": then left clicks at those client points, 1 s apart (e.g. the face button)
#   -Command NAME: runs that command (`dump commands` names, e.g. web_page) before the clicks; -Wait MS: then waits
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class W {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr c);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string title);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  public struct RECT { public int L, T, R, B; }
}
"@
[W]::SetProcessDPIAware() | Out-Null
[W]::SetThreadDpiAwarenessContext([IntPtr](-4)) | Out-Null   # per monitor v2: posted mouse positions aren't rescaled
$exe = Join-Path $PSScriptRoot "..\build\moss.exe"
$env:MOSS_TEST_WINDOW = "-3000,100,1200,760"
if ($Command) { $env:MOSS_TEST_COMMAND = $Command }
$p = Start-Process -FilePath $exe -ArgumentList "--test" -PassThru
$hwnd = [IntPtr]::Zero
for ($i = 0; $i -lt 100 -and $hwnd -eq [IntPtr]::Zero; $i++) {
  Start-Sleep -Milliseconds 100
  $p.Refresh()
  $hwnd = $p.MainWindowHandle
}
if ($hwnd -eq [IntPtr]::Zero) { "no window"; exit 1 }
Start-Sleep -Milliseconds 2500
foreach ($c in $Text.ToCharArray()) { [W]::PostMessageW($hwnd, 0x0102, [IntPtr][int]$c, [IntPtr]1) | Out-Null; Start-Sleep -Milliseconds 15 }
Start-Sleep -Milliseconds 200
[W]::PostMessageW($hwnd, 0x0100, [IntPtr]0x0D, [IntPtr]1) | Out-Null   # WM_KEYDOWN Enter (TranslateMessage makes its WM_CHAR)
Start-Sleep -Milliseconds 1500
for ($i = 0; $i -lt $LatencyKeys; $i++) {
  [W]::PostMessageW($hwnd, 0x0100, [IntPtr]0x58, [IntPtr]1) | Out-Null          # WM_KEYDOWN X
  [W]::PostMessageW($hwnd, 0x0101, [IntPtr]0x58, [IntPtr]0xC0000001) | Out-Null # WM_KEYUP X
  Start-Sleep -Milliseconds 80
}
if ($Command) { [W]::PostMessageW($hwnd, 0x8003, [IntPtr]0, [IntPtr]0) | Out-Null }   # WM_APP + 3: MOSS_TEST_COMMAND
Start-Sleep -Milliseconds $Wait
foreach ($c in ($Clicks -split ";")) {
  if (!$c) { continue }
  $xy = $c -split ","
  $l = [IntPtr](([int]$xy[1] -shl 16) -bor ([int]$xy[0] -band 0xFFFF))
  [W]::PostMessageW($hwnd, 0x0200, [IntPtr]0, $l) | Out-Null   # WM_MOUSEMOVE
  Start-Sleep -Milliseconds 50
  [W]::PostMessageW($hwnd, 0x0201, [IntPtr]1, $l) | Out-Null   # WM_LBUTTONDOWN
  Start-Sleep -Milliseconds 50
  [W]::PostMessageW($hwnd, 0x0202, [IntPtr]0, $l) | Out-Null   # WM_LBUTTONUP
  Start-Sleep -Milliseconds 1000
}
Start-Sleep -Milliseconds 300
$r = New-Object W+RECT
[W]::GetWindowRect($hwnd, [ref]$r) | Out-Null
$bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$dc = $g.GetHdc()
[W]::PrintWindow($hwnd, $dc, 2) | Out-Null
$g.ReleaseHdc($dc)
$bmp.Save($Out)
"captured $Out ($($r.R - $r.L) x $($r.B - $r.T)), pid $($p.Id)"
[W]::PostMessageW($hwnd, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null   # WM_CLOSE
Start-Sleep -Milliseconds 1500
if (!$p.HasExited) { "still running after WM_CLOSE"; exit 1 }
"closed"
