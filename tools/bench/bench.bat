@echo off
rem One throughput run in whatever terminal this runs in: refterm's splat.exe streams FILE to the terminal; the
rem times before and after go to results.txt (the writer blocks while the terminal is behind, so the elapsed time is
rem the terminal's).   bench.bat NAME FILE SPLAT_EXE
set T0=%time%
"%~3" "%~2"
set T1=%time%
echo %~1 %~nx2 %T0% %T1%>> "%~dp0results.txt"
