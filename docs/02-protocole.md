# Protocole v5

Implémentation : `windows/agent/src/protocol.h`, `channel.cpp` côté PC, `Protocol.kt`, `StreamServer.kt`, `Crypto.kt` côté tablette.

## Rôles et transport

La **tablette écoute** sur TCP 7236 et le **PC se connecte**, en USB comme en Wi-Fi. Le PC n'a donc besoin d'aucune règle de pare-feu entrante.

| Lien | Chemin |
|---|---|
| USB | `adb forward tcp:7236 tcp:7236`, puis connexion à `127.0.0.1:7236` |
| Wi-Fi | Découverte mDNS `_screenlink._tcp`, repli sur la dernière adresse connue |

Entiers en big-endian.

## Appairage

Possible en USB comme en Wi-Fi. Le code comparé des deux côtés écarte un intrus du réseau ou une application locale. Une tablette déjà appairée refuse le mode appairage tant que l'utilisateur n'a pas touché *Nouvel appairage*.

```text
PC → "S36X" u8 version, u8 mode=1, nonceC[16], pubA[64]
T  → "S36X" u8 statut, pubT[64]
       secret = ECDH P-256, coordonnée X big-endian
       T = nonceC | pubA | pubT
       code = u32(HMAC(secret, "sas"|T)) % 1 000 000, affiché des deux côtés
       psk = HMAC(secret, "psk"|T)
PC → HMAC(psk, "cli"|T), après confirmation sur le PC
T  → "S36X" u8 statut, HMAC(psk, "srv"|T), après confirmation sur la tablette
```

La clé n'est jamais transmise. Elle est stockée sous DPAPI sur Windows et chiffrée par Android Keystore sur la tablette. Une clé refusée n'est jamais renvoyée : il faut un nouvel appairage confirmé.

## Session

Chaque session ajoute un échange ECDH P-256 éphémère à la clé d'appairage : une clé d'appairage volée plus tard ne déchiffre pas une session enregistrée avant.

```text
PC → "S36X" u8 version, u8 mode=0, nonceC[16], pubA[64]   (clé éphémère)
T  → "S36X" u8 statut, nonceS[16], pubT[64], HMAC(psk, "srv"|C)
       C = nonceC | nonceS | pubA | pubT
       e = ECDH(éphémères), coordonnée X big-endian
PC → HMAC(psk, "cli"|C)
```

Clés AES-256-GCM : `HMAC(psk, "a2t"|C|e)` du PC vers la tablette, `HMAC(psk, "t2a"|C|e)` en sens inverse. Les deux MAC couvrent les clés éphémères, donc un intermédiaire ne peut pas les remplacer. La tablette vérifie que chaque point reçu est sur la courbe.

Chaque connexion s'authentifie sur son propre thread côté tablette, avec au plus 2 connexions en attente par adresse (4 en local, 16 en tout) : des connexions muettes ne retardent plus le vrai PC. Le PC ne propose un appairage que si la tablette répond « non appairée » ou présente une autre clé, et au plus une fois par minute.

Statuts : 0 OK, 1 non appairée, 2 mauvaise version, 3 appairage refusé.

## Enregistrements

```text
u32 longueur | AES-256-GCM(message) | tag 16 octets
```

Nonce de 12 octets : 4 octets nuls, puis compteur 64 bits propre à chaque sens. Taille maximale : 8 Mio vers la tablette, 256 Kio vers le PC, refusée avant toute allocation. Tout échec d'authentification coupe la session. Côté tablette, chaque message est vérifié avant lecture : longueur, dimensions de 16 à 8192, rotation, codec, curseur d'au plus 256 × 256.

## Messages

