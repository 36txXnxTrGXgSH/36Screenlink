#Requires -RunAsAdministrator
# À lancer dans un PowerShell administrateur. Retire le pilote et ses certificats.
$ErrorActionPreference = 'Continue'

$packages = pnputil /enum-drivers | Out-String
foreach ($block in ($packages -split "(\r?\n){2,}")) {
    if ($block -match '(?i)36screenlinkdriver\.inf' -and $block -match '(oem\d+\.inf)') {
        pnputil /delete-driver $Matches[1] /uninstall /force
    }
}

# Certificats de ce projet uniquement, y compris celui des anciennes versions.
foreach ($store in 'Root', 'TrustedPublisher') {
    Get-ChildItem "Cert:\LocalMachine\$store" |
        Where-Object { $_.Subject -in 'CN=36Screenlink (local)', 'CN=36Screenlink Dev' } |
        Remove-Item
}
Write-Host 'Pilote et certificats retirés.'
