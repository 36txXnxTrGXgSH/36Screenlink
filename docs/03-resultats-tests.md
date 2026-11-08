# Résultats de tests — 2026-10-31

## Campagne de validation et corrections 1.6

Corrections issues de la campagne du 8 novembre 2026 (fonctionnel, sécurité, performance). Tests sur le service installé, PC et tablette de référence, en USB. Les scripts sont ceux de la campagne : entrées A3, modes A5, moniteur A1, plus une charge vidéo plein écran sur l'écran virtuel.

| Vérification | Avant | 1.6 |
|---|---|---|
| Clavier physique branché puis retiré (A3.8) | 3 coupures de session | **0** |
| F5, Début, Page suivante, Ctrl+A sans code physique (A3.7) | 0 sur 4 | **4 sur 4** |
| Clic de souris branchée à la tablette (A3.13) | reçu comme toucher | **reçu comme souris** |
| Molette de souris (A3.14 et crochet souris Windows) | rien | **WM_MOUSEWHEEL, 3 crans** |
| Retour Étendu après Tablette seule (A5.5) | 60 Hz | **120 Hz** |
| Autres contrôles A3, A5, A1 | OK | OK, inchangés |
| PC sans saisie pendant 8 min, animation sur la tablette | image figée vers 300 s | **jamais figée**, demande d'affichage visible dans `powercfg /requests` |
| Latence sous charge à 60 Hz, 80 Mb/s | 45,2 ms, décodage 21,4 ms | **36,2 ms**, décodage 18,6 ms |
| Latence sous charge à 120 Hz | 25,7 ms | 26,3 ms |
| 144 Hz depuis le menu | impossible | **OK**, 24,6 ms |
| Media Foundation HEVC sous charge, textures en rotation | encodage 7,6 ms, 27,2 ms | 7,7 ms, 27,9 ms : sans régression |
| Capture pour 120 images envoyées | 145 à 165 i/s | 123 à 134 i/s |
| Poignée de main v5 avec clé éphémère | — | OK, appairage conservé |
| Journal daté, journal précédent gardé | — | OK |
| Certificats dans `LocalMachine\CA` (A1.13) | 11 | 1 |

**Corrigé grâce aux tests :**
- un plantage de l'application à sa fermeture : une interruption pendant l'arrêt du décodeur n'était pas rattrapée ;
- Retour fermait l'application d'un coup : Android 16 n'appelle plus l'ancienne méthode.

**Non testé :**
- confirmation d'identité avant l'UAC : il faut l'option à l'installation et une vraie invite ;
- famine du serveur de la tablette et curseur géant, sur l'appareil lui-même ;
- file d'envoi réduite sous vraie saturation Wi-Fi ;
- vraie souris (clic droit, survol) ;
- Alt+² au clavier physique ;
- multi-touch à 5 et 10 doigts ;
- GPU AMD et Intel.

## Menu en verre dépoli 1.5

Test sur la tablette de référence en USB, gestes simulés par adb. Cette tablette n'a pas le flou de fenêtre d'Android : le verre floute une copie réduite de l'image reçue, rafraîchie 10 fois par seconde.

| Vérification | Résultat |
|---|---|
| Flou et lisibilité sur un fond d'écran clair | OK |
| Rotation, Tactile | OK, états affichés |
| Fréquence 60 puis 120 Hz | OK, session relancée au bon mode |
| Dupliquer, retour Étendu | OK |
| Sortie son PC puis Tablette | Boutons actifs ; son **non écouté** |
| Touche Windows, Clavier | OK, menu Démarrer ouvert, clavier affiché |
| Lisibilité sur un fond sombre | OK |
| Toucher à côté, croix | OK, menu refermé |
| Épingle | OK, menu conservé |
| Icône Wi-Fi, Déconnecter, Bureau, portrait, Android 11 | **Non testés** |

## Fenêtres amenées sur la tablette 1.4.1

