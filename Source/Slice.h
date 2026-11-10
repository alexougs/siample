#pragma once

namespace siample
{

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
    int   choke   = 0;       // 0 = aucun, 1..8 = groupe
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
constexpr int uiKeyBase  = 1000;   // écoute depuis l'interface : 1000 + numéro de tranche
constexpr int patternKey = 2000;   // lecture interne de la boucle
constexpr int keyboardRootNote = 60;

} // namespace siample
