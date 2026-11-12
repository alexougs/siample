# Siample

Sampler-découpeur pour FL Studio (Mac et Windows) et Logic Pro : tu charges jusqu'à quatre samples, Siample les découpe en tranches, les cale sur le tempo de ton projet, et tu joues ces tranches sur 16 pads façon MPC ou au clavier MIDI. Une vue Montage permet d'assembler les tranches de plusieurs samples sur une grille, d'écouter le résultat et de l'exporter.

Formats produits : VST3 (FL Studio Mac et Windows), Audio Unit (Logic Pro), et une application autonome pour tester sans DAW.

## Ce que fait la version 0.5

**Quatre samples en même temps**
- Les onglets **A, B, C, D** portent chacun un sample, avec sa découpe, son tempo, sa tonalité et ses réglages. Les pads et le panneau de réglages montrent le sample affiché.
- Chargement par glisser-déposer, par le bouton « Charger » ou depuis le **navigateur de samples** (WAV, AIFF, FLAC, OGG, MP3, 15 minutes maximum).

**Découpe**
- Découpe automatique par **transitoires** (réglage de sensibilité), par **grille** (2 à 64 parts égales, ou mesures, noires, croches, doubles d'après le tempo), ou **manuelle**.
- **Zone de travail** : sur un morceau entier, tu traces dans le bandeau turquoise la partie à découper.
- Jusqu'à **64 tranches** par sample, sur 4 banques de 16 pads. Chaque tranche a sa couleur, la même sur la forme d'onde, les pads et le montage.
- **Niveler** met toutes les tranches au même niveau.

**Tempo et tonalité**
- **Détection du tempo** de chaque sample, corrigeable à la main (clic sur la valeur, ÷2, ×2, Auto).
- **Sync** : les tranches sont étirées pour suivre le tempo du projet, sans changer de hauteur. À activer sur chaque sample pour qu'ils tombent ensemble.
- **Tonalité** estimée du sample, et transposition de tout le sample en demi-tons, ou directement vers la tonalité de ton morceau (menu « Vers… »).

**Jeu**
- **Pads** : une note par tranche. **Clavier** : tout le clavier joue la tranche sélectionnée. **Mono** : chaque note coupe la précédente.
- **Vélocité fixe** : toutes les notes jouent à fond. **Répétition** (note repeat) : un pad tenu se répète au 1/4, 1/8, 1/16 ou 1/32, calé sur le tempo.
- Par tranche : volume, pan, pitch et accordage fin, **filtre** passe-bas / passe-haut avec **résonance**, **drive** (saturation), inversion, **enveloppe** dessinée en courbe, groupe de choke, et trois façons de jouer : **one-shot**, **gate** ou **boucle**, avec un **fondu** réglable au point de bouclage.
- **Grain** : le son granuleux des vieux samplers, réglable de 0 à 100 %.

**Montage**
- Une ligne par sample, une grille en mesures au tempo du projet (1 à 16 mesures, aimantation au 1/4, 1/8, 1/16, 1/32 ou libre).
- Tu glisses des pads sur le montage, tu déplaces les blocs, tu tires leur bord droit pour changer leur durée, tu superposes les samples.
- **Dans l'ordre** pose la boucle du sample affiché sur sa ligne ; **Flip** la réarrange au hasard en gardant le rythme.
- Trois **variantes** (1, 2, 3) pour comparer des montages.
- **Écouter** joue le montage en boucle. **Glisser l'audio** le dépose en WAV dans le DAW, au tempo du projet ; **Glisser le MIDI** le dépose en notes.

**Et aussi**
- **Annuler / Rétablir** sur la découpe, les réglages, les chargements et le montage.
- **Presets** : tout l'état du plugin dans un fichier `.siample` (menu).
- **Export** d'une tranche en audio (bouton « Glisser l'audio » du panneau de la tranche) ou de toutes les tranches d'un sample en WAV numérotés (menu).
- **Navigateur de samples** : ton dossier, pré-écoute d'un clic, chargement d'un double-clic.

Tout est enregistré dans le projet du DAW. Les projets des versions précédentes s'ouvrent : leur sample devient le sample A, et la « piste » de la 0.4 devient des blocs du montage.

## Récupérer le plugin compilé

Le plugin est compilé automatiquement par GitHub à chaque envoi de code.

