# Where a throughput run's CPU goes: CPU seconds per process (console hosts, moss, shells, splat) from now until the
# next bench result lands. Run it alongside run.ps1:   powershell -File tools/bench/cpu.ps1
$results = Join-Path $PSScriptRoot "results.txt"
$before = (Get-Content $results).Count
$first = @{}
$last = @{}
$names = 'conhost', 'OpenConsole', 'moss', 'clodw', 'clod', 'cmd', 'splat', 'WindowsTerminal'
for ($i = 0; $i -lt 3000 -and (Get-Content $results).Count -le $before; $i++) {
  foreach ($p in (Get-Process -Name $names -ErrorAction SilentlyContinue)) {
    $k = "$($p.ProcessName) $($p.Id)"
    $t = $p.TotalProcessorTime.TotalSeconds
    if (!$first.ContainsKey($k)) { $first[$k] = $t }
    $last[$k] = $t
  }
  Start-Sleep -Milliseconds 100
}
foreach ($k in ($last.Keys | Sort-Object)) {
  $d = $last[$k] - $first[$k]
  if ($d -gt 0.1) { "{0,-28} {1,6:N2} s cpu" -f $k, $d }
}
