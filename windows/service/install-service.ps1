#Requires -RunAsAdministrator
# À lancer dans un PowerShell administrateur. Installe un service optionnel qui exécute
# l'agent sous le compte SYSTEM, pour atteindre l'écran de verrouillage, l'invite UAC et
# les fenêtres administrateur. Retrait complet : uninstall-service.ps1
$ErrorActionPreference = 'Stop'

Write-Host @'
ATTENTION : avec ce service, toute personne tenant la tablette appairée, application
ouverte, peut valider une invite UAC et agir sur ce PC même verrouillé.
Protégez la tablette par un code. Sans ce service, l'application reste utilisable.
'@
if ((Read-Host 'Tapez OUI pour continuer') -cne 'OUI') { Write-Host 'Installation annulée.'; exit 1 }

# Paquet publié : fichiers à côté du script. Sources : dossier de construction.
$source = if (Test-Path (Join-Path $PSScriptRoot '36Screenlink.exe')) { $PSScriptRoot } else { Join-Path $PSScriptRoot '..\..\dist\36Screenlink' }
$target = Join-Path $env:ProgramFiles '36Screenlink'
$data = Join-Path $env:ProgramData '36Screenlink'

function Invoke-Icacls {
    # Une erreur d'ACL laisserait la clé lisible par tous : elle doit arrêter l'installation.
    & icacls @args | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "icacls a échoué : $args" }
}

if (Get-Service 36Screenlink -ErrorAction SilentlyContinue) {
    Stop-Service 36Screenlink -Force
    Start-Sleep 2
}
Get-Process 36ScreenlinkAgent -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep 1

# Program Files n'est modifiable que par les administrateurs : indispensable pour un binaire SYSTEM.
New-Item -ItemType Directory -Force $target | Out-Null
foreach ($file in '36Screenlink.exe', '36ScreenlinkService.exe', '36ScreenlinkDevice.exe') {
    Copy-Item (Join-Path $source $file) $target -Force
}
foreach ($file in 'adb.exe', 'AdbWinApi.dll', 'AdbWinUsbApi.dll') {
    $path = Join-Path $source $file
    if (Test-Path $path) { Copy-Item $path $target -Force }
}

# SID et non noms : ces derniers changent avec la langue de Windows.
# Clé réservée à SYSTEM (S-1-5-18) et aux administrateurs (S-1-5-32-544).
New-Item -ItemType Directory -Force "$data\logs" | Out-Null
Invoke-Icacls $data /inheritance:r /grant:r '*S-1-5-18:(OI)(CI)F' '*S-1-5-32-544:(OI)(CI)F'
Invoke-Icacls "$data\logs" /grant '*S-1-5-32-545:(OI)(CI)RX'
$acl = (Get-Acl $data).Access | Where-Object { $_.IdentityReference.Translate([Security.Principal.SecurityIdentifier]).Value -notin 'S-1-5-18', 'S-1-5-32-544' }
if ($acl) { throw "Droits inattendus sur $data" }

# Toute clé précédente a pu être exposée : elle est remplacée par un nouvel appairage.
Remove-Item "$data\pairing.bin", "$data\tablet.txt" -Force -ErrorAction SilentlyContinue
Write-Host "`nBranchez la tablette en USB, ouvrez l'application, puis comparez le code."
& "$target\36Screenlink.exe" --pair --system-store
if ($LASTEXITCODE -ne 0) { throw 'Appairage annulé ou impossible : service non démarré.' }

if (-not (Get-Service 36Screenlink -ErrorAction SilentlyContinue)) {
    New-Service -Name 36Screenlink -DisplayName '36Screenlink' -StartupType Automatic `
        -BinaryPathName "`"$target\36ScreenlinkService.exe`"" -Description 'Écran étendu sur tablette Android' | Out-Null
}
& sc.exe failure 36Screenlink reset= 86400 actions= restart/5000/restart/5000/restart/30000 | Out-Null
Start-Service 36Screenlink
Get-Service 36Screenlink | Select-Object Name, Status, StartType
