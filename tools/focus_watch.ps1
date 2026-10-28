# Watches the foreground window while something runs (tests must never take the user's keyboard focus):
#   powershell -File tools/focus_watch.ps1 -Seconds 120
# Prints every change of the foreground window: time, process, title.
param([int]$Seconds = 60)
Add-Type @"
using System; using System.Runtime.InteropServices; using System.Text;
public class F {
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
}
"@
$last = [IntPtr]::Zero
$end = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $end) {
  $h = [F]::GetForegroundWindow()
  if ($h -ne $last) {
    $procId = 0
    [F]::GetWindowThreadProcessId($h, [ref]$procId) | Out-Null
    $sb = New-Object System.Text.StringBuilder 256
    [F]::GetWindowTextW($h, $sb, 256) | Out-Null
    $name = (Get-Process -Id $procId -ErrorAction SilentlyContinue).ProcessName
    Write-Output ("{0:HH:mm:ss.fff}  {1}  {2}  '{3}'" -f (Get-Date), $procId, $name, $sb.ToString())
    $last = $h
  }
  Start-Sleep -Milliseconds 30
}
