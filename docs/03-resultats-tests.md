# Résultats de tests — 2026-10-31

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
