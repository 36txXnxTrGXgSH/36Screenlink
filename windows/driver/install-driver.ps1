#Requires -RunAsAdministrator
# À lancer dans un PowerShell administrateur. Installe le pilote d'écran virtuel.
#
# Aucun certificat n'est distribué avec le projet. Ce script en crée un, propre à ce PC,
# signe le pilote, enregistre la partie publique, puis détruit la clé privée : personne,
# ni l'auteur du projet ni ce PC, ne peut ensuite signer autre chose avec ce certificat.
# Annulation complète : uninstall-driver.ps1
$ErrorActionPreference = 'Stop'

# Paquet publié : sous-dossier driver. Sources : dossier de construction.
$package = if (Test-Path (Join-Path $PSScriptRoot 'driver\36ScreenlinkDriver.inf')) { Join-Path $PSScriptRoot 'driver' } else { Join-Path $PSScriptRoot 'dist' }
$stage = Join-Path $env:ProgramData '36Screenlink-driver'
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory $stage | Out-Null
foreach ($file in '36ScreenlinkDriver.inf', '36ScreenlinkDriver.dll', '36ScreenlinkDriver.cat') {
    Copy-Item (Join-Path $package $file) $stage
}

$subject = 'CN=36Screenlink (local)'
$cert = New-SelfSignedCertificate -Subject $subject -Type CodeSigningCert -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 `
    -CertStoreLocation Cert:\LocalMachine\My -NotAfter (Get-Date).AddYears(20) -KeyExportPolicy NonExportable
$public = Join-Path $stage 'local.cer'
try {
    Export-Certificate -Cert $cert -FilePath $public | Out-Null
    Import-Certificate -FilePath $public -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
    Import-Certificate -FilePath $public -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
    foreach ($file in '36ScreenlinkDriver.dll', '36ScreenlinkDriver.cat') {
        $signature = Set-AuthenticodeSignature -Certificate $cert -FilePath (Join-Path $stage $file) -HashAlgorithm SHA256
        if ($signature.Status -ne 'Valid') { throw "Signature de $file : $($signature.Status)" }
    }
}
finally {
    # La clé privée ne survit jamais au script, même en cas d'erreur.
    Remove-Item "Cert:\LocalMachine\My\$($cert.Thumbprint)" -DeleteKey
}

pnputil /add-driver (Join-Path $stage '36ScreenlinkDriver.inf') /install
if ($LASTEXITCODE -notin 0, 259, 3010) { throw "pnputil a échoué : $LASTEXITCODE" }
Remove-Item $stage -Recurse -Force
Write-Host "`nPilote installé. L'agent crée le moniteur à la connexion de la tablette."