1. Sur la page du dépôt, ouvre l'onglet **Actions**.
2. Clique sur la compilation la plus récente (coche verte). Compte 10 à 20 minutes après un envoi.
3. En bas de la page, dans **Artifacts**, télécharge `Siample-macOS.zip` ou `Siample-Windows.zip`.
4. Décompresse l'archive et suis le fichier `LISEZMOI.txt` qu'elle contient. Sur Windows, l'archive contient un installeur (`Installer Siample.exe`) qui fait tout.

Pour relancer une compilation à la main : onglet Actions > « Build Siample » > « Run workflow ».

## Utilisation

**Vue Découpe**

| Geste | Effet |
|---|---|
| Clic sur une tranche ou un pad | L'écouter et la sélectionner |
| Double-clic sur la forme d'onde | Ajouter un point de coupe |
| Glisser un point | Le déplacer |
| Clic droit sur un point | Le supprimer |
| Molette | Zoomer (Maj + molette : défiler) |
| Glisser dans le bandeau turquoise | Tracer la zone à découper |
| Double-clic dans le bandeau | Revenir au fichier entier |
| Double-clic sur un bouton rotatif ou sur l'enveloppe | Revenir à la valeur d'origine |
| Clic sur la valeur du tempo | La corriger au clavier |

Dès que tu modifies un point à la main, Siample passe en mode « Manuel » pour ne plus écraser tes points.

**Vue Montage**

| Geste | Effet |
|---|---|
| Glisser un pad sur le montage | Poser un bloc (sur la ligne de son sample) |
| Double-clic dans une ligne | Poser la tranche sélectionnée de ce sample |
| Glisser un bloc | Le déplacer (Maj : sans aimantation) |
| Glisser le bord droit d'un bloc | Changer sa durée |
| Alt + glisser un bloc | Le dupliquer |
| Clic sur un bloc | L'écouter et afficher sa tranche |
| Clic droit sur un bloc | Le retirer |
| Clic sur la lettre d'une ligne | Afficher ce sample |

Sur une même ligne, un bloc coupe celui qui joue encore : un sample ne joue qu'une tranche à la fois dans le montage. Un bloc plus long que sa tranche laisse du silence, sauf si la tranche est en mode Boucle : elle se répète alors jusqu'à la fin du bloc.

### Monter une boucle à partir de deux samples

1. Charge un sample dans A, vérifie son tempo (÷2 ou ×2 si besoin) et active **Sync**.
2. Clique sur l'onglet **B**, charge un deuxième sample, vérifie son tempo et active **Sync** là aussi.
3. Si les deux ne sont pas dans la même tonalité, choisis celle de ton morceau dans le menu « Vers… » de chaque sample.
4. Passe en vue **Montage**. Clique sur « Dans l'ordre » pour poser la boucle de A, puis glisse des pads de B par-dessus.
5. Clique sur **Écouter**, ajuste, essaie une autre idée dans la variante 2.
6. Glisse « Glisser l'audio » dans la playlist du DAW, ou « Glisser le MIDI » sur la piste Siample.

Si le DAW refuse le dépôt direct, un simple clic sur le bouton montre le fichier dans son dossier : glisse-le de là.

### Notes MIDI

En mode Pads, la tranche 1 répond à la note MIDI 36, la tranche 2 à la note 37, et ainsi de suite jusqu'à la tranche 64 (note 99). La note 36 s'appelle **C3 dans FL Studio** et **C1 dans Logic Pro** ; Siample affiche le nom utilisé par le DAW dans lequel il tourne. C'est aussi la note du premier pad sur la plupart des contrôleurs à pads.

En mode Clavier, la note MIDI 60 (C5 dans FL Studio, C3 dans Logic Pro) joue la tranche sélectionnée à sa hauteur d'origine.

**Canaux MIDI.** Le canal 1 (celui d'un clavier ou d'un contrôleur à pads, en général) joue le sample affiché. Les canaux **2, 3, 4 et 5** jouent toujours les samples **A, B, C et D**, quel que soit l'onglet ouvert : c'est ce qu'utilise le MIDI exporté du montage. Sur ces canaux, une note dure ce que dure la note (même si la tranche est en one-shot) et chaque sample ne joue qu'une tranche à la fois, pour que le MIDI rejoue le montage tel que tu l'as entendu. Dans FL Studio, le canal MIDI d'une note est sa couleur dans le piano roll.

### À savoir

