# 36ScreenlinkAndroid — Faisabilité IDD et architecture globale

Version 0.3 — 2026-10-31. Document de conception initial, écrit pour un usage personnel : le projet est désormais public, et le README fait foi. Le scénario par carte de capture HDMI vers USB est exclu du périmètre. Le squelette de code vient après la levée des décisions de la section 10.

Convention de fiabilité utilisée partout :

- **[V]** vérifié : doc Microsoft/Android consultée, ou constaté sur cette machine.
- **[E]** estimation d'ingénierie, à mesurer. Aucun chiffre de latence ou de débit de ce document n'a été mesuré.
- **[P]** hypothèse à prouver par un spike (section 8).

## Écarts de l'implémentation v0.1

Le code livré diffère de ce document sur les points suivants. Mesures : `03-resultats-tests.md`, protocole réel : `02-protocole.md`.

| Sujet | Document | Implémenté | Raison |
|---|---|---|---|
| Sens de connexion | PC serveur, `adb reverse` | Tablette serveur, `adb forward` | Aucune règle de pare-feu sur le PC |
| Sécurité | TLS 1.3, PIN/QR, SPAKE2 | ECDH par USB avec code à comparer, AES-256-GCM | Pas de PIN à protéger, résistant à une app malveillante |
| Wi-Fi | UDP, NACK/FEC | TCP | UDP reporté sur mesures, voir `03-resultats-tests.md` |
| Cadence | 60 Hz | 120 Hz par défaut, 80 Mb/s, débit adaptatif | Mesuré plus réactif |
| Interface | WPF | Icône Win32 native | SDK .NET absent ; exe portable unique |
| Pilote/agent | IOCTL, texture partagée | Pilote qui vide la swapchain, capture Desktop Duplication | Prévu au MVP ; phase 5 pour la texture |
| Moniteur | Créé à la connexion de la tablette, IOCTL | Conforme, via `36ScreenlinkDevice.exe` élevé et deux événements, retrait après 10 s | Agent non élevé, sans IOCTL |
| Android | Android 14 attendu | Android 16 constaté, `minSdk` 34 conservé | Mesure sur l'appareil |

## 0. Configuration cible

| Élément | Valeur | Conséquence |
|---|---|---|
| Tablette | Lenovo Idea Tab Pro (TB373FU), Android 14 [V] | Une seule cible : `MediaCodec` et réglages à valider sur ce SoC uniquement |
| Écran | 12,7", 2944×1840, 16:10, 144 Hz, 273 PPI [V] | Résolution absente des modes HDMI courants (section 4.5) |
| SoC | MediaTek Dimensity 8300, décodage H.264, HEVC, VP9, AV1 [V] | Aucun débit de décodage officiel en 4K60 trouvé (spike S3) |
| USB | USB-C 5 Gb/s, charge, **DP-Out** [V] | **Sortie vidéo seule** : aucune source n'indique d'entrée vidéo |
| Wi-Fi | Wi-Fi 6E, 1000 Mb/s mesurés en 5 GHz par Notebookcheck [V] | Limite pratique : le PC est en Ethernet 1 Gb/s |
| PC | RTX 3080 (pilote 610.88), Windows 11 build 26300, HVCI actif [V] | NVENC seul à valider en v1 |
| Sorties PC | Écran principal 3440×1440 en DisplayPort [V] | Port HDMI supposé libre, à confirmer physiquement |
| Usage | Personnel, sur vos propres PC | Pas de signature Microsoft (section 3.1) |

**Conséquence directe :** la tablette ne peut pas recevoir un signal HDMI ou DisplayPort. Le câblage direct PC vers tablette en USB-C ne fournit donc pas d'écran natif. Il reste deux façons d'obtenir un moniteur dans Windows : IDD logiciel ou dummy plug HDMI.

---

## 1. Verdict

| Exigence | Verdict | Condition ou limite |
|---|---|---|
| Moniteur dans Paramètres > Affichage (étendre, dupliquer, résolution, fréquence) | **Faisable** | Par IDD ou par dummy plug |
| Pilote sans code noyau | **Faisable** | IDD = UMDF pur, aucun composant noyau tiers |
| Fonctionnement sur vos propres PC | **Faisable, sans signature Microsoft** | Signature auto-signée en mode IDD (spike S1) ; aucune en mode HDMI |
| Portable sans droits admin | **Faisable en mode HDMI** | Dummy plug requis. Impossible avec l'IDD (section 3) |
| Portable sans trace durable en mode IDD | **Faisable** | Une élévation UAC à chaque session, nettoyage automatique |
| Câble direct PC vers tablette reconnu comme écran | **Impossible** | La tablette n'a pas d'entrée vidéo |
| USB < 50 ms bout en bout | **Probable** [E] | Dépend surtout du décodeur du Dimensity 8300 |
| Wi-Fi 5 ou 6 GHz | **Faisable** | Latence correcte, pics à absorber |
| Bluetooth comme transport vidéo | **Non viable** | Conservé uniquement pour l'appairage/réveil (section 6.3) |

**Conclusion : GO.** La signature n'est plus un verrou pour un usage personnel. Le risque principal est la latence réelle du décodeur de la tablette.

---

## 2. Validation du pilote IDD

### 2.1 Preuves

