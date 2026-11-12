#pragma once

namespace siample
{

constexpr int numDecks        = 4;    // samples chargés en même temps (A, B, C, D)
constexpr int maxSlices       = 64;   // 4 banques de 16 pads
constexpr int baseNote        = 36;   // première tranche = note MIDI 36
constexpr int numChokeGroups  = 8;
constexpr int minSliceLength  = 64;   // en échantillons

/** Réglages propres à une tranche. */
struct SliceParams
{
    float gainDb  = 0.0f;    // -24 .. +12 dB
    float pan     = 0.0f;    // -1 (gauche) .. +1 (droite)
    int   pitch   = 0;       // demi-tons, -24 .. +24
    float fine    = 0.0f;    // centièmes, -100 .. +100
    float attack  = 0.0f;    // secondes
    float decay   = 0.0f;    // secondes
    float sustain = 1.0f;    // 0 .. 1
    float release = 0.02f;   // secondes
    float filter  = 0.0f;    // -1 .. 0 : passe-bas de plus en plus fermé ; 0 .. +1 : passe-haut
    bool  reverse = false;
    bool  oneShot = true;    // true : la tranche joue jusqu'au bout ; false : tant que la note est tenue
    bool  loop    = false;   // true : la tranche se répète tant que la note est tenue (prend le pas sur oneShot)
    int   choke   = 0;       // 0 = aucun, 1..8 = groupe
    float resonance = 0.0f;  // 0 .. 1 : accentue la fréquence de coupure du filtre
    float drive     = 0.0f;  // 0 .. 1 : saturation
    float loopFade  = 0.0f;  // millisecondes de fondu enchaîné au point de bouclage (mode boucle), 0 .. 250
};

struct Slice
{
    int start = 0;           // position de début dans le sample, en échantillons
    SliceParams params;
};

enum class SliceMode
{
    transients = 0,
    grid       = 1,
    manual     = 2
};

/** Ce que jouent les notes MIDI. */
enum class PlayMode
{
    pads = 0,   // une note = une tranche (note 36 = tranche 1)
    keys = 1    // tout le clavier joue la tranche sélectionnée, do central = hauteur d'origine
};

/** Grille de découpe : un nombre positif = parts égales ; sinon une valeur musicale. */
namespace grid
{
    constexpr int bars      = -1;
    constexpr int quarters  = -4;
    constexpr int eighths   = -8;
    constexpr int sixteenths = -16;
}

/** Identifiants de voix qui ne viennent pas d'une note MIDI. */
constexpr int uiKeyBase   = 10000;  // écoute depuis l'interface : 10000 + 100 × sample + tranche
constexpr int timelineKey = 20000;  // lecture du montage : 20000 + sample
// (les notes MIDI utilisent : 128 × canal + note)
constexpr int keyboardRootNote = 60;

/** Montage : des blocs posés sur une grille en temps (noires). */
constexpr int maxClips       = 256;
constexpr int numVariants    = 3;
constexpr int beatsPerBar    = 4;
constexpr int maxTimelineBars = 16;

struct Clip
{
    int deck  = 0;
    int slice = 0;
    double start  = 0.0;   // en temps, depuis le début du montage
    double length = 1.0;   // en temps
};

/** Premier canal MIDI (1..16) des canaux « montage » : canal 2 = sample A, 3 = B, 4 = C, 5 = D.
    Sur ces canaux une note dure ce que dure la note, et chaque sample ne joue qu'une tranche à la fois. */
constexpr int firstDeckChannel = 2;

} // namespace siample
