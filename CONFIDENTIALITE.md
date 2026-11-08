# Confidentialité

36Screenlink relie votre PC et votre appareil Android, et rien d'autre.

## Ce qui circule

| Donnée | Trajet | Conservée |
|---|---|---|
| Image de l'écran du PC, curseur | PC → appareil, chiffrée | Non : affichée puis oubliée |
| Son du PC, si vous le dirigez vers l'appareil | PC → appareil, chiffré | Non |
| Touchers, stylet, souris, frappes | Appareil → PC, chiffrés | Non : rejoués dans Windows, jamais écrits dans un journal |
| Clé d'appairage | Jamais transmise | Oui, chiffrée : DPAPI sur le PC, Android Keystore sur l'appareil |
| Dernière adresse Wi-Fi de l'appareil | Locale | Oui, sur le PC, pour se reconnecter |

Aucun serveur, aucun compte, aucune télémétrie. Le flux ne quitte jamais votre réseau local ou votre câble USB.

## Ce qu'il faut savoir

- **L'écran partagé montre tout ce qu'affiche le PC** : messages, documents, mots de passe visibles. Pensez à qui peut voir l'appareil.
- **Journal du PC** (`C:\ProgramData\36Screenlink\logs`) : heures de connexion, débit, latence, taille de l'écran de l'appareil. Ni image, ni texte saisi. Seuls le journal en cours et le précédent sont gardés.
- **Option « Validation UAC et déverrouillage »** : décochée par défaut. Activée, l'appareil demande son code, son empreinte ou son visage avant toute saisie sur l'écran sécurisé du PC.

## Effacer les données

- Sur l'appareil : *Oublier ce PC* sur l'écran d'attente efface la clé d'appairage ; désinstaller l'application efface tout.
- Sur le PC : la désinstallation retire le pilote, le service, les certificats, la clé d'appairage et les journaux.