- **[V] Modèle officiel.** Microsoft cite explicitement le streaming réseau vers un client distant et les moniteurs virtuels comme cas d'usage de l'IDD. Le pilote est un UMDF qui crée l'adaptateur, signale les arrivées/départs de moniteurs, décrit les modes et traite les images dans une surface DirectX.
- **[V] Preuve sur cette machine.** Windows 11 26H2 build 26300 exécute déjà un IDD tiers : `Parsec Virtual Display Adapter` (classe `Display`, `UmdfExtensions = IddCx0102`, `UmdfHostProcessSharing = ProcessSharingDisabled`, `UmdfHostPriority = PriorityHigh`, signé `Microsoft Windows Hardware Compatibility Publisher`). Son INF est un bon modèle de configuration. Les processus `WUDFHost` tournent en session 0.
- **[V] Coexistence.** Le pilote Parsec cohabite avec la RTX 3080 ; notre IDD devra aussi cohabiter avec lui.
- **[V] Compatibilité.** Un binaire compilé contre IddCx 1.10 ou 1.11 tourne sur Windows 10 1803 et suivants, avec tests à l'exécution (`IDD_IS_FUNCTION_AVAILABLE`, `IddCxCheckOsFeatureSupport`).

### 2.2 Fonctions IddCx utiles par version [V]

| Version | Apport pour ce projet | Plancher OS |
|---|---|---|
| 1.2 | `IddCxGetVersion`, `IddCxReportCriticalError` | Win10 1709 |
| 1.4 | `IddCxAdapterSetRenderAdapter` (multi-GPU), `EvtIddCxMonitorGetPhysicalSize` | Win10 1903 |
| 1.7 | `IddCxMonitorQueryHardwareCursor2` | Server 2022 |
| 1.8 | `PREFER_PRECISE_PRESENT_REGIONS` | Win11 21H2 |
| 1.9 | `IddCxSetRealtimeGPUPriority` ; le process pooling UMDF est interdit | Win11 22H2 |
| 1.10 | HDR10, gamme étendue SDR (0x1A00), gestion d'énergie runtime (0x1A80) | Win11 23H2 / 2024 |
| 1.11 | Swapchain D3D12, mise à jour du reencode count statique, descripteurs DisplayID seuls | À détecter à l'exécution |

**Décision :** compiler contre le header le plus récent du WDK, activer les fonctions à l'exécution, cibler Windows 10 22H2 et Windows 11. Rester en D3D11 : la synchronisation D3D12 est plus exigeante et n'apporte rien à cette charge. Le HDR est hors périmètre v1.

### 2.3 Capacités et contraintes de conception

- Le pilote s'exécute en session 0, sans composant dans la session utilisateur. Interdits : GDI, fenêtrage, OpenGL, Vulkan.
- Une boucle swapchain dédiée consomme les images (`SwapChainProcessor` dans le sample [V]). Elle doit rester active même sans client, sous peine de bloquer le rendu de ce moniteur [P].
- Le curseur matériel (`IddCxMonitorSetupHardwareCursor`) permet de sortir le curseur du flux vidéo et de le dessiner côté Android, ce qui réduit la latence perçue.
- Un plantage du pilote reste confiné à `WUDFHost` et n'entraîne pas d'écran bleu.

### 2.4 IDD maison ou pilote existant

| Critère | IDD maison (sample Microsoft) | Virtual-Display-Driver | Parsec VDA |
|---|---|---|---|
| Licence | MS-PL [V] | MIT [V, page du dépôt] | Propriétaire, redistribution non acquise |
| Signature | À notre charge | Assurée par le projet via SignPath [V] | Assurée par Parsec |
| Modes dictés par l'appareil Android, EDID unique par appareil | Oui | Configuration par XML | Non |
| Sortie des images vers notre agent | Oui, conçue pour | Non : moniteur seul, capture par un tiers [P] | Non |
| Curseur matériel séparé | Oui | À vérifier | Non |
| Cycle de vie piloté par notre app | Oui | Partiel | Limité |
| Effort initial | 3 à 5 semaines [E] | Quelques jours | Aucun, en développement seulement |

**Décision : IDD maison dérivé du sample Microsoft, en conservant la notice MS-PL.** Le besoin (moniteur créé à la connexion, modes négociés, images en accès direct, curseur séparé) correspond au sample, et non à un pilote de type « moniteur factice ». Virtual-Display-Driver sert de plan B et de banc de test.

**Accélérateur :** le pipeline peut être développé sans attendre le pilote, avec le dummy plug de la section 4.5. En attendant le plug, le Parsec VDA présent sur ce PC peut servir de moniteur de test.

---

## 3. Signature et portabilité : le vrai verrou

### 3.1 Signature

| Option | Usage | Coût | Remarque |
|---|---|---|---|
| Certificat auto-signé + mode test | Développement | Gratuit | `bcdedit testsigning` ; réservé aux VM ou PC de dev |
| Certificat auto-signé importé dans Trusted Root/Publisher, sans mode test | Parc maîtrisé | Gratuit | Pratique répandue pour les UMDF, sans confirmation officielle [P]. **Rejeté pour le portable** : imposer une racine de confiance sur la machine d'un tiers est une faute de sécurité |
| Attestation Partner Center | Distribution directe | EV + compte | Microsoft signe, pas de Windows Update |
| WHCP (HLK) | Distribution commerciale | EV + banc HLK + effort élevé | Seule voie pour Windows Update |
| SignPath Foundation | Open source uniquement | Gratuit sous conditions | Ne donne pas une signature Microsoft ; effet réel sur l'installation PnP à vérifier [P] |