Test sur le service installé, touches simulées par adb sur l'icône de Bloc-notes dans la barre des tâches de la tablette.

| Vérification | Résultat |
|---|---|
| Fenêtre sur l'écran principal, pas au premier plan | OK, amenée sur la tablette et activée |
| Fenêtre sur l'écran principal, déjà au premier plan | OK, réduite par Windows puis rouverte sur la tablette |
| Fenêtre agrandie sur l'écran principal | OK, agrandie sur la tablette |
| Glissement long sur la tablette | OK, la fenêtre de l'écran principal ne bouge pas |
| Doigt réel, application lancée pour la première fois | **Non testé** |

**Image figée corrigée :** quand le décodeur de la tablette saturait, elle attendait une image clé, mais l'agent répondait par une image ordinaire. L'image restait figée pour toute la session, avec tactile actif. Constaté après une réinstallation, 48 images perdues au démarrage. Après correction, 51 pertes au démarrage, image reprise et statistiques normales.

## Quitter et encodeur Media Foundation 1.4.0

Test sur le PC et la tablette de référence, en USB, encodeur Media Foundation forcé par `--encoder mf`.

| Vérification | Résultat |
|---|---|
| HEVC par Media Foundation, session complète | OK, `NVIDIA HEVC Encoder MFT` |
| H.264 par Media Foundation, session complète | OK, `NVIDIA H.264 Encoder MFT` |
| Animation plein écran sur l'écran virtuel, HEVC | OK, 60 à 63 images/s, encodage 12 ms, latence 35 ms, P95 44 ms |
| Couleurs comparées à NVENC sur la même image | OK, luminosité moyenne 182,1 contre 182,0 |
| NVENC toujours choisi sur NVIDIA | OK |
| Icône du menu Démarrer, service arrêté | OK, service et agent relancés |
| Clic sur Quitter, service arrêté sans relance | Arrêt constaté ; cause **non confirmée** |
| Vrai GPU AMD, vrai GPU Intel, processeur avec partie graphique | **Non testé** : aucun matériel disponible |
| PC portable hybride | **Non testé** |

## Version portable et désinstallation 1.3.0

| Vérification | Résultat |
|---|---|
| Désinstallation : service, fichiers, données, raccourci | OK |
| Portable depuis un dossier, UAC, installation du pilote | OK |
| Appairage et flux en Wi-Fi sans adb | OK |
| Moniteur virtuel reconnu par son identité | OK sur un PC à un écran |
| Retirer le pilote et quitter : pilote, certificats, processus | OK, rien ne reste |
| Réinstallation après retrait | OK, après correction du réglage de registre |
| PC à deux écrans physiques | **Non testé** |
| Option UAC et déverrouillage cochée à l'installation | Installée ; validation par la tablette **non retestée** |
## Menu latéral et modes d'affichage 1.2.0

Test sur la tablette de référence, gestes simulés par adb, sur le service installé.

| Vérification | Résultat |
|---|---|
| Ouverture par glissement depuis le bord gauche | OK |
| Fréquence 60 puis 120 Hz | OK, session relancée au bon mode |
| Rotation verrouillée, tactile coupé | OK, états affichés |
| Dupliquer, retour Étendu | OK, moniteur retiré puis recréé |
| Tablette seule, retour par le menu | OK, écran du PC rétabli |
| Déconnexion en Tablette seule | OK, écran du PC revenu en 15 s, reconnexion normale |
| Touche Windows, Afficher le bureau, Déconnecter | **Non testés** |
| Clic sur l'icône du menu Démarrer | Raccourci présent ; validé en 1.4.0 |
## Installateur 1.1.0

Test de bout en bout sur le PC de référence, tablette réinstallée à zéro, avec `36Screenlink-Setup.exe`.

