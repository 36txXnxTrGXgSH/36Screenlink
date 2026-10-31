# Assemble le paquet de publication dans dist\release. Liste blanche volontaire : aucune
# donnée locale (clé d'appairage, journal, adresse) ne peut y entrer par accident.
param([string]$Version = '1.0.0')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$name = "36Screenlink-$Version"
$out = Join-Path $root "dist\release\$name"
Remove-Item (Join-Path $root 'dist\release') -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$out\driver" | Out-Null

$files = @{
    'windows\build\Release\36Screenlink.exe'   = $out
    'windows\build\Release\36ScreenlinkService.exe' = $out
    'windows\driver\build\36ScreenlinkDevice.exe'   = $out
    'windows\driver\dist\36ScreenlinkDriver.dll'    = "$out\driver"
    'windows\driver\dist\36ScreenlinkDriver.inf'    = "$out\driver"
    'windows\driver\dist\36ScreenlinkDriver.cat'    = "$out\driver"
    'windows\driver\install-driver.ps1'               = $out
    'windows\driver\uninstall-driver.ps1'             = $out
    'windows\driver\LICENSE-MS-PL.txt'                = $out
    'THIRD_PARTY_NOTICES.md'                           = $out
    'windows\service\install-service.ps1'             = $out
    'windows\service\uninstall-service.ps1'           = $out
    'windows\release\Appairer.cmd'                    = $out
    'windows\release\Demarrer-USB.cmd'                = $out
    'windows\release\Demarrer-WiFi.cmd'               = $out
    'windows\release\portable.txt'                    = $out
    'LICENSE'                                         = $out
    'README.md'                                       = $out
}
foreach ($source in $files.Keys) { Copy-Item (Join-Path $root $source) $files[$source] }

$apk = Join-Path $root 'android\app\build\outputs\apk\release\app-release.apk'
if (-not (Test-Path $apk)) { throw 'APK signée absente : voir keystore.properties.' }
Copy-Item $apk "$out\36Screenlink.apk"

$zip = "$out.zip"
Compress-Archive -Path $out -DestinationPath $zip
Get-Item $zip | Select-Object Name, Length
Get-FileHash $zip -Algorithm SHA256 | Select-Object Hash
