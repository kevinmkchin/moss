@echo off
setlocal
rem moss: builds build\moss.exe (window, server, headless runner) with MSVC. Run from any shell.
rem   build          optimized, internal checks on (asserts, logs next to the exe)
rem   build debug    unoptimized
rem   build release  optimized, no internal checks (logs in %LOCALAPPDATA%\term\logs)
rem   build content  optimized, then the website's content\ (screenshots, text, icon); see CLAUDE.md

where cl >nul 2>nul
if not errorlevel 1 goto :build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul

:build
set "ROOT=%~dp0"
if not exist "%ROOT%build" mkdir "%ROOT%build"
pushd "%ROOT%build"

set INTERNAL=1
set OPT=/O2 /MT
if "%1"=="debug" set OPT=/Od /MTd
if "%1"=="release" set INTERNAL=0
set COMMON=/nologo /utf-8 /W4 /wd4100 /wd4505 /wd4201 /Zi /FC /DUNICODE /D_UNICODE /DTERM_INTERNAL=%INTERNAL% %OPT%

rem --- icon: drawn by tools\make_icon.cpp, then compiled into a resource for the exe ---
cl /nologo /std:c++20 /EHsc /O2 /MT "%ROOT%tools\make_icon.cpp" /Fe:make_icon.exe /link /INCREMENTAL:NO user32.lib gdi32.lib
if errorlevel 1 (set ERR=1& goto :done)
"%ROOT%build\make_icon.exe" moss.ico "%ROOT%vendor\stix\STIXTwoText-SemiBold.otf"
if errorlevel 1 (set ERR=1& goto :done)
rc /nologo /i "%ROOT%build" /i "%ROOT%code" /fo moss.res "%ROOT%code\term.rc"
if errorlevel 1 (set ERR=1& goto :done)

rem --- what the tabs use, next to moss.exe: clink (vendor\clink, GPL-3.0, a separate program run in each
rem --- tab's cmd), clink-completions (MIT), the default shell setup ---
xcopy /e /i /y /q /d "%ROOT%vendor\clink" clink >nul
if errorlevel 1 (set ERR=1& goto :done)
xcopy /e /i /y /q /d "%ROOT%vendor\clink-completions" clink-completions >nul
if errorlevel 1 (set ERR=1& goto :done)
xcopy /e /i /y /q /d "%ROOT%shell" shell >nul
if errorlevel 1 (set ERR=1& goto :done)
rem --- the console host the tabs run in: conpty.dll + OpenConsole.exe (vendor\conpty, MIT). Copied only when newer:
rem --- running tabs hold OpenConsole.exe ---
xcopy /y /q /d "%ROOT%vendor\conpty\conpty.dll" . >nul
xcopy /y /q /d "%ROOT%vendor\conpty\OpenConsole.exe" . >nul
copy /y "%ROOT%vendor\conpty\LICENSE" conpty-LICENSE.txt >nul

rem --- the two unity builds: C (UI, renderer, app, platform) and C++ (server, client, cells, fonts) ---
cl %COMMON% /std:c11 /c "%ROOT%code\term_c.c" /Fo:term_c.obj /Fd:moss.pdb
if errorlevel 1 (set ERR=1& goto :done)
cl %COMMON% /std:c++20 /EHsc /c "%ROOT%code\term_cxx.cpp" /Fo:term_cxx.obj /Fd:moss.pdb
if errorlevel 1 (set ERR=1& goto :done)

rem --- (link by full path: Git for Windows' link.exe can shadow it on PATH)
rem --- a running moss locks moss.exe, but Windows lets it be renamed: move it aside so the new build ---
rem --- lands while the old one keeps running. Old copies go once nothing uses them. ---
del /q moss.old-*.exe >nul 2>nul
if exist moss.exe del /q moss.exe >nul 2>nul
if exist moss.exe move /y moss.exe moss.old-%RANDOM%.exe >nul

"%VCToolsInstallDir%bin\Hostx64\x64\link.exe" /nologo /DEBUG /INCREMENTAL:NO /OPT:REF /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTINPUT:"%ROOT%code\term.manifest" /OUT:moss.exe /PDB:moss.pdb ^
    term_c.obj term_cxx.obj moss.res ^
    kernel32.lib user32.lib shell32.lib advapi32.lib gdi32.lib ole32.lib windowscodecs.lib shlwapi.lib d3d11.lib dxgi.lib ^
    d3dcompiler.lib d2d1.lib dwrite.lib dcomp.lib dwmapi.lib comdlg32.lib uuid.lib imm32.lib dbghelp.lib
set ERR=%errorlevel%
if not "%ERR%"=="0" goto :done
if not "%1"=="content" goto :done

rem --- build content: the website's screenshots of this build (headless, made-up sessions and files), its
rem --- text and the icon, into content\ for the site generator (C:\UnitySrc\kevin.gd\build.py) ---
python "%ROOT%tools\site\capture.py"
if errorlevel 1 (set ERR=1& goto :done)
python "%ROOT%tools\site\content.py"
set ERR=%errorlevel%

:done
popd
exit /b %ERR%