Points vérifiés :

- **[V]** Microsoft exige un certificat EV pour le programme matériel, auprès de Certum, DigiCert, GlobalSign, IdenTrust, Sectigo ou SSL.com.
- **[V]** L'attestation ne fonctionne que sur Windows 10 desktop et suivants, donc pas sur Windows Server. Elle n'est pas publiée sur Windows Update pour le grand public.
- **[V] Risque d'évolution.** La documentation classe désormais l'attestation sous l'intitulé « for testing scenarios ». La distribution directe reste techniquement possible, mais cette formulation est à surveiller. Parsec prouve qu'un IDD peut être signé Microsoft ; le signataire affiché ne permet pas de savoir s'il s'agit d'attestation ou de WHCP.
- **[V]** La politique d'avril 2026 retirant la confiance aux pilotes cross-signed « applies to kernel-mode drivers only ». Notre DLL est en mode utilisateur et `WUDFRd.sys` est fourni par Windows : elle n'est donc pas visée. La politique bloque en revanche tout pilote noyau non WHCP, ce qui confirme qu'il ne faut **aucun** `.sys` tiers dans le produit.
- **[E]** Azure Trusted Signing n'est pas utilisable pour le pilote d'après une réponse communautaire Microsoft Q&A, à confirmer. Il pourrait en revanche convenir pour signer les `.exe`.
- **[P]** Un EV s'adresse en général à une entité légale. À vérifier auprès de l'autorité choisie si le projet est porté par un développeur individuel.

**Décision (usage personnel) :** certificat auto-signé, avec mode test sur VM pendant le développement, puis ajout du certificat aux magasins de confiance de vos propres PC si le spike S1 le confirme. Si l'IDD refuse de se charger sans mode test, deux replis restent possibles : mode HDMI (sans pilote) ou Virtual-Display-Driver. Attestation et WHCP sont hors périmètre, et ne redeviendraient nécessaires qu'en cas de distribution à des tiers.

Protéger la clé privée de ce certificat : un certificat racine ajouté à la machine lui fait confiance pour tout ce qu'il signe. Le générer sans clé exportable hors de vos postes, avec contrainte de nom.

### 3.2 Ce que « portable » peut signifier

Faits bloquants :

- **[V]** `SwDeviceCreate` exige des droits Administrateur.
- **[V]** Ajouter un paquet au Driver Store (`pnputil /add-driver`) exige l'élévation.
- **[V]** Avec `SWDeviceLifetimeHandle`, valeur par défaut, le périphérique disparaît à la fermeture du handle, y compris lors d'un plantage du processus.

Ce dernier point est très favorable : le nettoyage est garanti par le système.

**Définition retenue du mode portable :**

```text
Dossier ou clé USB, aucun installeur, rien dans Program Files
  Agent.exe (asInvoker)
    └─ UAC → Helper.exe (requireAdministrator)
         ├─ paquet pilote absent → pnputil /add-driver
         ├─ SwDeviceCreate(lifetime = handle)
         └─ reste vivant pendant la session, handle ouvert
  À la fermeture ou au crash
    ├─ handle fermé → moniteur et périphérique retirés par PnP
    └─ option « Ne rien laisser » → pnputil /delete-driver /uninstall
```

| Élément | Installé | Portable |
|---|---|---|
| Pilote IDD | Permanent | Temporaire, UAC à chaque session |
| Service Windows SYSTEM | Oui | Non |
| Démarrage automatique | Oui | Non |
| Contenu sur l'écran UAC ou l'écran de verrouillage | Oui, phase 5 | Oui si images issues du pilote [P] ; pas d'injection tactile |
| Injection tactile vers fenêtres élevées | Oui, via service | Seulement si le helper élevé injecte |
| Configuration | `%ProgramData%` + `%AppData%` | Fichier à côté de l'exécutable |

**Sans droits admin**, l'IDD est impossible. Le mode HDMI de la section 4.5 répond à ce cas : Windows voit un moniteur physique et aucun pilote n'est installé.

Conséquence pour l'usage personnel : le portable IDD fonctionne sur vos PC une fois le certificat ajouté à chacun, ce qui demande une élévation initiale. Sur un PC où vous n'êtes pas administrateur, seul le mode HDMI convient.

---

## 4. Architecture globale

### 4.1 Composants

