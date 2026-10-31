# Lancé à la désinstallation : retire le service, le pilote, les certificats et les données.
$ErrorActionPreference = 'Continue'
$serviceName = '36Screenlink'

if (Get-Service $serviceName -ErrorAction SilentlyContinue) {
    Stop-Service $serviceName -Force
    Start-Sleep 3
    & sc.exe delete $serviceName | Out-Null
}
Get-Process 36Screenlink, 36ScreenlinkDevice, adb -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -like "$PSScriptRoot*" } | Stop-Process -Force

$enumerated = pnputil /enum-drivers | Out-String
foreach ($block in ($enumerated -split "(\r?\n){2,}")) {
    if ($block -match '(?i)36screenlinkdriver\.inf' -and $block -match '(oem\d+\.inf)') {
        pnputil /delete-driver $Matches[1] /uninstall /force | Out-Null
    }
}
foreach ($store in 'Root', 'TrustedPublisher') {
    Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object Subject -eq 'CN=36Screenlink (local)' | Remove-Item
}
Remove-Item 'HKLM:\SOFTWARE\36Screenlink' -Recurse -ErrorAction SilentlyContinue
Remove-Item (Join-Path $env:ProgramData '36Screenlink') -Recurse -Force -ErrorAction SilentlyContinue
