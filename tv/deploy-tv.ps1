# Développement : signe l'application TV, l'installe sur la télévision par sdb, puis la lance.
# Prérequis : kit Tizen en ligne de commande, télévision en mode développeur, et le certificat
# avec lequel l'application est déjà installée (sinon la télévision refuse la mise à jour).
# Exemple : .\deploy-tv.ps1 -Tv 192.168.x.y -Tizen C:\tizen-studio
param(
    # Adresse IP du téléviseur sur le réseau local (Paramètres > Réseau du téléviseur).
    [Parameter(Mandatory)][string]$Tv,
    # Dossier du kit Tizen (Tizen Studio ou sa version en ligne de commande).
    [string]$Tizen = $(if ($env:TIZEN_STUDIO) { $env:TIZEN_STUDIO } else { Join-Path $env:USERPROFILE 'tizen-studio' }),
    # Certificat de signature : celui avec lequel l'application est déjà installée. Par défaut, le
    # certificat fourni avec Apps2Samsung ; un certificat Samsung à votre nom convient aussi.
    [string]$CertDir = 'C:\Program Files\Apps2Samsung\Assets\Certificate\Jellyfin',
    [string]$Profile = '36Screenlink'
)
$ErrorActionPreference = 'Stop'
$cli = Join-Path $Tizen 'tools\ide\bin\tizen.bat'
$sdb = Join-Path $Tizen 'tools\sdb.exe'
$build = Join-Path $env:TEMP '36screenlink-tv'

# Profil de signature, créé une fois. Le mot de passe n'est jamais affiché.
$profiles = & $cli security-profiles list 2>&1 | Out-String
if ($profiles -notmatch [regex]::Escape($Profile)) {
    $password = (Get-Content (Join-Path $CertDir 'password.txt') -Raw).Trim()
    & $cli security-profiles add -n $Profile -a (Join-Path $CertDir 'author.p12') -p $password `
        -d (Join-Path $CertDir 'distributor.p12') -dp $password | Out-Null
    if ($LASTEXITCODE) { throw 'Profil de signature' }
}

# Copie de travail : seuls les fichiers de l'application, comme dans build-tv.ps1.
Remove-Item $build -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory "$build\js" | Out-Null
New-Item -ItemType Directory "$build\wasm" | Out-Null
foreach ($f in 'config.xml', 'index.html', 'icon.png', 'js\bytes.js', 'js\secure.js', 'js\app.js', 'wasm\player.js', 'wasm\player.wasm') {
    Copy-Item (Join-Path $PSScriptRoot $f) (Join-Path $build $f)
}
& $cli package -t wgt -s $Profile -- $build | Select-Object -Last 2
if ($LASTEXITCODE) { throw 'Signature du paquet' }
$wgt = Get-ChildItem $build -Filter *.wgt | Select-Object -First 1

& $sdb connect $Tv | Out-Null
$target = "$($Tv):26101"
& $cli install -n $wgt.Name -s $target -- $build | Select-Object -Last 3
if ($LASTEXITCODE) { throw 'Installation sur la télévision' }
& $cli run -p S36LinkTV1.Screenlink -s $target | Select-Object -Last 1
