#Requires -RunAsAdministrator
# À lancer vous-même dans un PowerShell administrateur. Retire le service, ses fichiers et sa clé.
$ErrorActionPreference = 'Continue'
if (Get-Service 36Screenlink -ErrorAction SilentlyContinue) {
    Stop-Service 36Screenlink -Force
    Start-Sleep 2
    sc.exe delete 36Screenlink | Out-Null
}
Get-Process 36ScreenlinkAgent, 36ScreenlinkDevice -ErrorAction SilentlyContinue | Stop-Process -Force
Remove-Item (Join-Path $env:ProgramFiles '36Screenlink') -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $env:ProgramData '36Screenlink') -Recurse -Force -ErrorAction SilentlyContinue
Write-Host 'Service retiré. Le pilote et son certificat restent en place : voir uninstall-driver.ps1.'
