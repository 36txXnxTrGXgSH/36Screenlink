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

    # Pilote : un certificat propre à ce PC signe le paquet, puis sa clé privée est détruite.
    # Personne ne peut ensuite signer autre chose avec ce certificat.
    $subject = 'CN=36Screenlink (local)'
    foreach ($store in 'Root', 'TrustedPublisher') {
        Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object Subject -eq $subject | Remove-Item
    }
    $enumerated = pnputil /enum-drivers | Out-String
    foreach ($block in ($enumerated -split "(\r?\n){2,}")) {
        if ($block -match '(?i)36screenlinkdriver\.inf' -and $block -match '(oem\d+\.inf)') {
            pnputil /delete-driver $Matches[1] /uninstall /force | Out-Null
        }
    }

    $stage = Join-Path $data 'driver-stage'
    Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory $stage | Out-Null
    Copy-Item "$app\driver\*" $stage

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
    Start-Service $serviceName
    Stop-Transcript | Out-Null
}
catch {
    Write-Output "ERREUR : $_"
    Stop-Transcript | Out-Null
    exit 1
}
