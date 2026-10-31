# 36Screenlink

Utilise une tablette ou un téléphone Android comme écran étendu d'un PC Windows, en Wi-Fi ou en USB, avec tactile, clavier, stylet et son. Windows le voit comme un vrai moniteur. En français, réalisé avec Claude.

## Installation

Dans la page **Releases** du dépôt :

1. Sur le PC, téléchargez et lancez `36Screenlink-Setup.exe`.
2. Sur l'appareil Android, téléchargez et installez `36Screenlink.apk`, puis ouvrez l'application.
3. Comparez le code à 6 chiffres affiché sur le PC et sur l'appareil. S'il est identique, confirmez des deux côtés.

C'est tout : l'écran étendu apparaît. Ensuite, 36Screenlink démarre avec Windows, et il suffit d'ouvrir l'application.

PC et appareil doivent être sur le même réseau Wi-Fi.

| Avertissement possible | Quoi faire |
|---|---|
| « Windows a protégé votre ordinateur » | *Informations complémentaires*, puis *Exécuter quand même* : l'installateur n'est pas signé |
| Android bloque l'installation de l'APK | Autorisez l'installation depuis votre navigateur ou gestionnaire de fichiers |

## Prérequis

| Élément | Requis |
|---|---|
| **PC** | Windows 10 22H2 ou Windows 11, x64 |
| **GPU** | NVIDIA, AMD ou Intel, dédié ou intégré au processeur, avec encodage vidéo matériel HEVC ou H.264 et pilote à jour |
| **Android** | Android 11 ou plus récent |

Testé uniquement avec une RTX 3080 et une Lenovo Idea Tab Pro. AMD et Intel passent par Media Foundation, validé sur la RTX 3080 mais **pas encore sur un vrai GPU AMD ou Intel**. Sur un autre PC, Windows peut refuser le pilote, notamment avec Smart App Control.

## Utilisation

Sur la tablette, **glissez depuis le bord gauche vers le centre** pour ouvrir le menu. Touchez à côté pour le refermer.

| Réglage | Effet |
|---|---|
| **Sortie son** | PC ou tablette. Sur la tablette, le son du PC est coupé |
| **Clavier** | Ouvre le clavier à l'écran. Un clavier branché à l'appareil fonctionne aussi |
| **Étendu** | La tablette prolonge le bureau |
| **Dupliquer** | La tablette montre l'écran principal du PC |
| **Tablette seule** | L'écran du PC s'éteint. Il revient seul environ 15 s après une déconnexion |
| **Fréquence** | 60 ou 120 Hz |
| **Rotation** | Automatique ou verrouillée |
| **Tactile** | Se coupe pour dessiner au stylet sans que la paume clique |
| **Touche Windows, Afficher le bureau** | Raccourcis Windows |
| **Déconnecter** | Ferme l'application et retire le moniteur |

- **Menu Démarrer :** l'icône 36Screenlink relance le service si besoin. Il démarre déjà avec Windows.
- **Quitter :** clic droit sur l'icône près de l'horloge, puis *Quitter*. L'application reste fermée jusqu'à l'icône du menu Démarrer ou au prochain démarrage de Windows.
- **Barre des tâches de la tablette :** en mode Étendu, une fenêtre activée d'une touche sur la tablette y vient, même si elle était sur l'écran principal. Elle y reste, jusqu'à ce que vous la déplaciez.
- **Son depuis le PC :** clic droit sur l'icône près de l'horloge.
- **Rotation :** l'écran Windows suit l'appareil, avec une coupure d'environ une seconde.
- **Résolution :** le mode le plus proche de l'appareil est choisi, jusqu'à 120 Hz. Sans mode identique, l'image a des bandes noires.
- **Déconnexion :** le moniteur disparaît après 10 s, et Windows récupère les fenêtres.
- **Nouveau PC :** dans l'application, touchez *Nouvel appairage*, puis lancez 36Screenlink sur le PC.

## Version portable

`36Screenlink-Portable.zip` se décompresse dans un dossier ou sur une clé USB, sans installation : lancez `36Screenlink.exe` et acceptez l'invite administrateur.

| | Installateur | Portable |
|---|---|---|
| Démarrage avec Windows | Oui | Non |
| Invite administrateur | À l'installation | À chaque lancement |
| Fenêtres administrateur, verrouillage, UAC | Oui ou en option | Non |
| Traces sur le PC | Retirées à la désinstallation | Pilote, retiré par *Retirer le pilote et quitter* |

Sans droits administrateur, l'écran étendu est impossible. N'utilisez pas le portable sur un PC où 36Screenlink est installé.

## Options de l'installateur

**Prise en charge USB :** télécharge Android Platform Tools chez Google. L'appareil doit avoir le débogage USB actif.

**Validation UAC et déverrouillage depuis la tablette :** décoché par défaut. Avec cette option, toute personne tenant l'appareil, application ouverte, peut valider une invite UAC et agir sur le PC verrouillé. Protégez l'appareil par un code.

Sans cette option, l'écran de verrouillage et les invites UAC restent sur le PC.

## Désinstallation

Paramètres Windows > Applications > 36Screenlink > Désinstaller. Le pilote, le service, les certificats et les données sont retirés.

## Compilation

Prérequis : Visual Studio 2022 ou Build Tools avec C++, Windows SDK 10.0.26100, Android SDK, JDK 17 et Inno Setup 6. Une APK finale exige un `android/keystore.properties` à vous, jamais versionné, avec `storeFile`, `storePassword`, `keyAlias` et `keyPassword`.

```powershell
powershell -ExecutionPolicy Bypass -File windows/build-release.ps1 -Version 1.4.0
```

Le résultat se trouve dans `dist/release`.

## Sécurité

- Flux chiffré en AES-256-GCM, avec authentification mutuelle.
- La clé naît d'un échange ECDH, confirmé par un code des deux côtés. Elle n'est jamais transmise.
- Elle est protégée par DPAPI sous Windows et Android Keystore sur l'appareil, dans un dossier réservé à SYSTEM et aux administrateurs.
- Le pilote est signé par un certificat créé sur votre PC, dont la clé privée est détruite aussitôt. Aucun certificat commun n'est distribué.
- Un appareil déjà appairé refuse un nouveau PC tant que vous ne touchez pas *Nouvel appairage*.

Le service fonctionne sous le compte SYSTEM. Signalez les failles par un message privé plutôt que par un ticket public.

## Licence

Logiciel propriétaire, tous droits réservés : voir [LICENSE](LICENSE). Toute modification ou redistribution est interdite sans autorisation écrite de l'auteur. Les composants tiers gardent leurs licences, voir [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
