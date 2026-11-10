# Siample

Sampler-découpeur pour FL Studio (Mac et Windows) et Logic Pro : tu glisses un sample, Siample le découpe en tranches, le cale sur le tempo de ton projet, et tu joues ces tranches sur 16 pads façon MPC ou au clavier MIDI.

Formats produits : VST3 (FL Studio Mac et Windows), Audio Unit (Logic Pro), et une application autonome pour tester sans DAW.

## Ce que fait la version 0.3

**Découpe**
- Chargement par glisser-déposer ou par le bouton « Charger un sample » (WAV, AIFF, FLAC, OGG, MP3, 15 minutes maximum).
- Découpe automatique par **transitoires** (réglage de sensibilité), par **grille** (2 à 64 parts égales, ou mesures, noires, croches, doubles d'après le tempo), ou **manuelle**.
- **Zone de travail** : sur un morceau entier, tu traces dans le bandeau turquoise la partie à découper.
- Jusqu'à **64 tranches**, sur 4 banques de 16 pads (A à D).

**Tempo**
- **Détection du tempo** du sample, corrigeable à la main (clic sur la valeur, boutons ÷2 et ×2, bouton Auto pour relancer la détection).
- **Sync** : les tranches sont étirées pour suivre le tempo du projet, sans changer de hauteur. Avec Sync, le pitch d'une tranche ne change plus sa durée.

**Jeu**
- **Pads** : une note par tranche. **Clavier** : tout le clavier joue la tranche sélectionnée, comme un instrument.
- **Mono** : chaque nouvelle note coupe la précédente.
- Par tranche : volume, pan, pitch (demi-tons et accordage fin), **filtre** passe-bas / passe-haut, inversion, enveloppe ADSR, groupe de choke, et trois façons de jouer : **one-shot** (jusqu'au bout), **gate** (tant que la touche est tenue) ou **boucle** (la tranche se répète tant que la touche est tenue).
- **Grain** : le son granuleux des vieux samplers, réglable de 0 à 100 %.

**Boucle**
- **Écouter** : joue la boucle en continu.
- **Flip** : réarrange les tranches au hasard en gardant le rythme d'origine. Chaque clic donne une nouvelle idée.
- **Glisser le MIDI vers le DAW** : dépose la boucle (d'origine ou flippée) en notes dans le piano roll.

La découpe et tous les réglages sont enregistrés dans le projet du DAW.

## Récupérer le plugin compilé

Le plugin est compilé automatiquement par GitHub à chaque envoi de code.

1. Sur la page du dépôt, ouvre l'onglet **Actions**.
2. Clique sur la compilation la plus récente (coche verte). Compte 10 à 20 minutes après un envoi.
3. En bas de la page, dans **Artifacts**, télécharge `Siample-macOS.zip` ou `Siample-Windows.zip`.
4. Décompresse l'archive et suis le fichier `LISEZMOI.txt` qu'elle contient.

Pour relancer une compilation à la main : onglet Actions > « Build Siample » > « Run workflow ».

## Utilisation

| Geste | Effet |
|---|---|
| Clic sur une tranche ou un pad | L'écouter et la sélectionner |
| Double-clic sur la forme d'onde | Ajouter un point de coupe |
| Glisser un point | Le déplacer |
| Clic droit sur un point | Le supprimer |
| Molette | Zoomer (Maj + molette : défiler) |
| Glisser dans le bandeau turquoise | Tracer la zone à découper |
| Double-clic dans le bandeau | Revenir au fichier entier |
| Double-clic sur un bouton rotatif | Revenir à la valeur d'origine |
| Clic sur la valeur du tempo | La corriger au clavier |

Dès que tu modifies un point à la main, Siample passe en mode « Manuel » pour ne plus écraser tes points.

### Refaire une boucle au tempo du projet

1. Charge le sample et trace la zone qui contient la boucle.
2. Vérifie le tempo affiché. S'il est deux fois trop lent ou trop rapide, utilise ×2 ou ÷2.
3. Active **Sync** : la boucle suit maintenant le tempo du projet.
4. Glisse le bouton « Glisser le MIDI vers le DAW » dans le piano roll de la piste Siample. Les notes rejouent la boucle ; déplace-les pour la réarranger.
5. Pour des idées, clique sur **Flip** puis **Écouter**, et glisse le MIDI quand un réarrangement te plaît. Active **Mono** pour que les tranches ne se chevauchent pas.

Si le DAW refuse le dépôt direct, un simple clic sur le bouton montre le fichier MIDI dans son dossier : glisse-le de là.

### Notes MIDI

En mode Pads, la tranche 1 répond à la note MIDI 36, la tranche 2 à la note 37, et ainsi de suite jusqu'à la tranche 64 (note 99). La note 36 s'appelle **C3 dans FL Studio** et **C1 dans Logic Pro** ; Siample affiche le nom utilisé par le DAW dans lequel il tourne. C'est aussi la note du premier pad sur la plupart des contrôleurs à pads.

En mode Clavier, la note MIDI 60 (C5 dans FL Studio, C3 dans Logic Pro) joue la tranche sélectionnée à sa hauteur d'origine.

### À savoir

- Siample enregistre **le chemin** du sample, pas le son lui-même. Si tu déplaces ou supprimes le fichier, le plugin affichera « Fichier introuvable » à la réouverture du projet ; ta découpe est conservée et revient dès que tu remets le fichier à sa place. Range donc tes samples dans un dossier stable avant de les charger.
- Sans Sync, le pitch change aussi la durée de la tranche, comme sur une MPC.
- En mode Boucle, la durée de la répétition est celle de la tranche : règle ses points de début et de fin pour que la boucle tombe juste. Avec Sync, une tranche qui dure un nombre rond de temps reste calée sur le projet.
- Quand Sync est activé, Siample recalcule les tranches en arrière-plan à chaque changement de tempo, de pitch ou de découpe. Pendant ce calcul (« calcul en cours… »), les tranches pas encore prêtes se jouent à leur vitesse d'origine. À l'export, Siample attend la fin du calcul.
- La détection du tempo peut se tromper d'un facteur deux sur les tempos très lents ou très rapides : c'est le rôle des boutons ÷2 et ×2.

## Compiler soi-même

Il faut CMake 3.24 ou plus récent, et Xcode (Mac) ou Visual Studio 2022 (Windows). JUCE et Signalsmith Stretch sont téléchargés automatiquement.

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Les plugins se trouvent ensuite dans `build/Siample_artefacts/Release/`.

Pour lancer les tests (moteur audio, découpe, tempo, étirement, sauvegarde, interface) :

```
cmake -B build -DSIAMPLE_BUILD_TESTS=ON
cmake --build build --target SiampleTests
```

## Organisation du code

| Fichier | Rôle |
|---|---|
| `Source/PluginProcessor.*` | Moteur : chargement, tranches, MIDI, tempo, boucle, sauvegarde |
| `Source/Voice.h` | Lecture d'une tranche (pitch, filtre, enveloppe, inversion) |
| `Source/TransientDetector.h` | Détection des attaques et du tempo |
| `Source/Stretcher.*` | Étirement temporel et transposition (via Signalsmith Stretch) |
| `Source/PluginEditor.*` | Fenêtre du plugin |
| `Source/ui/` | Forme d'onde, pads, panneau de réglages, glisser MIDI, thème |
| `tests/SiampleTests.cpp` | Tests automatiques |
| `.github/workflows/build.yml` | Compilation automatique Mac et Windows |

## Licences des bibliothèques

- [JUCE](https://juce.com) est distribué sous double licence : AGPLv3 ou licence commerciale JUCE. Avant de vendre ou de distribuer le plugin, vérifie les conditions de licence sur le site de JUCE.
- [Signalsmith Stretch](https://github.com/Signalsmith-Audio/signalsmith-stretch) (étirement temporel) est sous licence MIT, © Geraint Luff / Signalsmith Audio Ltd. Cette licence demande de conserver sa mention de copyright si tu distribues le plugin.
