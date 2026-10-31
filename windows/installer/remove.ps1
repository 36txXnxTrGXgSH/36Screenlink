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

& "$PSScriptRoot\driver.ps1" -Action Remove
Remove-Item 'HKLM:\SOFTWARE\36Screenlink' -Recurse -ErrorAction SilentlyContinue
Remove-Item (Join-Path $env:ProgramData '36Screenlink') -Recurse -Force -ErrorAction SilentlyContinue
