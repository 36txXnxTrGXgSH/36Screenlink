# Lancé par l'installateur, avec les droits administrateur déjà validés.
param([switch]$SecureScreens, [string]$PlatformTools)
$ErrorActionPreference = 'Stop'
$app = $PSScriptRoot
$data = Join-Path $env:ProgramData '36Screenlink'
$serviceName = '36Screenlink'

function Invoke-Icacls {
    # Une erreur d'ACL laisserait la clé lisible par tous : elle doit arrêter l'installation.
    & icacls @args | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "icacls a échoué : $args" }
}

New-Item -ItemType Directory -Force "$data\logs" | Out-Null
# SID et non noms : ces derniers changent avec la langue de Windows.
# Clé réservée à SYSTEM (S-1-5-18) et aux administrateurs (S-1-5-32-544), journal lisible.
Invoke-Icacls $data /inheritance:r /grant:r '*S-1-5-18:(OI)(CI)F' '*S-1-5-32-544:(OI)(CI)F'
Invoke-Icacls "$data\logs" /grant '*S-1-5-32-545:(OI)(CI)RX'
Start-Transcript -Path "$data\logs\setup.log" -Force | Out-Null

try {
    if (Get-Service $serviceName -ErrorAction SilentlyContinue) {
        Stop-Service $serviceName -Force
        Start-Sleep 2
    }

    & "$app\driver.ps1" -Action Install -Force

    if ($PlatformTools -and (Test-Path $PlatformTools)) {
        $unpacked = Join-Path $data 'platform-tools-stage'
        Remove-Item $unpacked -Recurse -Force -ErrorAction SilentlyContinue
        Expand-Archive $PlatformTools $unpacked
        foreach ($file in 'adb.exe', 'AdbWinApi.dll', 'AdbWinUsbApi.dll') {
            Copy-Item "$unpacked\platform-tools\$file" $app -Force
        }
        Remove-Item $unpacked -Recurse -Force
    }

    New-Item -Path 'HKLM:\SOFTWARE\36Screenlink' -Force | Out-Null
    Set-ItemProperty 'HKLM:\SOFTWARE\36Screenlink' SecureScreens ([int][bool]$SecureScreens) -Type DWord

    if (-not (Get-Service $serviceName -ErrorAction SilentlyContinue)) {
        New-Service -Name $serviceName -DisplayName '36Screenlink' -StartupType Automatic `
            -BinaryPathName "`"$app\36ScreenlinkService.exe`"" -Description 'Écran étendu sur tablette Android' | Out-Null
    }
    & sc.exe failure $serviceName reset= 86400 actions= restart/5000/restart/5000/restart/30000 | Out-Null
    # L'utilisateur connecté peut démarrer le service depuis le menu Démarrer, sans droits admin.
    $descriptor = (& sc.exe sdshow $serviceName | Where-Object { $_.Trim() }) -join ''
    if ($descriptor -notmatch '\(A;;RPLC;;;IU\)' -and $descriptor -match '^(D:.*?)(S:.*)?$') {
        & sc.exe sdset $serviceName ($Matches[1] + '(A;;RPLC;;;IU)' + $Matches[2]) | Out-Null
    }
    Start-Service $serviceName
    Stop-Transcript | Out-Null
}
catch {
    Write-Output "ERREUR : $_"
    Stop-Transcript | Out-Null
    exit 1
}
