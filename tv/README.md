# 36Screenlink — application téléviseur (Samsung Tizen)

Affiche l'écran du PC sur un téléviseur Samsung sous Tizen 5.5 ou plus récent, à faible latence
(environ 80 ms de bout en bout, 1080p à 60 images/s, débit plafonné à 30 Mb/s), sur le réseau local,
en flux chiffré.

Le téléviseur joue le rôle d'un troisième écran « tablette » dans le protocole de 36Screenlink :
l'agent du PC lui envoie un flux H.264, et c'est le téléviseur qui se connecte au PC (une page web
ne peut pas écouter le réseau).

## Pourquoi WebAssembly, et pas le lecteur web habituel

Media Source Extensions (le lecteur vidéo du navigateur) garde toujours une réserve d'environ une
demi-seconde avant d'afficher : impossible de descendre sous ~350 ms sur ce téléviseur. Le lecteur
natif bas niveau (ESPlayer, en .NET) donnerait une latence bien plus faible, mais il exige un
certificat de **partenaire Samsung** que seul un compte entreprise obtient.

La solution retenue n'exige aucun certificat spécial : un petit module **C++ compilé en WebAssembly**
qui pilote l'**Elementary Media Stream Source** (lecteur WASM de Samsung) en **mode faible latence**.
Les images H.264 vont directement au décodeur matériel, sans tampon. Appairage, chiffrement et
réseau restent en JavaScript ; le module ne fait que décoder.

## Structure

| Fichier | Rôle |
|---|---|
| `config.xml` | Manifeste du widget Tizen (identifiant, privilèges, orientation) |
| `index.html` | Balise vidéo, curseur, panneau d'état ; charge le module WASM puis `js/app.js` |
| `js/bytes.js` | Lecture/écriture d'octets, flux binaire |
| `js/secure.js` | Appairage (ECDH + code), session chiffrée (AES-256-GCM) |
| `js/app.js` | Découverte du PC, réseau, télécommande, curseur, et alimentation du lecteur WASM |
| `wasm/player.cc` | Source C++ du lecteur (Elementary Media Stream Source, faible latence) |
| `wasm/player.js`, `wasm/player.wasm` | Module compilé |
| `wasm/build-player.ps1` | Compile le module |
| `build-tv.ps1` | Paquet `.wgt` non signé (pour signature par Apps2Samsung) |
| `deploy-tv.ps1` | Signe, installe et lance l'application sur le téléviseur par `sdb` |

## Télécommande

Testé avec la Smart Remote Samsung 2020. Codes reçus : croix 37 à 40, OK 13, Retour 10009,
Lecture/Pause 10252, Volume 447/448, Muet 449, Chaîne 427/428, appui sur Chaîne 458 (« Guide »).
Le téléviseur laisse une application réserver 46 touches (`tizen.tvinputdevice.getSupportedKeys()`),
mais ni Netflix, ni Prime Video, ni Rakuten, ni Accueil, ni micro, ni mode Ambiant, ni 123 : vérifié
sur le téléviseur, `registerKey` les refuse sous tous les noms essayés, et leur appui n'envoie aucun
événement à l'application (seulement une perte de focus, ou le passage en arrière-plan). Même les
chiffres et couleurs du pavé affiché par la touche 123 n'arrivent pas jusqu'à elle.

L'appui sur le bouton Chaîne (« Guide », 458) est donc la seule touche libre de la Smart Remote :
il ouvre le menu pendant l'affichage du PC, et autorise un nouvel appairage le reste du temps.

- Volume, chaînes et touches média ne sont réservés que pendant l'affichage du PC : hors session,
  ils reviennent au téléviseur. Guide, rouge et Info restent réservés tant que l'application est ouverte.
- La télécommande n'envoie qu'un appui et un relâchement, sans répétition : déplacement continu du
  pointeur, molette et touches répétées sont gérés par `app.js`.
- Le PC reçoit les messages existants du protocole : `kMouse` (position absolue 0..65535 sur l'écran
  du téléviseur), `kKey` (touches virtuelles Windows), `kText` (saisie) et `kControl` (affichage).
  L'agent les ignore sur l'UAC et l'écran de verrouillage, comme pour la tablette.

## Détails du lecteur WASM

- Mode **faible latence** (`EmssLatencyMode::kLow`). L'ultra-faible latence n'existe pas sur les
  téléviseurs de 2020 (EMSS « hérité ») ; le mode faible latence y est la meilleure option.
- La source ne passe en lecture qu'après l'appel à `HTMLMediaElement::Play()` : on l'appelle dès
  l'ouverture, sans attendre `OnCanPlay`.
- `AppendPacket` est interdit sur le fil principal ; on utilise `AppendPacketAsync`, qui lui est
  autorisé (le module reste donc mono-thread, ce que le moteur WebAssembly du téléviseur impose :
  il refuse les modules multi-thread).
- Les images H.264 sont envoyées en Annex-B avec SPS/PPS en ligne sur chaque image clé ; l'`extradata`
  de la configuration de piste reste vide.
