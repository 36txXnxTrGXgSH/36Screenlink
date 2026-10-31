# Compile le pilote et le programme de périphérique, crée le paquet non signé dans dist\.
# La signature se fait sur le PC de destination, voir install-driver.ps1.
# Prérequis : Visual Studio 2022 ou Build Tools avec C++, Windows SDK 10.0.26100.
# Le WDK est téléchargé via NuGet ; $env:WDK_ROOT évite ce téléchargement.
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$sdkVer = '10.0.26100.0'
$wdkPackage = '10.0.26100.6584'
$iddVer = '1.4'
$umdfVer = '2.27'

$kits = (Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots').KitsRoot10.TrimEnd('\')
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Outils C++ de Visual Studio introuvables.' }
$msvcVer = (Get-Content "$vs\VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt").Trim()
$msvc = "$vs\VC\Tools\MSVC\$msvcVer"

$wdk = $env:WDK_ROOT
if (-not $wdk) {
    $cache = Join-Path $here "build\wdk-$wdkPackage"
    if (-not (Test-Path $cache)) {
        New-Item -ItemType Directory -Force (Split-Path $cache) | Out-Null
        $archive = "$cache.zip"
        $ProgressPreference = 'SilentlyContinue'
        Invoke-WebRequest "https://api.nuget.org/v3-flatcontainer/microsoft.windows.wdk.x64/$wdkPackage/microsoft.windows.wdk.x64.$wdkPackage.nupkg" -OutFile $archive -UseBasicParsing
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        [IO.Compression.ZipFile]::ExtractToDirectory($archive, $cache)
        Remove-Item $archive
    }
    $wdk = "$cache\c"
}

$build = "$here\build"
$dist = "$here\dist"
New-Item -ItemType Directory -Force $build | Out-Null
Remove-Item $dist -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory $dist | Out-Null

$env:INCLUDE = @(
    "$msvc\include", "$kits\Include\$sdkVer\ucrt", "$kits\Include\$sdkVer\um", "$kits\Include\$sdkVer\shared",
    "$kits\Include\$sdkVer\winrt", "$kits\Include\$sdkVer\cppwinrt",
    "$wdk\Include\$sdkVer\um", "$wdk\Include\$sdkVer\shared", "$wdk\Include\$sdkVer\um\iddcx\$iddVer", "$wdk\Include\wdf\umdf\$umdfVer"
) -join ';'
$env:LIB = @(
    "$msvc\lib\x64", "$kits\Lib\$sdkVer\ucrt\x64", "$kits\Lib\$sdkVer\um\x64",
    "$wdk\Lib\$sdkVer\um\x64", "$wdk\Lib\$sdkVer\um\x64\iddcx\$iddVer", "$wdk\Lib\wdf\umdf\x64\$umdfVer"
) -join ';'

$env:PATH = "$kits\bin\$sdkVer\x64;$env:PATH"
$cl = "$msvc\bin\Hostx64\x64\cl.exe"
$defs = '/DUNICODE','/D_UNICODE','/D_AMD64_','/DAMD64','/DUMDF_DRIVER','/DIDDCX_VERSION_MAJOR=1','/DIDDCX_VERSION_MINOR=4',
    '/DIDDCX_MINIMUM_VERSION_REQUIRED=4','/DUMDF_VERSION_MAJOR=2','/DUMDF_VERSION_MINOR=27','/D_WIN32_WINNT=0x0A00','/DWINVER=0x0A00','/DNTDDI_VERSION=0x0A00000A'

& $cl /nologo /c /std:c++17 /O2 /EHsc /MT /W3 /wd4005 /utf-8 /Zc:__cplusplus /GS /guard:cf @defs "/Fo$build\Driver.obj" "$here\src\Driver.cpp"
if ($LASTEXITCODE) { throw 'Compilation du pilote' }

& "$msvc\bin\Hostx64\x64\link.exe" /nologo /DLL /OUT:"$build\36ScreenlinkDriver.dll" /SUBSYSTEM:WINDOWS /GUARD:CF /CETCOMPAT `
    /EXPORT:FxDriverEntryUm "$build\Driver.obj" WdfDriverStubUm.lib IddCxStub.lib d3d11.lib dxgi.lib avrt.lib user32.lib kernel32.lib ole32.lib ntdll.lib
if ($LASTEXITCODE) { throw 'Édition de liens du pilote' }
Copy-Item "$build\36ScreenlinkDriver.dll" $dist

& $cl /nologo /std:c++17 /O2 /EHsc /MT /W3 /utf-8 /DUNICODE /D_UNICODE "/Fo$build\Device.obj" "/Fe$build\36ScreenlinkDevice.exe" "$here\device\36ScreenlinkDevice.cpp" `
    /link /MANIFEST:EMBED /MANIFESTUAC:"level='requireAdministrator'" advapi32.lib
if ($LASTEXITCODE) { throw 'Compilation du programme de périphérique' }

Copy-Item "$here\src\36ScreenlinkDriver.inx" "$dist\36ScreenlinkDriver.inf" -Force
& "$wdk\bin\$sdkVer\x64\stampinf.exe" -f "$dist\36ScreenlinkDriver.inf" -a amd64 -d * -v 1.0.0.0
if ($LASTEXITCODE) { throw 'stampinf' }

& "$wdk\bin\$sdkVer\x86\Inf2Cat.exe" /driver:"$dist" /os:10_x64
if ($LASTEXITCODE) { throw 'Inf2Cat' }
Rename-Item "$dist\36screenlinkdriver.cat" '36ScreenlinkDriver.cat' -ErrorAction SilentlyContinue
Get-ChildItem $dist | Select-Object Name, Length
