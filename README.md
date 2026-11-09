# 36Screenlink

Utilise une tablette ou un téléphone Android comme écran étendu d'un PC Windows, en Wi-Fi ou en USB, avec tactile, clavier, stylet et son. Windows le voit comme un vrai moniteur. En français, réalisé avec Claude.

**[Télécharger](https://github.com/36txXnxTrGXgSH/36Screenlink/releases/latest)** · **[Page de présentation](https://36txxnxtrgxgsh.github.io/36Screenlink/)**

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

Testé uniquement avec une RTX 3080, une tablette Lenovo Idea Tab Pro et un téléphone Motorola edge 60. AMD et Intel passent par Media Foundation, validé sur la RTX 3080 mais **pas encore sur un vrai GPU AMD ou Intel**. Sur un autre PC, Windows peut refuser le pilote, notamment avec Smart App Control.

## Tablette ou téléphone

La même application sert aux deux et s'adapte toute seule :

- **Tablette :** un grand second écran. L'orientation suit l'appareil.
- **Téléphone :** un petit écran d'appoint, pour une messagerie, un lecteur ou un tableau de bord à côté du clavier. Il s'ouvre en paysage, même avec la rotation d'Android verrouillée, et le menu parle de « Téléphone ».
- **Résolution sur téléphone :** l'écran Windows prend le mode le plus proche disponible. Sur le Motorola edge 60 (2712×1220, 120 Hz), c'est 2400×1080 à 60 Hz pour l'instant.

## Utilisation

Sur l'appareil, **glissez depuis le bord gauche vers le centre** pour ouvrir le menu en verre dépoli. Touchez à côté ou la croix pour le refermer. L'épingle le garde ouvert : les touches à côté repartent alors vers Windows.

| Réglage | Effet |
|---|---|
| **Sortie son** | PC ou appareil. Sur l'appareil, le son du PC est coupé |
| **Clavier** | Ouvre le clavier à l'écran. Un clavier branché à l'appareil fonctionne aussi, raccourcis et touches F1 à F12 compris |
| **Étendu** | L'appareil prolonge le bureau |
| **Dupliquer** | L'appareil montre l'écran principal du PC |
| **Tablette seule / Téléphone seul** | L'écran du PC s'éteint. Il revient seul environ 15 s après une déconnexion |
| **Fréquence** | 60, 120 ou 144 Hz selon l'appareil |
| **Orientation** | Auto (suit l'appareil), Paysage ou Portrait, gardé d'une fois sur l'autre. Paysage par défaut sur un téléphone |
| **Tactile** | Se coupe pour dessiner au stylet sans que la paume clique |
| **Clavier auto** | Coupé par défaut. Activé, le clavier à l'écran s'ouvre quand vous touchez un champ où écrire dans Windows, et se referme quand vous en sortez |
| **Touche Windows, Afficher le bureau** | Raccourcis Windows |
| **Déconnecter** | Ferme l'application et retire le moniteur |

La touche Retour referme le menu ; deux appuis rapprochés ferment l'application.

- **Menu Démarrer :** l'icône 36Screenlink relance le service si besoin. Il démarre déjà avec Windows.
- **Quitter :** clic droit sur l'icône près de l'horloge, puis *Quitter*. L'application reste fermée jusqu'à l'icône du menu Démarrer ou au prochain démarrage de Windows.
- **Barre des tâches de l'appareil :** en mode Étendu, une fenêtre activée d'une touche sur l'appareil y vient, même si elle était sur l'écran principal. Elle y reste, jusqu'à ce que vous la déplaciez.
- **Envoyer une fenêtre sur un écran :** dans la barre des tâches de l'appareil, posez le doigt sur l'icône d'une application ouverte et glissez vers le haut. Un rectangle numéroté s'affiche pour chaque écran du PC : lâchez l'icône sur celui voulu. Si l'application a plusieurs fenêtres, touchez ensuite la miniature voulue. L'appui long sur l'icône ouvre toujours sa liste de raccourcis.
- **Clavier à l'écran :** quand il couvre le champ où vous écrivez, l'image de Windows remonte pour le garder visible, puis redescend à la fermeture du clavier.
- **Son depuis le PC :** clic droit sur l'icône près de l'horloge.
- **Clavier branché à l'appareil :** Android garde pour lui la touche Windows et Alt+Tab. Utilisez Ctrl+Échap pour le menu Démarrer, Alt+² (la touche sous Échap) pour Alt+Tab, ou la tuile Windows du menu.
- **Souris branchée à l'appareil :** déplacement, clic droit, clic du milieu et molettes arrivent dans Windows.
- **Veille :** tant que l'appareil est connecté, les écrans du PC ne se mettent pas en veille, sinon Windows cesserait de dessiner l'écran étendu. Le verrouillage automatique de session attend donc lui aussi la déconnexion.
- **Rotation :** l'écran Windows suit l'appareil, avec une coupure d'environ une seconde.
- **Résolution :** le mode le plus proche de l'appareil est choisi, jusqu'à 120 Hz. Sans mode identique, l'image a des bandes noires.
- **Déconnexion :** le moniteur disparaît après 10 à 15 s, et Windows récupère les fenêtres.
- **Batterie :** en flux, la tablette consomme plus que ce qu'un port USB ordinaire lui fournit, environ 6 % par heure mesurés. Pour une longue session, branchez l'appareil à un port USB-C PD ou à son chargeur.
- **Nouveau PC :** dans l'application, touchez *Nouvel appairage*, puis lancez 36Screenlink sur le PC. *Oublier ce PC* efface la clé d'appairage de l'appareil.

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

**Validation UAC et déverrouillage depuis l'appareil :** décoché par défaut. Avec cette option, toute personne tenant l'appareil, application ouverte, peut valider une invite UAC et agir sur le PC verrouillé. Protégez l'appareil par un code.

Avec cette option, l'appareil demande d'abord son code, son empreinte ou son visage : sans cette confirmation, le PC ignore toute saisie sur l'UAC ou l'écran verrouillé.

Sans cette option, l'écran de verrouillage et les invites UAC restent sur le PC.

## Désinstallation

Paramètres Windows > Applications > 36Screenlink > Désinstaller. Le pilote, le service, les certificats et les données sont retirés.

## Compilation

Prérequis : Visual Studio 2022 ou Build Tools avec C++, Windows SDK 10.0.26100, Android SDK, JDK 17 et Inno Setup 6. Une APK finale exige un `android/keystore.properties` à vous, jamais versionné, avec `storeFile`, `storePassword`, `keyAlias` et `keyPassword`.

```powershell
powershell -ExecutionPolicy Bypass -File windows/build-release.ps1 -Version 1.7.0
```

Le résultat se trouve dans `dist/release`.

## Sécurité

- Flux chiffré en AES-256-GCM, avec authentification mutuelle.
- La clé naît d'un échange ECDH, confirmé par un code des deux côtés. Elle n'est jamais transmise.
- Chaque session ajoute un ECDH éphémère : une clé volée plus tard ne déchiffre pas une session enregistrée.
- Des connexions muettes d'un autre appareil du réseau ne bloquent plus l'appareil Android, et un message malformé est ignoré.
- Elle est protégée par DPAPI sous Windows et Android Keystore sur l'appareil, dans un dossier réservé à SYSTEM et aux administrateurs.
- Le pilote est signé par un certificat créé sur votre PC, dont la clé privée est détruite aussitôt. Aucun certificat commun n'est distribué.
- Un appareil déjà appairé refuse un nouveau PC tant que vous ne touchez pas *Nouvel appairage*.

Le service fonctionne sous le compte SYSTEM. Un administrateur du PC peut lire la clé d'appairage, comme tout secret stocké sur la machine. Signalez les failles par un message privé plutôt que par un ticket public.

Ce qui est diffusé, et ce qui ne l'est pas : voir [CONFIDENTIALITE.md](CONFIDENTIALITE.md).

## Licence

Logiciel propriétaire, tous droits réservés : voir [LICENSE](LICENSE). Toute modification ou redistribution est interdite sans autorisation écrite de l'auteur. Les composants tiers gardent leurs licences, voir [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