- Aucune image clé périodique : le transport est fiable (TCP) et ce lecteur n'a pas de tampon à vider.
  Une image clé n'est demandée qu'au démarrage et quand le téléviseur refuse une image.
- L'encodeur du PC est déjà réglé en faible latence sans image B, ce qu'exige le mode faible latence.

Quatre pièges, et leur parade :

1. **`AppendPacketAsync` ne copie pas les octets.** La glue JavaScript du SDK transmet une *vue* sur la
   mémoire du module (`new Uint8Array(HEAPU8.buffer, ptr, taille)`), lue plus tard par le téléviseur.
   Réutiliser un même tampon mélange deux images, corrompt les références, et l'image se dégrade
   jusqu'à l'illisible. Parade : chaque image est écrite dans un anneau de 32 Mo (`player.cc`), réécrit
   seulement environ 4 s plus tard ; la mémoire du module est fixe (64 Mo), car une croissance
   remplacerait le tampon et invaliderait les vues en cours.
2. **L'horloge du lecteur s'arrête dès qu'aucune image n'arrive.** Avec des horodatages calqués sur la
   capture du PC, chaque micro-trou du réseau les met en avance sur l'horloge, et ce retard s'additionne
   sans fin (jusqu'à plusieurs secondes). Parade (`app.js`) : l'horodatage suit la progression réelle,
   mais jamais plus de 35 ms devant `video.currentTime`, et jamais en arrière.
3. **Les envois refusés ne remontent que comme promesses rejetées.** `app.js` les intercepte
   (`unhandledrejection`) et repart sur la prochaine image clé, au lieu de laisser l'erreur se propager.
4. **Le lecteur ne fusionne pas les images qui ne se touchent pas.** Avec la durée nominale (1/60 s),
   chaque image arrivée à un rythme irrégulier formait sa propre plage dans `video.buffered`, jamais
   libérée : environ 6 000 plages après une heure, un fil principal figé jusqu'à une seconde à chaque
   ajout, et une latence qui montait jusqu'à plusieurs secondes, même à 1 Mb/s. Parade : chaque image
   est déclarée longue de 250 ms (`SPAN` dans `app.js`) ; elle recouvre l'écart jusqu'à la suivante
   et le tampon reste une seule plage. Le journal du téléviseur affiche `plages` et `blocage`.

Côté PC, la latence de la télévision redevient un signal de régulation : au-delà de 250 ms (P95),
l'agent baisse le débit d'un quart et attend 3 s ; il remonte de 10 % après 5 s sous 180 ms. Mesuré
sur une vidéo en mouvement : environ 27 Mb/s en moyenne, latence médiane 82 ms (73 à 97), stable.

Le débit est plafonné à 30 Mb/s : le port réseau du téléviseur est limité à 100 Mb/s, et à 60 Mb/s
chaque image mettait environ 10 ms à le traverser (latence médiane 92 ms au lieu de 82), pour un
gain visuel négligeable en 1080p.

Budget mesuré : environ 16 ms de la capture au téléviseur (encodage, réseau ; le déchiffrement ne
coûte que 1,2 ms), puis environ 65 ms dans le lecteur vidéo du téléviseur. Cette seconde part est
fixe sur ce modèle : réduire l'avance des horodatages (de 35 ms à 0) ou déclarer dans le SPS l'absence
de réordonnancement (`max_dec_frame_buffering = 1`, désormais émis par l'agent) n'y change rien.

## Compilation du module

Il faut l'**Emscripten de Samsung** (extensions Tizen), qui fournit les en-têtes `samsung/wasm` et
le drapeau `-s ENVIRONMENT_MAY_BE_TIZEN`. Téléchargeable sur developer.samsung.com (section
WebAssembly). Deux ajustements sont nécessaires dans ce SDK (version 1.39.4.7) :

1. `tools/system_libs.py` : compiler `lib_tizen_emss` et `lib_tizen_tv_wasm` en `-std=c++17`
   (au lieu de `c++14`), sinon une lambda `constexpr` de la bibliothèque ne compile pas.
2. Un fichier `~/.emscripten` valide, pointant `LLVM_ROOT`, `BINARYEN_ROOT` et `NODE_JS` vers le SDK.

Ensuite :

```powershell
powershell -ExecutionPolicy Bypass -File tv\wasm\build-player.ps1 -Emsdk <dossier emsdk>
```

## Déploiement sur le téléviseur

Le téléviseur doit être en **mode développeur** (Apps > paramètres), avec l'IP du PC autorisée, et
joignable par `sdb`. Le kit Tizen en ligne de commande (Tizen Studio) fournit `sdb` et `tizen`.

```powershell
powershell -ExecutionPolicy Bypass -File tv\deploy-tv.ps1 -Tv <IP du téléviseur> -Tizen <dossier du kit Tizen>
```

Le script crée un profil de signature, signe le widget, l'installe par `sdb` et le lance. La première
fois, l'application affiche un code : comparez-le avec celui du PC et confirmez avec **OK** sur la
télécommande.

Remarque : les téléviseurs du commerce bloquent la désinstallation par ligne de commande. Pour retirer
une application de test, supprimez-la depuis le menu Applications du téléviseur.
