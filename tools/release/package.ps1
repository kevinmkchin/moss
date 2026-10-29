# Builds a release of moss and everything a winget submission needs, in dist\ (never committed):
#   dist\moss-<version>-win-x64.zip      moss.exe, the console host, WebView2's loader, clink, the default shell setup, the licenses
#   dist\winget\manifests\n\NoclipSoftware\Moss\<version>\*.yaml   the three manifests, the zip's hash filled in
# The version comes from code\moss_version.h; the zip's URL is where the GitHub release will put it.
#   powershell -File tools/release/package.ps1 [-Owner kevinmkchin] [-Repo moss]
param([string]$Owner = "kevinmkchin", [string]$Repo = "moss")
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$version = (Select-String -Path (Join-Path $root "code\moss_version.h") -Pattern '#define MOSS_VERSION "(.*)"').Matches[0].Groups[1].Value
$name = "moss-$version-win-x64"
$dist = Join-Path $root "dist"
$stage = Join-Path $dist $name
$zip = Join-Path $dist "$name.zip"
Write-Host "moss $version"

# --- a release build: optimized, no internal checks, logs in %LOCALAPPDATA%\moss\logs ---
& cmd /c "`"$root\build.bat`" release"
if ($LASTEXITCODE -ne 0) { throw "build failed" }

# --- what ships ---
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null
# moss.exe from the build; the rest from the repo itself (build\ may keep files the repo has dropped)
Copy-Item (Join-Path $root "build\moss.exe") $stage
foreach ($f in "conpty.dll", "OpenConsole.exe") { Copy-Item (Join-Path $root "vendor\conpty\$f") $stage }
Copy-Item (Join-Path $root "vendor\conpty\LICENSE") (Join-Path $stage "conpty-LICENSE.txt")
Copy-Item (Join-Path $root "vendor\webview2\WebView2Loader.dll") $stage
Copy-Item (Join-Path $root "vendor\webview2\LICENSE") (Join-Path $stage "webview2-LICENSE.txt")
Copy-Item -Recurse (Join-Path $root "vendor\clink") (Join-Path $stage "clink")
Copy-Item -Recurse (Join-Path $root "vendor\clink-completions") (Join-Path $stage "clink-completions")
Copy-Item -Recurse (Join-Path $root "clink-moss") (Join-Path $stage "clink-moss")
Copy-Item -Recurse (Join-Path $root "shell") (Join-Path $stage "shell")
Copy-Item (Join-Path $root "LICENSE") (Join-Path $stage "LICENSE.txt")
Copy-Item (Join-Path $PSScriptRoot "THIRD-PARTY.txt") $stage
if (Test-Path $zip) { Remove-Item -Force $zip }
# tar (Windows' bsdtar) writes standard zips; PowerShell 5's Compress-Archive puts backslashes in the paths
$items = Get-ChildItem $stage | ForEach-Object { $_.Name }
& (Join-Path $env:SystemRoot "System32\tar.exe") -a -c -f $zip -C $stage @items
if ($LASTEXITCODE -ne 0) { throw "zip failed" }
$sha = (Get-FileHash -Algorithm SHA256 $zip).Hash
Write-Host "$zip"
Write-Host "sha256 $sha"

# --- the winget manifests, from the templates next to this script ---
$url = "https://github.com/$Owner/$Repo/releases/download/v$version/$name.zip"
$out = Join-Path $dist "winget\manifests\n\NoclipSoftware\Moss\$version"
New-Item -ItemType Directory -Force $out | Out-Null
foreach ($t in Get-ChildItem (Join-Path $PSScriptRoot "winget") -Filter "*.yaml") {
  $text = (Get-Content -Raw $t.FullName).Replace("{{VERSION}}", $version).Replace("{{URL}}", $url).Replace("{{SHA256}}", $sha).
          Replace("{{OWNER}}", $Owner).Replace("{{REPO}}", $Repo).Replace("{{DATE}}", (Get-Date -Format "yyyy-MM-dd"))
  [IO.File]::WriteAllText((Join-Path $out $t.Name), $text)
}
Write-Host "winget manifests in $out"
Write-Host "check them: winget validate --manifest `"$out`"   (and try it: winget install --manifest `"$out`")"