| Vérification | Résultat |
|---|---|
| Installation du pilote et du service sans script | OK, constaté dans le journal |
| Démarrage de l'agent par le service, sans UAC | OK |
| Clé illisible et dossier non modifiable par un compte standard | OK, vérifié depuis un compte non élevé |
| Aucune clé privée restante après signature | OK |
| Icône et menu du son sous SYSTEM | OK, constaté par l'utilisateur |
| Son sur la tablette, PC silencieux | OK, constaté par l'utilisateur |
| Passage automatique en Wi-Fi au retrait du câble | OK, constaté par l'utilisateur |
| Appairage en Wi-Fi après effacement de l'application | OK, constaté par l'utilisateur |
| Windows vierge, autre GPU NVIDIA, autre appareil Android | **Non testé** |
| Option UAC et déverrouillage avec l'installateur | **Non testée** depuis l'installateur |
| Désinstallation complète | **Non testée** |
## Écran étendu par le pilote IDD

Pilote installé par l'utilisateur en administrateur, avec HVCI actif, sans avoir demandé le mode test. Windows voit `Generic Monitor`, identifiant `DISPLAY\SXT0136`, sortie `\\.\DISPLAY21` en 2944×1840, échelle 200 %. Le rapport PnP indique `IsSigned=False` : le pilote se charge quand même ; il ne reflète pas les signatures hors Microsoft.

| Lien | Résolution | Images/s | Débit réel | Encodage + envoi | Décodage | Latence moy. | Latence P95 |
|---|---|---|---|---|---|---|---|
| USB | 2944×1840 | 59-60 | 52 Mb/s | 5 à 13 ms | 20 ms | **40 à 53 ms** | 47 à 65 ms |
| Wi-Fi | 2944×1840 | 59-60 | 52 Mb/s | 5 à 13 ms | 19 ms | **44 à 51 ms** | 50 à 63 ms |

Charge : rectangles aléatoires et texte défilant, à 60 Hz, sur **un quart de l'écran seulement**, car la fenêtre de test non adaptée au DPI a gardé 1472×920 pixels. Un contenu plein écran demandera plus de débit, et le lien USB plafonne à 65 Mb/s. Durée : 20 s.

| Vérification | Résultat |
|---|---|
| Bureau étendu visible sur la tablette, barre des tâches comprise | OK par capture d'écran |
| Curseur Windows sur la tablette | OK, position et forme |
| Injection tactile réelle | OK : (736,460) reçu à (736,460), (2200,1400) à (2199,1399), glissement reçu |
| Texte de la fenêtre de test | Lisible sur la capture, sans validation à l'œil sur la dalle |
| Pertes | 3 images en USB, 9 en Wi-Fi sur environ 1180 |

### Lancement intégré

| Vérification | Résultat |
|---|---|
| `Demarrer-USB.cmd`, invite UAC, assistant lancé par l'agent | OK |
| Moniteur retiré après arrêt de l'app tablette | OK, entre 10 et 15 s |
| Retour du moniteur et du flux à la reconnexion, sans UAC | OK, en moins de 5 s |
| Moniteur déjà présent : pas d'élévation demandée | OK |
| Arrêt forcé de l'agent : assistant et moniteur retirés | OK, en moins de 4 s |

Le menu de l'icône de notification et le lancement par `Demarrer-WiFi.cmd` n'ont pas été testés.

### Wi-Fi intégré

`Demarrer-WiFi.cmd`, USB branché mais tunnel adb retiré. Signal : RSSI −42 dBm, 1201 Mb/s.

| Vérification | Résultat |
|---|---|
| Lancement : UAC, moniteur, flux PC → tablette | OK |
| Coupure Wi-Fi de 4 s | Reprise seule sous 10 s environ, moniteur conservé |
| Coupure Wi-Fi de 25 s | Moniteur retiré, puis moniteur et flux revenus sous 10 s environ |
| Fin de test | Wi-Fi de la tablette réactivé, même adresse |

