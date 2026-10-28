# Terminal throughput bench: opens a terminal window on screen (1280 x 800), has it run bench.bat (refterm's
# splat.exe streaming a 64 MiB file through ConPTY), waits for the result line, captures the window, closes it.
#   powershell -File tools/bench/run.ps1 -Terminal moss|clodw|wt|refterm -File color|plain -Splat C:\path\splat.exe
# refterm also runs splat straight from its own prompt (its fast pipe, no ConPTY): read that from the capture.
# Data and captures go to %TEMP%\term-bench; results.txt next to this script; summarize.py prints MB/s.
param([string]$Terminal = "moss", [string]$File = "color", [string]$Splat = "", [string]$Refterm = "", [string]$Clodw = "C:\UnitySrc\w\clod\build\clodw.exe")
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class B {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int w, int hh, uint f);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  public struct RECT { public int L, T, R, B; }
}
"@
[B]::SetProcessDPIAware() | Out-Null
$bench = $PSScriptRoot
$data = Join-Path $env:TEMP "term-bench"
New-Item -ItemType Directory -Force $data | Out-Null
$target = Join-Path $data "$File.txt"
if (!(Test-Path $target)) { python (Join-Path $bench "gen.py") $data | Out-Null }
$results = Join-Path $bench "results.txt"
if (!(Test-Path $results)) { New-Item -ItemType File $results | Out-Null }
$before = (Get-Content $results).Count
$line = "`"$bench\bench.bat`" $Terminal `"$target`" `"$Splat`""

function Type-Line($hwnd, $text) {
  foreach ($c in $text.ToCharArray()) { [B]::PostMessageW($hwnd, 0x0102, [IntPtr][int]$c, [IntPtr]1) | Out-Null; Start-Sleep -Milliseconds 5 }
  Start-Sleep -Milliseconds 100
  # Enter: a key press for the terminals that translate keys, a carriage return character for refterm (its loop reads
  # WM_CHAR and never translates WM_KEYDOWN)
  if ($Terminal -eq "refterm") { [B]::PostMessageW($hwnd, 0x0102, [IntPtr]0x0D, [IntPtr]1) | Out-Null }
  else { [B]::PostMessageW($hwnd, 0x0100, [IntPtr]0x0D, [IntPtr]1) | Out-Null }
}
function Wait-Result($timeout) {
  for ($i = 0; $i -lt $timeout * 10; $i++) { if ((Get-Content $results).Count -gt $before) { return $true }; Start-Sleep -Milliseconds 100 }
  return $false
}
function Capture($hwnd, $name) {
  $r = New-Object B+RECT
  [B]::GetWindowRect($hwnd, [ref]$r) | Out-Null
  $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  # what's on screen there (the window is in front): PrintWindow can't read every renderer's window
  $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
  $file = Join-Path $data "$name.png"
  $bmp.Save($file)
  "captured $file"
}

if ($Terminal -eq "wt") {
  # Windows Terminal: a new window running the bench, which closes when cmd exits
  Start-Process wt.exe -ArgumentList "-w new --pos 100,100 --size 128,40 cmd /c $line"
  if (!(Wait-Result 300)) { "timed out"; exit 1 }
  Get-Content $results | Select-Object -Last 1
  exit 0
}
$exe = @{ moss = (Join-Path $bench "..\..\build\moss.exe"); clodw = $Clodw; refterm = $Refterm }[$Terminal]
if ($Terminal -eq "refterm") {
  # refterm has no shell and no ConPTY: it runs NAME.exe from its working folder over plain pipes (or its fast pipe),
  # so it starts in the data folder with splat.exe next to the files
  Copy-Item $Splat (Join-Path $data "splat.exe") -Force
  $p = Start-Process -FilePath $exe -WorkingDirectory $data -PassThru
} else {
  $p = Start-Process -FilePath $exe -ArgumentList "--test" -PassThru
}
$hwnd = [IntPtr]::Zero
for ($i = 0; $i -lt 100 -and $hwnd -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 100; $p.Refresh(); $hwnd = $p.MainWindowHandle }
if ($hwnd -eq [IntPtr]::Zero) { "no window"; exit 1 }
[B]::SetWindowPos($hwnd, [IntPtr]::Zero, 100, 100, 1280, 800, 0x0004) | Out-Null   # SWP_NOZORDER
Start-Sleep -Seconds 4   # the shell's first prompt
if ($Terminal -eq "clodw") {
  # clodw opens on clod's home screen, where letters are commands (n starts a claude session!): t opens a shell tab
  [B]::PostMessageW($hwnd, 0x0102, [IntPtr][int][char]'t', [IntPtr]1) | Out-Null
  Start-Sleep -Seconds 4
}
if ($Terminal -eq "refterm") {
  # splat over refterm's plain pipes, then over its fast pipe; splat prints its own sink time (read the captures)
  Type-Line $hwnd "splat $File.txt"
  Start-Sleep -Seconds 25
  Capture $hwnd "refterm-pipe-$File"
  Type-Line $hwnd "fastpipe"
  Start-Sleep -Seconds 1
  Type-Line $hwnd "splat $File.txt"
  Start-Sleep -Seconds 25
  Capture $hwnd "refterm-fastpipe-$File"
  [B]::PostMessageW($hwnd, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null   # WM_CLOSE
  Start-Sleep -Seconds 1
  if (!$p.HasExited) { $p.Kill() }
  exit 0
}
Type-Line $hwnd $line
$ok = Wait-Result 300
Start-Sleep -Milliseconds 500
Capture $hwnd "$Terminal-$File"
[B]::PostMessageW($hwnd, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null   # WM_CLOSE
Start-Sleep -Seconds 1
if (!$p.HasExited) { $p.Kill() }
if (!$ok) { "timed out"; exit 1 }
Get-Content $results | Select-Object -Last 1