```text
WINDOWS                                                       ANDROID (Kotlin + NDK)
┌──────────────────────────────────────────────┐
│ dwm.exe → compose le bureau sur le moniteur  │
│              │ swapchain IddCx               │
│ ┌────────────▼───────────────┐ Session 0     │
│ │ IDD UMDF (WUDFHost)        │               │
│ │ moniteurs, modes, EDID,    │               │
│ │ curseur, copie image       │               │
│ └───┬────────────────┬───────┘               │
│ IOCTL│ texture partagée (NT handle)          │
│ ═════╪════════════════╪═════════════════════ │
│ ┌───▼────────────────▼───────┐ Session user  │     ┌─────────────────────────────┐
│ │ Agent C++ (cœur temps réel)│               │     │ Transport                   │
│ │ IFrameSource               │               │     │ TCP tunnel / UDP            │
│ │  ├ IddSharedTexture (cible)│               │     │ réassemblage, NACK/FEC      │
│ │  └ DesktopDuplication (MVP)│               │     └──────────┬──────────────────┘
│ │ IEncoder                   │  USB : TCP    │                │
│ │  ├ NVENC  ├ AMF  ├ oneVPL  │  via adb      │     ┌──────────▼──────────────────┐
│ │  └ Media Foundation        │◄─────────────►│     │ MediaCodec (low latency)    │
│ │ ITransport                 │  Wi-Fi :      │     │ → SurfaceView plein écran   │
│ │  ├ TunnelTcp  └ UdpMedia   │  TLS + UDP    │     │ overlay curseur séparé      │
│ │ Control · Pairing · QoS    │               │     └──────────┬──────────────────┘
│ │ Injection pointeur         │◄───── touch ──│◄───────────────┘
│ └───▲────────────────────────┘               │     Découverte NsdManager, UI appairage
│     │ Named pipe (JSON)                      │
│ ┌───┴────────────────────┐                   │
│ │ UI WPF / tray          │ Helper élevé      │
│ └────────────────────────┘ ou service SYSTEM │
└──────────────────────────────────────────────┘
```

### 4.2 Choix structurants

| Sujet | Décision | Justification | Alternative écartée |
|---|---|---|---|
| Lieu de l'encodage | Agent, hors du pilote | Isolation des pannes ; pas de re-signature à chaque réglage ; pas de réseau dans `WUDFHost` | Encodage dans le pilote : compte à droits réduits et chaque correctif repasse par Microsoft |
| Source d'images | DXGI Desktop Duplication au MVP, texture partagée pilote en cible | Le MVP évite l'IPC session 0 ; la cible voit le bureau sécurisé et supprime une étape | WGC : pratique, mais pas conçu pour l'écran UAC |
| Langage du cœur | C++20 | Accès direct à D3D11, SDK encodeurs, IOCTL | C# : pauses GC incompatibles avec le chemin temps réel |
| UI | WPF sur .NET 10 self-contained | Portable simple (dossier unique, sans MSIX) | WinUI 3 : déploiement non packagé plus lourd pour un gain visuel secondaire |
| Encodage | SDK directs NVENC, AMF, oneVPL ; repli Media Foundation | Zéro copie D3D11, latence contrôlée, licences permissives | FFmpeg + x264 : copie mémoire et contrainte GPL. Option de repli uniquement si GPL acceptée |
| Codec | HEVC par défaut, H.264 en repli, AV1 ultérieurement | Meilleur rendement pour du contenu d'écran ; la RTX 3080 n'encode pas l'AV1 | H.264 seul |
| Transport USB | TCP via `adb reverse` | Pas de perte, simple, PC serveur dans tous les modes | Android Open Accessory : pas de mode développeur, mais pilote WinUSB supplémentaire |
| Transport Wi-Fi | Protocole média maison sur UDP, inspiré de RTP | Pas de tampon de gigue de visioconférence ; contrôle fin du NACK/FEC | WebRTC, voir ci-dessous |
| Tactile | `CreateSyntheticPointerDevice` + `InjectSyntheticPointerInput` | Multi-touch et stylet, sans pilote HID | `SendInput` : souris seule ; pilote HID virtuel : seconde signature |

### 4.3 WebRTC ou protocole maison

| Critère | libwebrtc | RTP-like maison |
|---|---|---|
| Traversée NAT | Excellente, sans intérêt sur un LAN | Absente, non nécessaire |
| Contrôle de congestion | GCC fourni | À écrire, version simple suffisante à un seul saut |
| Latence | Tampon de gigue orienté visio | Pilotée par nous |
| Mode USB | Inadapté à un tunnel TCP | Même format de trame |
| Intégration zéro copie NVENC | Encodeur externe à brancher | Native |
| Taille et build | Lourds, surtout en double plateforme | Faibles |
| Risque | Faible sur le transport, élevé sur le build | Bugs à notre charge |

**Décision : protocole maison.** À ma connaissance, non revérifiée ici, Moonlight/Sunshine suit la même approche (RTP et FEC Reed-Solomon). Le risque est borné : `ITransport` permet d'ajouter WebRTC plus tard si un usage hors LAN apparaît.

### 4.4 Cycle de vie d'une session

1. L'appareil se connecte (USB détecté par `adb track-devices` ou annonce mDNS).
2. Le canal de contrôle TLS est établi et authentifié, ou l'appairage est lancé.
3. Android annonce résolution native, densité, fréquences, codecs et profils de décodage.
4. L'agent choisit la configuration et demande au pilote `ADD_MONITOR` avec un EDID généré. Son numéro de série, dérivé de l'identité de l'appareil, permet à Windows de mémoriser disposition et échelle.
5. Windows active le moniteur, appelle `EvtIddCxMonitorAssignSwapChain` et le flux démarre.
6. Un changement de mode dans Paramètres relance l'encodeur et notifie Android.
7. La rotation Android met à jour la liste des modes [P].
8. À une coupure, un délai de grâce de 10 s (réglable) conserve le moniteur. Windows ne redistribue donc pas les fenêtres à chaque micro-coupure Wi-Fi. Au-delà, `IddCxMonitorDeparture`.

