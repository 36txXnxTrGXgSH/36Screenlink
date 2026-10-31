# 36Screenlink

Utilise une tablette ou un téléphone Android comme écran étendu d'un PC Windows, en USB ou en Wi-Fi, avec tactile, clavier et stylet. Windows le voit comme un vrai moniteur. En français, réalisé avec Claude.

## Prérequis

| Élément | Requis |
|---|---|
| **PC** | Windows 10 22H2 ou Windows 11, x64 |
| **GPU** | **NVIDIA avec NVENC**. AMD et Intel ne sont pas pris en charge |
| **Android** | Android 11 ou plus récent |
| **USB** | Débogage USB actif, et [Android Platform Tools](https://developer.android.com/tools/releases/platform-tools) |

Testé uniquement avec une RTX 3080 et une Lenovo Idea Tab Pro. Les autres appareils peuvent demander des ajustements : les retours sont les bienvenus.

## Installation

1. Téléchargez et décompressez le paquet de la page Releases.
2. Installez `36Screenlink.apk` sur l'appareil.
3. Copiez `adb.exe`, `AdbWinApi.dll` et `AdbWinUsbApi.dll` de Platform Tools dans le dossier décompressé.
4. Dans ce dossier, ouvrez un PowerShell **administrateur** et installez le pilote :

```powershell
powershell -ExecutionPolicy Bypass -File .\install-driver.ps1
```

Les exécutables ne sont pas signés : Windows peut afficher un avertissement SmartScreen.

**Pilote :** il n'est pas signé par Microsoft. Le script crée un certificat propre à votre PC, signe le pilote, l'ajoute aux magasins de confiance de la machine, puis détruit la clé privée. Aucun certificat commun n'est distribué. Windows peut tout de même refuser ce pilote, notamment avec Smart App Control.

## Utilisation

1. **Appairage, une seule fois :** appareil branché en USB, lancez `Appairer.cmd`. Le même code à 6 chiffres doit s'afficher sur le PC et sur l'appareil. Confirmez des deux côtés uniquement s'ils sont identiques.
2. **En USB :** lancez `Demarrer-USB.cmd`.
3. **En Wi-Fi :** ouvrez l'application, puis lancez `Demarrer-WiFi.cmd`.

Une invite UAC crée le moniteur. Il disparaît 10 s après la déconnexion de l'appareil, et revient à la reconnexion. Pour quitter : clic droit sur l'icône de notification.

- **Résolution :** l'agent choisit le mode le plus proche de l'appareil, jusqu'à 120 Hz par défaut. Sans mode identique, l'image s'affiche avec des bandes noires.
- **Clavier :** le bouton ⌨ ouvre le clavier à l'écran.
- **Rotation :** l'écran Windows suit l'appareil, avec une coupure d'environ une seconde.
- **Pare-feu :** le PC se connecte à l'appareil, aucune règle entrante n'est nécessaire.

Options de `36Screenlink.exe` : `--pair`, `--usb`, `--wifi [ip]`, `--auto`, `--output n`, `--fps n`, `--bitrate Mbps`, `--codec hevc|h264`, `--no-input`, `--no-monitor`, `--tray`, `--duration s`, `--list`, `--stop`, `--adb chemin`.

## Service optionnel : UAC, verrouillage et fenêtres administrateur

Sans service, l'agent n'atteint ni les fenêtres administrateur, ni l'écran de verrouillage, ni les invites UAC. `install-service.ps1`, en administrateur, installe un service sous le compte SYSTEM qui lève ces limites et démarre avec Windows.

**Risque :** toute personne tenant l'appareil appairé, application ouverte, peut alors valider une invite UAC et agir sur le PC verrouillé. Protégez-le par un code, ou n'installez pas ce service.

Le script demande un nouvel appairage. Ensuite, n'utilisez plus `Demarrer-*.cmd` : ouvrez l'application. Journal : `C:\ProgramData\36Screenlink\logs\agent.log`. Retrait : `uninstall-service.ps1`.

## Désinstallation

Dans un PowerShell administrateur : `uninstall-service.ps1` si le service est installé, puis `uninstall-driver.ps1`. Supprimez ensuite le dossier et l'application.

## Compilation

Prérequis : Visual Studio 2022 ou Build Tools avec C++, Windows SDK 10.0.26100, CMake, Android SDK et JDK 17.

```powershell
cmake -S windows -B windows/build -G "Visual Studio 17 2022" -A x64
cmake --build windows/build --config Release
powershell -ExecutionPolicy Bypass -File windows/driver/build-driver.ps1
```

Le WDK est téléchargé via NuGet ; `WDK_ROOT` permet d'utiliser une copie existante.

Android, depuis `android/`. Une version finale exige un `keystore.properties` à vous, jamais versionné, avec `storeFile`, `storePassword`, `keyAlias` et `keyPassword` :

```powershell
.\gradlew.bat assembleRelease
```

Puis `windows/package-release.ps1 -Version 1.0.0` crée le paquet dans `dist/release`. Il ne contient que les fichiers listés dans le script, donc aucune donnée locale.

## Sécurité

- Flux chiffré en AES-256-GCM, avec authentification mutuelle par clé partagée.
- La clé naît d'un échange ECDH par USB, confirmé par un code des deux côtés.
- Elle est protégée par DPAPI sous Windows et Android Keystore sur l'appareil.
- Le service stocke sa clé dans un dossier réservé à SYSTEM et aux administrateurs.

Signalez les failles par un message privé plutôt que par un ticket public.

## Licence

Logiciel propriétaire, tous droits réservés : voir `LICENSE`. Toute modification ou redistribution est interdite sans autorisation écrite de l'auteur. Les composants tiers gardent leurs licences, voir [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
