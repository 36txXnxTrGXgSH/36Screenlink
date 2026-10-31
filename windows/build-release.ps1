# Construit tout ce qui se publie : dist\release\36Screenlink-Setup.exe et 36Screenlink.apk.
# Aucune donnée locale ne peut y entrer : l'installateur ne prend que les fichiers qu'il liste.
param([string]$Version = '1.1.0')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$out = Join-Path $root 'dist\release'
New-Item -ItemType Directory -Force $out | Out-Null

$iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe") |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw 'Inno Setup 6 introuvable.' }

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$cmake = Join-Path (& $vswhere -latest -products * -property installationPath) 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
& $cmake -S "$root\windows" -B "$root\windows\build" -G 'Visual Studio 17 2022' -A x64 | Out-Null
& $cmake --build "$root\windows\build" --config Release | Out-Null
if ($LASTEXITCODE) { throw 'Compilation de l''agent' }
& powershell -NoProfile -ExecutionPolicy Bypass -File "$root\windows\driver\build-driver.ps1" | Out-Null
if ($LASTEXITCODE) { throw 'Compilation du pilote' }

Push-Location "$root\android"
& .\gradlew.bat --no-daemon assembleRelease | Out-Null
$gradle = $LASTEXITCODE
Pop-Location
if ($gradle) { throw 'Compilation de l''application Android' }
$apk = Join-Path $root 'android\app\build\outputs\apk\release\app-release.apk'
if (-not (Test-Path $apk)) { throw 'APK signée absente : keystore.properties requis.' }
Copy-Item $apk "$out\36Screenlink.apk" -Force

& $iscc /Q "/DVersion=$Version" "$root\windows\installer\36Screenlink.iss"
if ($LASTEXITCODE) { throw 'Inno Setup' }
Get-ChildItem $out -File -Include *.exe, *.apk -Recurse -Depth 0 | Select-Object Name, Length