Charge plein écran en 2944×1840 pendant 25 s, capture confirmant toute la surface : 45 Mb/s mesurés sur l'Ethernet du PC, **latence 40 à 52 ms**, P95 54 à 66 ms, décodage 19 à 22 ms, 41 à 53 images/s reçues. Le débit reste sous les 60 Mb/s autorisés.

Seconde série de 18 s, après les coupures : la fenêtre produit 61,4 images/s et la tablette en reçoit **55 à 61, 58,3 en moyenne**. La cause de la baisse à 41-53 images/s de la première série n'est pas identifiée.

### Session longue

31 minutes en Wi-Fi, APK de version finale, charge plein écran 2944×1840, USB branché mais tunnel retiré, 101 500 images.

| Mesure | Résultat |
|---|---|
| Connexion | Aucune coupure sur les 60 relevés |
| Mémoire agent | 254,5 à 256,4 Mo, stable |
| Handles agent | 641 à 611, sans dérive |
| Mémoire pilote (`WUDFHost`, total) | 58,9 à 58,6 Mo, stable |
| Tablette | 30,5 à 29,9 °C ; batterie 59 à 55 % malgré l'USB |
| Ping tablette | 2 à 14 ms, 5,7 ms en moyenne |
| Latence moyenne | 48 ms, de 43 à 52 ms par tranche de 5 min ; P95 max 74 ms |
| Encodage | 8,1 ms, maximum 10,7 ms |
| Socket | 0,1 ms |
| Décodage | 19,6 ms |
| Images perdues | 148 sur 101 500, soit 0,15 % |
| Images envoyées | 53,9/s en moyenne pour 60,8/s capturées |

**Cause des baisses d'images :** l'écart entre capture et envoi vient du limiteur de cadence de l'agent, pas du réseau ni de NVENC. Il comparait le délai depuis la dernière image : une image un peu en avance attendait, puis la suivante l'écrasait. Corrigé par un échéancier et validé en Wi-Fi, sur l'écran étendu en charge plein écran : **60,1 images/s, minimum 58**, latence 44,3 ms, P95 maximum 61 ms, sur 44 s relevées d'un test de 90 s.

**`IsSigned=False` :** sans gravité. `pnputil` donne `36Screenlink Dev` comme signataire, catalogue et DLL sont `Valid`. Le certificat expire le 31/10/2029.

**Hausse d'encodage + envoi :** elle vient de l'encodage, pas du réseau.

### Performances, 2026-10-31 soir

Durée 22 à 35 s par test. La fenêtre de charge produit 76 à 86 images/s : elle limite le test, pas la chaîne. Pics observés à 130-140 images/s envoyées.

| Lien | Mode | Images/s stables | Débit | Encodage | Décodage | Latence moy. | P95 |
|---|---|---|---|---|---|---|---|
| Wi-Fi | 120 Hz | 80-95 | 30-35 Mb/s | 8 ms | 13 ms | **28 ms** | 33-37 ms |
| Wi-Fi | 144 Hz | 92-140 | 36-59 Mb/s | 4,4-6,5 ms | 11-12 ms | **21-26 ms** | 27-36 ms |
| USB | 120 Hz, charge lourde | 69-83 | **110-133 Mb/s** | 6-10 ms | 13-15 ms | **28-35 ms** | 34-49 ms |

- Ces charges ne sont pas comparables entre elles ni avec les 60 Hz, moins de rectangles et sans texte à 120 et 144 Hz.
- À 60 Hz, le décodage prenait 20 ms ; à 120 et 144 Hz, 11 à 14 ms. Le gain est net, sa cause n'est pas identifiée. **120 Hz et 80 Mb/s deviennent les réglages par défaut.**
- **S4 corrigé :** les 62-65 Mb/s mesurés venaient de `adb exec-out`. Le tunnel `adb forward` a tenu 133 Mb/s, avec 0,2 à 0,3 ms sur la socket. Le câble actuel suffit.
- Effet sur la batterie et la chauffe à 120 Hz : non mesuré.