En mode HDMI, les étapes 4 et 7 disparaissent : le moniteur existe tant que le plug est branché (section 4.5).

### 4.5 Mode HDMI par dummy plug

Un dummy plug est une fiche branchée sur une sortie HDMI du GPU. Il active Hot Plug Detect et fournit un EDID, sans afficher d'image. Windows le traite comme un moniteur physique. L'agent capture cet écran et l'envoie à la tablette par le même flux USB ou Wi-Fi. Le plug ne transporte **aucune image vers la tablette**.

```text
GPU ──HDMI──► dummy plug           (EDID, Hot Plug Detect, pas d'image)
 │
 └─ Bureau étendu → Desktop Duplication → NVENC → USB ou Wi-Fi → tablette
```

**Comparaison des deux modes**

| Critère | Mode IDD | Mode HDMI |
|---|---|---|
| Pilote et signature | Pilote auto-signé | Aucun |
| Droits admin | À chaque session en portable | Aucun pour la capture |
| Matériel | Aucun | Dummy plug, un port HDMI libre |
| Résolution native 2944×1840 | Oui, EDID généré | Selon l'EDID du plug |
| Fréquences | Libres, jusqu'à 144 Hz si le pipeline suit | 60 à 90 Hz réalistes |
| Moniteur à la déconnexion | Retiré ou conservé par l'agent | Reste présent, à gérer |
| Écran UAC et verrouillage | Possible par texture pilote [P] | Coupure de la duplication |
| Effort | 4 à 5 semaines [E] | Quelques jours [E] |

**Paramètres HDMI**

Les horloges ci-dessous sont calculées en CVT Reduced Blanking pour RGB 8 bits [E]. La méthode redonne bien les 268,5 MHz standard du mode 2560×1600@60.

| Mode | Horloge pixel | Lien requis | Pixels/s | Débit HEVC cible [E] | Statut |
|---|---|---|---|---|---|
| 1920×1200 @ 60 | 154 MHz | HDMI 1.4 | 138 M | 20 à 30 Mb/s | Repli, mise à l'échelle sur la tablette |
| 2560×1600 @ 60 | 268 MHz | HDMI 1.4 | 246 M | 40 à 60 Mb/s | **Repli recommandé**, même ratio 16:10 |
| **2944×1840 @ 60** | ~353 MHz | **HDMI 2.0** (dépasse 340 MHz) | 325 M | 50 à 80 Mb/s | **Cible par défaut**, net au pixel près |
| 2944×1840 @ 90 | ~537 MHz | HDMI 2.0 | 488 M | 70 à 110 Mb/s | Possible, limite du plug et du décodeur |
| 2944×1840 @ 120 | ~727 MHz | HDMI 2.1 (au-delà de 600 MHz) | 650 M | 90 à 140 Mb/s | Non retenu |
| 2944×1840 @ 144 | ~882 MHz | HDMI 2.1 | 780 M | 110 à 170 Mb/s | Non retenu, réservé à l'IDD |

Réglages retenus :

| Paramètre | Valeur | Justification |
|---|---|---|
| Plug | HDMI 2.0 minimum, à EDID programmable de préférence | 2944×1840 n'existe pas dans les EDID courants |
| Port | Sortie HDMI de la RTX 3080 | DisplayPort déjà utilisé par l'écran principal |
| Format | RGB plein, 8 bits, 4:4:4 en sortie GPU | NVENC convertit lui-même en 4:2:0 |
| Fréquence | 60 Hz par défaut | Demander 60 ou 120 Hz à la tablette via `setFrameRate` pour éviter les saccades ; à 144 Hz, 60 Hz n'est pas un diviseur exact (spike S3) |
| Mise à l'échelle Windows | 200 % | 273 PPI : donne un bureau de 1472×920 |
| HDR, VRR/G-Sync, HDCP | Désactivés | Le plug n'a pas de dalle ; les contenus DRM resteront noirs |
| Sortie audio HDMI | Ne pas sélectionner par défaut | Le plug crée un périphérique audio sans son réel |
| Encodage | Résolution du mode, sans remise à l'échelle sur PC | Évite un flou et une étape de calcul |

Si l'EDID du plug ne contient pas le mode voulu, trois moyens existent, par ordre de préférence :

1. Reprogrammer l'EDID du plug. Le réglage suit alors le matériel, et aucun droit admin n'est requis sur les PC.
2. Créer une résolution personnalisée dans le panneau NVIDIA. Réglage local au PC et au pilote.
3. Surcharger l'EDID dans le registre avec CRU. Droits admin requis ; ce choix annule l'intérêt du portable sans admin.

**Comportements propres au mode HDMI**

- **Déconnexion de la tablette :** le plug reste branché, donc l'écran reste étendu et des fenêtres peuvent y rester invisibles. L'agent doit désactiver cette sortie via `SetDisplayConfig` après le délai de grâce, puis la réactiver à la reconnexion [P].
- **Identification :** l'agent repère le bon écran par son EDID, et n'utilise jamais la sortie DisplayPort principale.
- **Curseur :** Desktop Duplication fournit forme et position séparément, donc l'overlay côté Android reste possible.
- **Tactile :** il passe par le canal d'entrées, et doit être cadré sur les coordonnées de cet écran.
- **Mise en veille :** une extinction d'écran Windows coupe aussi cette sortie. Empêcher la veille d'écran pendant une session.
- **Sans droits admin :** adb fonctionne en mode USB si le pilote USB de la tablette est déjà présent. Le Wi-Fi peut exiger une règle de pare-feu, donc une élévation [P].