| Id | Nom | Sens | Contenu |
|---|---|---|---|
| 1 | HELLO | T→PC | u16 w, u16 h, u16 fpsMax, u8 codecs (bit 0 AVC, bit 1 HEVC) |
| 2 | STREAM_CONFIG | PC→T | u8 codec, u16 w, u16 h, u16 fps, u32 débit, u16 rotation en degrés |
| 3 | VIDEO | PC→T | u8 drapeaux (1 config, 2 clé), u64 ptsUs, Annex-B |
| 4 | TOUCH | T→PC | u8 n, n × (u8 id, u8 appui, u16 x, u16 y) ; x et y sur 0..65535 |
| 5 | PING | PC→T | u64 t0 |
| 6 | PONG | T→PC | u64 t0, u64 horloge tablette |
| 7 | STATS | T→PC | u32 images, latMoy, latP95, décodageMoy, pertes cumulées |
| 8 | REQUEST_IDR | T→PC | vide |
| 9 | CURSOR_POS | PC→T | u8 visible, i16 x, i16 y |
| 10 | CURSOR_SHAPE | PC→T | u16 w, h, hotX, hotY, BGRA |
| 11 | CLOCK | PC→T | i64 décalage (tablette − PC) en µs |
| 12 | KEY | T→PC | u8 appui, u16 code (scancode set 1, 0x100 = préfixe E0 ; 0x8000 \| code virtuel Windows pour une touche sans code physique) |
| 13 | TEXT | T→PC | Texte UTF-8 du clavier à l'écran |
| 18 | CONTROL | T→PC | u8 réglage (1 affichage, 2 fréquence : 60, 120 ou 144), u8 valeur |
| 19 | CONTROL_STATE | PC→T | u8 mode d'affichage (0 étendu, 1 dupliqué, 2 tablette seule), u8 fréquence |
| 15 | AUDIO | PC→T | u32 fréquence, PCM stéréo 16 bits little-endian |
| 16 | AUDIO_ROUTE | T→PC | u8 sortie demandée (0 PC, 1 tablette) |
| 17 | AUDIO_STATE | PC→T | u8 sortie active (0 PC, 1 tablette) |
| 20 | SECURE_STATE | PC→T | u8 1 si l'écran montré est l'UAC ou le verrouillage |
| 21 | SECURE_ARMED | T→PC | vide : identité confirmée sur la tablette, la saisie est acceptée sur l'écran sécurisé |
| 22 | MOUSE | T→PC | u8 boutons (1 gauche, 2 droit, 4 milieu), u16 x, u16 y, i16 molette verticale, i16 horizontale (crans de 120) |
| 14 | PEN | T→PC | u8 état (1 portée, 2 contact, 4 bouton, 8 gomme), u16 x, u16 y, u16 pression 0..1024, i8 tiltX, i8 tiltY, u16 rotation |
| 23 | KEYBOARD_STATE | T→PC | u8 drapeaux (1 clavier à l'écran affiché, 2 ouverture automatique) ; le PC ne suit le focus que si l'un des deux est actif |
| 24 | TEXT_FOCUS | PC→T | u8 1 si un champ de saisie modifiable a le focus sur l'écran de l'appareil, u16 x, y, w, h du curseur ou du champ (0..65535) |
| 25 | SCREENS | PC→T | u8 n, u8 index de l'appareil, n × (i16 x, i16 y, u16 w, u16 h) en pixels du bureau, puis la barre des tâches de l'appareil u16 x, y, w, h (0..65535). n = 0 quand le glisser d'icône n'a pas de sens |
| 26 | WINDOW_MOVE | T→PC | u16 x, u16 y de l'icône touchée (0..65535), u8 index de l'écran choisi |

Les messages 23 à 26 ont été ajoutés sans changer de version : une ancienne application ou un ancien agent les ignore, et le reste fonctionne.

Le PC numérote les écrans ainsi : l'écran principal d'abord, puis de gauche à droite. Il envoie SCREENS au début de la session et à chaque changement, au plus une fois par seconde. Pour WINDOW_MOVE, il simule une touche sur l'icône. La fenêtre qui s'active ensuite part sur l'écran choisi, y compris celle choisie dans les miniatures, pendant 8 s.

À un changement de taille d'écran (orientation), la tablette ferme la session ; un clavier branché ou le mode sombre ne la coupent plus. Le PC se reconnecte et lit la nouvelle orientation dans HELLO : hauteur supérieure à la largeur signifie portrait.

Un ping part toutes les 250 ms. Seul l'aller-retour le plus court met à jour le décalage d'horloge. Sans réponse pendant 5 s, le PC coupe et se reconnecte.

## Latence mesurée

`latence = horloge tablette à la libération du décodeur − décalage − pts`, où `pts` est l'heure de présentation Windows. La mesure exclut donc le temps d'affichage de la dalle.

## Limites connues

- Le Wi-Fi utilise TCP. La variante UDP avec NACK/FEC reste à faire ; TCP ajoute de la latence en cas de perte radio.
- QR code non implémenté : l'appairage repose sur un code à comparer.
