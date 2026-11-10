#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace siample
{

/** Étire (ou raccourcit) et transpose un passage du sample, hors temps réel.

    @param source      le sample complet
    @param sampleRate  sa fréquence d'échantillonnage
    @param start, end  le passage à traiter, en échantillons
    @param ratio       facteur de durée : 2 = deux fois plus long, 0.5 = deux fois plus court
    @param semitones   transposition, sans effet sur la durée
    @param result      reçoit le passage traité (même nombre de canaux que la source)
    @returns false si le passage est trop court pour être traité
*/
bool stretchSegment (const juce::AudioBuffer<float>& source, double sampleRate,
                     int start, int end, double ratio, double semitones,
                     juce::AudioBuffer<float>& result);

} // namespace siample