- Siample enregistre **le chemin** du sample, pas le son lui-même. Si tu déplaces ou supprimes le fichier, le plugin affichera « Fichier introuvable » à la réouverture du projet ; ta découpe est conservée et revient dès que tu remets le fichier à sa place. Range donc tes samples dans un dossier stable avant de les charger.
- Sans Sync, le pitch change aussi la durée de la tranche, comme sur une MPC.
- Les boutons à bordure en pointillés se glissent vers le DAW. Si le DAW refuse le dépôt direct depuis la fenêtre du plugin, un simple clic sur le bouton montre le fichier dans son dossier : glisse-le de là.
- Les fichiers exportés sont écrits dans le dossier temporaire du système : range dans ton projet ceux que tu veux garder (FL Studio et Logic copient en général l'audio déposé, vérifie selon tes réglages).
- Le montage se joue au tempo du projet. Dans la version autonome, où aucun DAW ne donne le tempo, clique sur la valeur « Projet » pour le régler.
- Sans Sync, un bloc garde la durée réelle de sa tranche : si tu changes ensuite le tempo du projet, les blocs ne tombent plus sur la grille. Avec Sync, ils suivent.
- La tonalité affichée est une estimation, faite pour les samples mélodiques ; sur une boucle de batterie elle n'a pas de sens. Fie-toi à ton oreille.
- Un preset, comme un projet, enregistre le chemin des samples et non leur son.
- Le fondu de boucle se nourrit du son qui précède la tranche : il n'a pas d'effet sur une tranche qui commence au tout début du fichier, ni sur une tranche inversée.
- En mode Boucle, la durée de la répétition est celle de la tranche : règle ses points de début et de fin pour que la boucle tombe juste. Avec Sync, une tranche qui dure un nombre rond de temps reste calée sur le projet.
- Quand Sync est activé, Siample recalcule les tranches en arrière-plan à chaque changement de tempo, de pitch ou de découpe. Pendant ce calcul (le bouton affiche « Sync… »), les tranches pas encore prêtes se jouent à leur vitesse d'origine. À l'export, Siample attend la fin du calcul.
- La détection du tempo peut se tromper d'un facteur deux sur les tempos très lents ou très rapides : c'est le rôle des boutons ÷2 et ×2.

## Compiler soi-même

Il faut CMake 3.24 ou plus récent, et Xcode (Mac) ou Visual Studio 2022 (Windows). JUCE et Signalsmith Stretch sont téléchargés automatiquement.

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Les plugins se trouvent ensuite dans `build/Siample_artefacts/Release/`.

Pour lancer les tests (moteur audio, découpe, tempo, étirement, montage, sauvegarde, interface) :

```
cmake -B build -DSIAMPLE_BUILD_TESTS=ON
cmake --build build --target SiampleTests
```

## Organisation du code

| Fichier | Rôle |
|---|---|
| `Source/PluginProcessor.*` | Moteur : voix, MIDI, montage, export, annulation, sauvegarde |
| `Source/Deck.*` | Un des quatre samples : fichier, découpe, tempo, tonalité, étirement en arrière-plan |
| `Source/KeyDetector.h` | Estimation de la tonalité |
| `Source/Voice.h` | Lecture d'une tranche (pitch, filtre, drive, enveloppe, boucle, inversion) |
| `Source/TransientDetector.h` | Détection des attaques et du tempo |
| `Source/Stretcher.*` | Étirement temporel et transposition (via Signalsmith Stretch) |
| `Source/PluginEditor.*` | Fenêtre du plugin |
| `Source/ui/` | Forme d'onde, montage, pads, panneau de réglages, enveloppe, navigateur, boutons à glisser, thème |
| `Fonts/` | Polices de l'interface |
| `packaging/` | Notices d'installation, script d'installation Mac, installeur Windows |
| `tests/SiampleTests.cpp` | Tests automatiques |
| `.github/workflows/build.yml` | Compilation automatique Mac et Windows |

## Licences des bibliothèques

- [JUCE](https://juce.com) est distribué sous double licence : AGPLv3 ou licence commerciale JUCE. Avant de vendre ou de distribuer le plugin, vérifie les conditions de licence sur le site de JUCE.
- [Signalsmith Stretch](https://github.com/Signalsmith-Audio/signalsmith-stretch) (étirement temporel) est sous licence MIT, © Geraint Luff / Signalsmith Audio Ltd. Cette licence demande de conserver sa mention de copyright si tu distribues le plugin.
- Les polices de l'interface, [Inter](https://rsms.me/inter/) et [Space Grotesk](https://github.com/floriankarsten/space-grotesk), sont sous licence SIL Open Font License (textes dans `Fonts/`).
