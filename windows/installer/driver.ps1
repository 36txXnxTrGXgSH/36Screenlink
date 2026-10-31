# Installe ou retire le pilote d'écran. Exige les droits administrateur.
# Appelé par l'installateur et, en version portable, par 36ScreenlinkDevice.exe.
#
# Un certificat propre à ce PC signe le paquet, puis sa clé privée est détruite : personne
# ne peut ensuite signer autre chose avec lui. Aucun certificat commun n'est distribué.
param([ValidateSet('Install', 'Remove')][string]$Action = 'Install', [switch]$Force)
$ErrorActionPreference = 'Stop'
$subject = 'CN=36Screenlink (local)'

function Get-DriverPackages {
    $enumerated = pnputil /enum-drivers | Out-String
    foreach ($block in ($enumerated -split "(\r?\n){2,}")) {
        if ($block -match '(?i)36screenlinkdriver\.inf' -and $block -match '(oem\d+\.inf)') { $Matches[1] }
    }
}

function Remove-Driver {
    foreach ($package in Get-DriverPackages) { pnputil /delete-driver $package /uninstall /force | Out-Null }
    foreach ($store in 'Root', 'TrustedPublisher') {
        Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object Subject -eq $subject | Remove-Item
    }
}

if ($Action -eq 'Remove') {
    Remove-Driver
    exit 0
}
if (-not $Force -and (Get-DriverPackages)) { exit 0 }
Remove-Driver

$stage = Join-Path $env:ProgramData '36Screenlink-stage'
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory $stage | Out-Null
Copy-Item "$PSScriptRoot\driver\*" $stage

$cert = New-SelfSignedCertificate -Subject $subject -Type CodeSigningCert -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 `
    -CertStoreLocation Cert:\LocalMachine\My -NotAfter (Get-Date).AddYears(20) -KeyExportPolicy NonExportable
try {
    $public = Join-Path $stage 'local.cer'
    Export-Certificate -Cert $cert -FilePath $public | Out-Null
    Import-Certificate -FilePath $public -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
    Import-Certificate -FilePath $public -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
    foreach ($file in '36ScreenlinkDriver.dll', '36ScreenlinkDriver.cat') {
        $signature = Set-AuthenticodeSignature -Certificate $cert -FilePath (Join-Path $stage $file) -HashAlgorithm SHA256
        if ($signature.Status -ne 'Valid') { throw "Signature de $file : $($signature.Status)" }
    }
}
finally {
    # La clé privée ne survit jamais à ce script, même en cas d'erreur.
    Remove-Item "Cert:\LocalMachine\My\$($cert.Thumbprint)" -DeleteKey
}

pnputil /add-driver (Join-Path $stage '36ScreenlinkDriver.inf') /install
if ($LASTEXITCODE -notin 0, 259, 3010) { throw "pnputil a échoué : $LASTEXITCODE" }
Remove-Item $stage -Recurse -Force
