# Paquet de l'application TV : dist\release\36Screenlink-TV.wgt, non signé.
# Apps2Samsung le signe avec le certificat Samsung lié à votre télévision, puis l'installe.
param([string]$Out = (Join-Path (Split-Path $PSScriptRoot) 'dist\release'))
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
New-Item -ItemType Directory -Force $Out | Out-Null
$wgt = Join-Path $Out '36Screenlink-TV.wgt'
if (Test-Path $wgt) {
    try { Remove-Item $wgt -ErrorAction Stop } catch { throw "$wgt est ouvert par un autre programme (Apps2Samsung ?) : fermez-le ou choisissez un autre dossier avec -Out." }
}
# Liste blanche : rien d'autre n'entre dans le paquet.
$files = 'config.xml', 'index.html', 'icon.png', 'js\bytes.js', 'js\secure.js', 'js\app.js', 'wasm\player.js', 'wasm\player.wasm'
$zip = [IO.Compression.ZipFile]::Open($wgt, 'Create')
try {
    foreach ($f in $files) {
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, (Join-Path $PSScriptRoot $f), $f.Replace('\', '/'))
    }
} finally {
    $zip.Dispose()
}
Get-Item $wgt | Select-Object Name, Length