**Débit adaptatif :** baisse de 25 % par seconde en cas de saturation, jusqu'à 15 Mb/s, puis hausse de 25 % toutes les 3 s de calme. Signaux : envoi socket supérieur à une demi-image, ou P95 tablette au-dessus de 150 ms.

- Testé avec une saturation **simulée** en miroir (`--simulate-congestion`) : 80 → 18 Mb/s, puis remontée, sans plantage. Cette remontée a été mesurée à l'ancien rythme, plus lent.
- Le débit réel est resté à 6-10 Mb/s après la baisse, alors que la cible remontait. Sur 38 s et ce contenu, impossible de dire si l'encodeur restait trop prudent.
- Saturation réelle : **non testée**, aucun lien n'a saturé.

**UDP : reporté, sur mesures.** La socket prend 0,1 à 0,3 ms, le ping vaut 5,7 ms en moyenne, et 31 minutes n'ont montré aucune coupure. Les images perdues viennent de la file du décodeur, pas du réseau. Un protocole avec NACK et FEC n'apporterait rien de mesurable ici. À rouvrir si des gels apparaissent avec un signal dégradé.

### Stylet et portrait

Test en USB sur l'écran étendu, fenêtre de test lisant les messages `WM_POINTER`, stylet simulé par `adb shell input stylus`.

| Vérification | Résultat |
|---|---|
| Windows reçoit un vrai stylet (`PT_PEN`) | OK, appui, relâchement, 30 mouvements |
| Pression transmise | OK : 1024 au contact, simulation à pression maximale |
| Position en paysage | OK : (736,460) exact |
| Rotation tablette vers portrait et retour | OK, écran Windows pivoté, flux repris en moins de 2 s |
| Moniteur pendant la rotation | Conservé, même `DISPLAY` |
| Image en portrait | OK par capture : complète, à l'endroit, cadrée |
| Position en portrait | OK : (460,736) exact, (1400,2600) reçu à (1399,2599) |
| Inclinaison, survol, bouton, gomme, pression variable | **Non testés** : à valider avec le Tab Pen Plus |
| Curseur de souris en portrait | **Non testé** |
| Portrait en Wi-Fi | Non testé ; même chemin que l'USB |

En rotation, DXGI annonce le bureau pivoté mais la texture reste en 2944×1840 : l'agent encode ce format et la tablette applique 90°.

Lors d'un premier essai, une reconnexion a expiré après 5 s, sans cause identifiée ; le problème ne s'est pas reproduit sur 3 essais suivants.

### Service SYSTEM

Service `36Screenlink` installé par l'utilisateur, agent en session 1 lancé par le service, liaison automatique USB ou Wi-Fi, sans invite UAC au démarrage.

| Vérification | Résultat |
|---|---|
| Moniteur et flux lancés par le service | OK |
| UAC détectée, bascule sur l'écran principal, retour à l'écran étendu | OK, reconnexions automatiques |
| Stylet vers une fenêtre administrateur | OK, position exacte |
| Toucher vers une fenêtre administrateur | OK |
| Clavier vers une fenêtre administrateur | OK |
| Invite UAC visible sur la tablette | OK par capture d'écran, y compris en portrait |
| Validation de l'UAC au doigt depuis la tablette | **Non prouvé** : l'invite a été acceptée avec élévation réelle, mais `touch=0` pendant cette session. Des touchers ont atteint le PC lors de sessions précédentes, sans preuve qu'ils aient validé l'invite |
| Écran de verrouillage dès Win+L, déverrouillage depuis la tablette | OK, constaté par l'utilisateur |

**Sécurité :** avec le service, toute personne tenant la tablette, app ouverte et session Android déverrouillée, peut valider une UAC et saisir sur l'écran de verrouillage. Le flux reste chiffré. Ctrl+Alt+Suppr n'est pas transmis.

### Clavier

Test par `adb shell input`, en USB, vers une fenêtre de test ayant le focus.