**Décision :** le mode HDMI devient la base du MVP, car il valide tout le pipeline sans pilote. L'IDD reste à développer pour se passer du plug et atteindre 144 Hz. `IFrameSource` permet aux deux modes de partager l'encodage et le transport.

---

## 5. Protocole

### 5.1 Canaux

| Canal | Wi-Fi | USB | Contenu |
|---|---|---|---|
| Contrôle | TLS 1.3 sur TCP | Même chose dans le tunnel | Capacités, modes, QoS, keepalive, curseur |
| Vidéo | UDP chiffré | Multiplexé dans une 2e connexion tunnel | Unités d'accès fragmentées |
| Entrées | UDP chiffré, redondance légère | Multiplexé | Pointeur, stylet, clavier |

La même trame binaire sert partout, seul le mode de transport change. TLS reste actif sur USB : un tunnel `adb` écoute en local, et toute application locale pourrait s'y connecter.

### 5.2 Sécurité

- Chaque hôte et appareil possède une paire de clés P-256 et un certificat auto-signé généré au premier lancement. La clé privée est protégée par DPAPI sur Windows et par Android Keystore.
- **QR code** : empreinte du certificat PC, adresse, jeton aléatoire à usage unique de 128 bits. Ce canal hors bande authentifie directement le serveur.
- **PIN à 6 chiffres** : un PIN brut en TLS auto-signé serait cassable hors ligne ou exposé à une attaque de l'homme du milieu. Il faut donc un PAKE (SPAKE2) lié à la session TLS par exporter (RFC 5705/8446). Blocage après 5 essais.
- Après appairage, les empreintes sont épinglées. Tout changement exige un nouvel appairage.
- Les clés média AES-128-GCM dérivent de l'exporter TLS et sont renouvelées avant toute réutilisation de nonce. Le nonce contient époque et numéro de séquence, avec fenêtre anti-rejeu.
- Le pare-feu Windows est limité au profil Privé. En mode portable, la règle est créée par le helper élevé et retirée en sortie.

### 5.3 Découverte

