# Idle cost: starts `moss --test` off screen (never takes the foreground), waits for its shell to settle, then samples
# CPU time and memory of the window and its server for 30 s, and closes it.   powershell -File tools/bench/idle.ps1
$exe = Join-Path $PSScriptRoot "..\..\build\moss.exe"
$env:MOSS_TEST_WINDOW = "-3000,100,1280,800"
$before = @(Get-Process moss -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })
$p = Start-Process -FilePath $exe -ArgumentList "--test" -PassThru
Start-Sleep -Seconds 10
$mine = @(Get-Process moss | Where-Object { $before -notcontains $_.Id })
$a = @{}; foreach ($q in $mine) { $a[$q.Id] = $q.TotalProcessorTime.TotalMilliseconds }
Start-Sleep -Seconds 30
foreach ($q in $mine) {
  $q.Refresh()
  $kind = if ($q.Id -eq $p.Id) { "window" } else { "server" }
  "{0,-7} cpu {1,5:N0} ms in 30 s ({2:N2}% of one core), {3} MB working set, {4} MB private" -f $kind, ($q.TotalProcessorTime.TotalMilliseconds - $a[$q.Id]), (($q.TotalProcessorTime.TotalMilliseconds - $a[$q.Id]) / 300), [int]($q.WorkingSet64 / 1MB), [int]($q.PrivateMemorySize64 / 1MB)
}
foreach ($q in $mine) { Stop-Process -Id $q.Id -Force -ErrorAction SilentlyContinue }