| Vérification | Résultat |
|---|---|
| Texte, espace, retour arrière, Entrée, flèche | OK : texte reçu exact, 24 messages |
| Touches physiques, Ctrl, Alt, disposition AZERTY | **Non testé** : aucun clavier physique branché |
| Bouton ⌨, ouverture et fermeture du clavier à l'écran | OK |
| Saisie réelle au clavier à l'écran, suggestions | **Non testé** |
| Fenêtres élevées | Non testé ; refus attendu (UIPI) |

Les sections suivantes décrivent la première série, en miroir, avant installation du pilote.

Matériel : RTX 3080 (pilote 610.88), Windows 11 build 26300, Ethernet 1 Gb/s ; Lenovo Idea Tab Pro TB373FU, Android 16 (API 36), Dimensity 8300 (`mt6897`).

**Conditions :** capture de l'écran principal 3440×1440 en **miroir**, HEVC, 60 Mb/s maximum, contenu peu animé. Ni écran étendu réel, ni résolution native 2944×1840, ni contenu lourd n'ont été testés. Durées courtes : 20 à 25 s.

## Tablette, relevé sur l'appareil

| Élément | Mesure |
|---|---|
| Modes d'écran | 144, 120, 60 et 30 Hz |
| Décodeurs | `c2.mtk.hevc.decoder.lowlatency`, équivalents AVC et AV1 |
| Wi-Fi | 802.11ax, 5560 MHz, RSSI −32 à −35 dBm, 600 à 1201 Mb/s en émission |

## Flux

La latence va de la présentation Windows à la sortie du décodeur, **sans l'affichage de la dalle**. Les valeurs sont celles du régime stabilisé.

| Lien | Images/s | Débit réel | Encodage + envoi | Décodage | Latence moy. | Latence P95 |
|---|---|---|---|---|---|---|
| USB, adb | 60 | 13 Mb/s | 12 ms | 13 ms | **33 ms** | 40 à 45 ms |
| Wi-Fi 5 GHz, TCP | 60 | 13 Mb/s | 9 ms | 20 ms | **41 ms** | 47 à 51 ms |

## Fonctions

| Test | Résultat |
|---|---|
| Appairage USB, session chiffrée | OK |
| Découverte mDNS sans adresse saisie | OK |
| Chemin Wi-Fi réel, tunnel adb retiré | OK, connexion PC → tablette |
| Image sur la tablette | OK par capture d'écran : nette, proportions correctes |
| Dossier portable, nouvelle clé renvoyée à la tablette | OK |
| Arrêt forcé de l'app puis reconnexion automatique | OK, 60 fps repris |
| Tactile reçu par l'agent | OK, 23 événements, injection désactivée |
| Icône de notification | Processus actif et session établie ; menu non cliqué |

## Spikes

| # | Résultat |
|---|---|
| S1 IDD auto-signé | **Réussi** : moniteur actif sur le build 26300 avec HVCI |
| S2 Texture partagée | Non testé, reporté |
| S3 Décodage natif | **Réussi à 60 Hz** : 20 ms en 2944×1840 HEVC ; 120 et 144 Hz non testés |
| S4 Débit adb | **Réussi** : 133 Mb/s via `adb forward`. Le premier résultat, 62-65 Mb/s, venait de `adb exec-out` |
| S5 Texte 4:2:0 | Non validé à l'œil |
| S6, S7 Dummy plug | Non testés, pas de plug |

## Points ouverts

- **Hausse d'encodage + envoi :** de 4 à environ 12 ms pendant les 15 premières secondes, en USB comme en Wi-Fi, puis stable. Cause non identifiée ; le partage entre encodage et envoi n'est pas mesuré.
- **Démarrage à 0 fps :** lors du test Wi-Fi, environ 10 s sans image au début. Compatible avec un écran immobile, car rien n'est encodé sans changement, mais non prouvé.
- **Injection tactile réelle :** validée ensuite sur l'écran étendu, voir la première section.