mDNS `_screenlink._tcp.local`. Windows : `DnsServiceRegister` (intégré à l'OS), donc **aucune dépendance à Bonjour**, ce qui sert le portable. Android : `NsdManager`.

Annoncer uniquement sur l'interface LAN réelle : cette machine expose aussi VMnet1 et VMnet8 [V], qui seraient sinon annoncées.

### 5.4 Qualité adaptative

- Mesures remontées par Android toutes les 100 à 200 ms : débit reçu, gigue, pertes, profondeur de la file décodeur.
- Le contrôle agit dans cet ordre : débit encodeur, puis FEC, puis fréquence, puis résolution d'encodage. Android remet à l'échelle sans changer de mode Windows.
- Pas d'I-frames périodiques : intra refresh pour lisser le débit, et demande d'IDR uniquement sur perte non réparée. Pas d'images B.
- Bureau statique : envoi d'images de raffinement à faible QP. C'est le rôle du reencode count d'IddCx et cela rend le texte net après un mouvement.
- En file saturée, supprimer les images anciennes. La fraîcheur prime toujours sur l'exhaustivité.

---

## 6. Faisabilité des transports

Tous les chiffres sont des estimations **[E]**, sauf mention contraire.

### 6.1 Synthèse

| | USB (adb) | Wi-Fi 5/6 5 GHz | Wi-Fi 2,4 GHz | Bluetooth Classic | BLE 2M |
|---|---|---|---|---|---|
| Lien brut | 5 Gb/s sur cette tablette [V], 480 Mb/s avec un câble USB 2 | 400 à 1200 Mb/s | 50 à 150 Mb/s | 3 Mb/s [V] | 2 Mb/s [V] |
| Utile pour la vidéo | 150 à 300 Mb/s, plus en USB 3 si adb suit | 60 à 200 Mb/s | 10 à 25 Mb/s | 1 à 2 Mb/s | ~1,4 Mb/s (vendeur) |
| Latence bout en bout à 60 Hz | 30 à 50 ms | 40 à 70 ms | 60 à 150 ms | > 500 ms | > 500 ms |
| Stabilité | Excellente | Bonne, pics | Médiocre | Faible | Faible |
| 2560×1600 @ 60 | Oui | Oui | Dégradé | Non | Non |
| Friction | Débogage USB à activer | Même réseau | Même réseau | Appairage OS | Appairage OS |
| Statut | **Prioritaire** | **Phase 4** | Toléré, avec alerte | Pas de vidéo | Pas de vidéo |

Débits vidéo visés : ceux du tableau de la section 4.5, soit 50 à 80 Mb/s en résolution native à 60 Hz. Vérifier que le câble utilisé est bien USB 3 : le lien réel peut tomber à 480 Mb/s avec un câble de charge.

### 6.2 Budget de latence USB à 60 Hz

| Étape | Estimation | Levier |
|---|---|---|
| Composition jusqu'à disponibilité | 1 à 8 ms | Alignement sur le vsync IDD |
| Copie GPU | < 2 ms | Même GPU pour rendu et encodage |
| Encodage NVENC | 3 à 6 ms | Preset basse latence, slices |
| Transport | 1 à 3 ms | `TCP_NODELAY`, tampons courts |
| Décodage Android | 4 à 20 ms | **Très variable selon le SoC** |
| Attente de présentation | 0 à 16 ms | Rendu immédiat, 120 Hz si disponible |
| **Total** | **~ 10 à 55 ms** | Hors temps de réponse de la dalle |

Le poste le plus incertain est le décodeur. **[V]** `FEATURE_LowLatency` existe depuis Android 11, mais son effet dépend du pilote fourni par le fabricant du SoC. Sur le Dimensity 8300, des clés propres à MediaTek seront peut-être nécessaires.

Sur ce poste de travail, le PC est en Ethernet 1 Gb/s sans carte Wi-Fi [V] : le Wi-Fi n'aura qu'un seul saut radio (AP vers appareil). C'est une configuration favorable, mais qui exclut Wi-Fi Direct sans carte supplémentaire.

Point adb : `adb reverse` ne transporte que du TCP, donc le mode USB n'a pas de chemin UDP. Sur lien USB sans perte, ce n'est pas un problème. Le **partage de connexion USB** est une alternative sans mode développeur : réseau IP avec UDP, mais activation manuelle sur le téléphone.

### 6.3 Bluetooth : verdict

Le calcul est sans appel. Une image-clé 720p d'écran fait 50 à 100 ko [E], soit 400 à 800 kbit. À 1 Mb/s utile, sa seule transmission prend **0,4 à 0,8 s**, sans compter que tout mouvement écroulerait le débit. On obtiendrait au mieux un diaporama, sans intérêt comme moniteur, avec une complexité réelle (RFCOMM sous Windows, L2CAP CoC sous Android).

**Décision : pas de transport vidéo Bluetooth.** Rôle conservé, optionnel et postérieur à la v1 : détection de proximité, ou réveil du mode Wi-Fi sans ouvrir l'application. Aucun jalon v1 n'en dépend.

---

## 7. Plan de développement

Durées pour un développeur expérimenté à temps plein [E].

| Phase | Contenu | Durée | Jalon de sortie |
|---|---|---|---|
| **0. Toolchain et spikes** | VS 2022, WDK, Android Studio, adb, VM de test ; spikes S1 à S7 | 2 sem. | GO/NO-GO mesuré |
| **1. MVP USB + HDMI** | Dummy plug + Desktop Duplication + NVENC + TCP/adb + MediaCodec, gestion de la sortie HDMI | 4 à 6 sem. | Bureau étendu en 2944×1840@60, latence mesurée |
| **2. IDD maison** | Moniteurs dynamiques, EDID, modes, IOCTL, lifecycle, curseur | 4 à 5 sem. | Fonctionne sans plug, jusqu'à 144 Hz si le pipeline suit |
| **3. Produit Windows** | UI WPF, tray, helper, portable, tactile et stylet, appairage PIN/QR | 3 à 4 sem. | Portable fonctionnel sur un second PC |
| **4. Wi-Fi** | mDNS, UDP chiffré, NACK/FEC, qualité adaptative | 4 à 6 sem. | 60 fps stables 30 min sans gel > 200 ms |
| **5. Durcissement** | Texture partagée, bureau sécurisé, tests longue durée | 2 à 3 sem. | Session de plusieurs heures sans incident |
| **Optionnel** | AMF, oneVPL, Media Foundation, hybride laptop | 3 à 4 sem. | Seulement si un PC sans NVIDIA doit être pris en charge |

Total hors option : **4,5 à 6 mois** [E].

Sur ce PC [V] : Visual Studio, WDK (headers `IddCx` absents), .NET, CMake, adb, JDK et SDK Android sont à installer. Seuls Git et un SDK Windows partiel 10.0.26100 sont présents.

---

## 8. Spikes de la phase 0

| # | Question | Critère de succès | Si échec |
|---|---|---|---|
| S1 | Le sample IDD auto-signé se charge-t-il sur 26300, HVCI actif et sans mode test ? | Moniteur visible dans Paramètres, sur VM puis hôte | Mode HDMI, ou Virtual-Display-Driver |
| S2 | Une texture partagée entre `WUDFHost` (session 0, compte restreint) et l'agent fonctionne-t-elle ? | Lecture image à image, nommage `Global\` + SDDL | Garder Desktop Duplication + surveillance du bureau sécurisé |
| S3 | Le Dimensity 8300 décode-t-il 2944×1840@60 en HEVC, avec quelle latence ? | ≤ 50 ms USB, méthode caméra 240 fps | Passer à 2560×1600 ; tester H.264 |
| S4 | Quel est le débit réel de `adb reverse` ? | ≥ 100 Mb/s soutenus | Partage de connexion USB, ou AOA |
| S5 | Texte lisible en 4:2:0 ? | Validation visuelle à 1:1 sur la tablette | Augmenter le débit ; 4:4:4 seulement si le décodeur le permet |
| S6 | Le plug accepte-t-il 2944×1840@60, et Desktop Duplication le capture-t-il à 60 fps ? | Mode actif, 60 fps sans image manquée sur 5 min | Résolution personnalisée, puis repli 2560×1600 |
| S7 | L'agent peut-il couper et rétablir la sortie HDMI sans droits admin ? | `SetDisplayConfig` réussi en compte standard | Commande manuelle, ou mode IDD |

---

## 9. Risques

| Risque | Impact | Prob. | Parade |
|---|---|---|---|
| IDD auto-signé refusé sur 26300 ou un build futur | Moyen | Moyenne | Mode HDMI sans pilote |
| Décodage 2944×1840@60 trop lent sur le Dimensity 8300 | Élevé | Moyenne | Repli 2560×1600@60, spike S3 |
| Plug incapable de fournir le mode natif | Moyen | Moyenne | Plug programmable, résolution personnalisée, repli 2560×1600 |
| Port HDMI indisponible sur la carte | Moyen | Faible | Dummy plug DisplayPort, ou mode IDD |
| Fenêtres perdues sur l'écran HDMI après déconnexion | Moyen | Certaine sans gestion | Désactivation par l'agent, spike S7 |
| Compromission de la clé du certificat auto-signé | Élevé | Faible | Clé non exportable, contrainte de nom, retrait si inutilisé |
| Texte flou en 4:2:0 | Moyen | Élevée | Raffinement statique, débit élevé, pixels denses |
| Copie inter-GPU sur portable hybride | Moyen | Moyenne | `IddCxAdapterSetRenderAdapter` sur le GPU d'encodage |
| Contenu protégé (HDCP, DRM) noir | Faible | Certaine | Documenté, pas de contournement |
| Écran UAC absent du flux au MVP | Moyen | Certaine au MVP | Texture pilote en phase 5 |
| Injection refusée vers fenêtres élevées (UIPI) | Moyen | Certaine sans service | Service SYSTEM en version installée |
| Pilote USB adb manquant selon le fabricant | Moyen | Moyenne | Diagnostic guidé |
| Redistribution de `adb.exe` | Moyen | À vérifier | Recompiler depuis AOSP (Apache 2.0), ou téléchargement par l'utilisateur |
| Conflit avec Parsec ou autres IDD | Faible | Faible | ID matériel propre, test de cohabitation sur ce PC |
| Moniteur fantôme persistant | Moyen | Faible | Lifetime handle ; bouton de réparation |

---

## 10. Décisions à prendre avant le code

Décisions déjà prises : signature auto-signée pour un usage personnel, carte de capture exclue, tablette de référence Lenovo Idea Tab Pro, `minSdk` fixé à 34 (Android 14), NVENC seul en v1.

Reste à confirmer :

1. **Dummy plug :** présence d'un port HDMI libre sur la carte, et achat d'un plug HDMI 2.0, si possible à EDID programmable.
2. **Portable :** mode HDMI sans admin en priorité, IDD avec UAC à chaque session en complément.

**Notice de licence :** même en usage personnel, conserver la notice MS-PL du code dérivé du sample Microsoft.

---

## Sources

- [IDD model overview](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/indirect-display-driver-model-overview)
- [IddCx versions](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/iddcx-versions)
- [IddCx 1.10 updates](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/iddcx1.10-updates)
- [IddCx 1.11 updates](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/iddcx1-dot-11-updates)
- [Sample IndirectDisplay](https://github.com/microsoft/Windows-driver-samples/tree/main/video/IndirectDisplay)
- [Driver Code Signing Requirements](https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/code-signing-reqs)
- [Attestation Sign Windows Drivers](https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/code-signing-attestation)
- [The Windows Driver Policy](https://support.microsoft.com/en-us/windows/hardware/drivers/the-windows-driver-policy)
- [Trusted Signing et programme matériel, Q&A](https://learn.microsoft.com/en-us/answers/questions/5866910/hardware-program-verification-using-azures-trusted)
- [SwDeviceCreate](https://msdn.microsoft.com/en-us/library/Dn315038.aspx)
- [SwDeviceSetLifetime](https://learn.microsoft.com/en-us/windows/win32/api/swdevice/nf-swdevice-swdevicesetlifetime)
- [Virtual-Display-Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver)
- [Android low latency media](https://source.android.com/docs/core/media/low-latency-media?hl=en)
- [Bluetooth BR/EDR et LE](https://www.mathworks.com/help/bluetooth/gs/comparison-of-bluetooth-bredr-and-bluetooth-le.html)
- [Débit BLE, Ezurio](https://www.ezurio.com/resources/blog/common-questions-about-bluetooth-le-throughput-and-high-data-rate-applications)
- [Test Lenovo Idea Tab Pro, Notebookcheck](https://www.notebookcheck.net/Lenovo-Idea-Tab-Pro-review-The-large-tablet-with-a-pen-and-long-runtimes-costs-less-than-450.959695.0.html)
- [Fiche Idea Tab Pro, DP-Out, Moglix](https://www.moglix.com/lenovo-idea-tab-pro-tb373fu-luna-grey-tablet-with-8gb-ram-256gb-storage-wi-fi-quad-speaker-with-dolby-atmos-127-inch-3k-display-zae50127in/mp/msnv5ooprqm056)
- [MediaTek Dimensity 8300](https://www.mediatek.com/products/tablets/mediatek-dimensity-8300)
