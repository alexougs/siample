#include "Stretcher.h"

// La bibliothèque Signalsmith Stretch (licence MIT) fait le travail d'étirement.
JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wconversion", "-Wsign-conversion", "-Wshadow", "-Wfloat-equal",
                                     "-Wshorten-64-to-32", "-Wimplicit-float-conversion", "-Wfloat-conversion",
                                     "-Wunused-parameter", "-Wextra-semi", "-Wzero-as-null-pointer-constant",
                                     "-Wimplicit-int-float-conversion", "-Wold-style-cast", "-Wdouble-promotion",
                                     "-Wunused-variable", "-Wsign-compare", "-Wcast-align", "-Wpedantic",
                                     "-Wimplicit-int-conversion", "-Wredundant-decls", "-Wuseless-cast")
JUCE_BEGIN_IGNORE_WARNINGS_MSVC (4244 4267 4305 4100 4456 4457 4458 4127 4996 4018 4389 4701 4702)
#include "signalsmith-stretch/signalsmith-stretch.h"
JUCE_END_IGNORE_WARNINGS_MSVC
JUCE_END_IGNORE_WARNINGS_GCC_LIKE

namespace siample
{

bool stretchSegment (const juce::AudioBuffer<float>& source, double sampleRate,
                     int start, int end, double ratio, double semitones,
                     juce::AudioBuffer<float>& result)
{
    const int total    = source.getNumSamples();
    const int channels = source.getNumChannels();

    start = juce::jlimit (0, total, start);
    end   = juce::jlimit (start, total, end);

    if (channels <= 0 || end - start < 2 || ratio <= 0.0 || sampleRate <= 0.0)
        return false;

    // On traite un peu d'audio avant et après le passage : l'algorithme a besoin de contexte
    // pour que le début et la fin du passage sonnent juste.
    const int margin   = (int) (0.3 * sampleRate);
    const int from     = juce::jmax (0, start - margin);
    const int to       = juce::jmin (total, end + margin);
    const int inLength = to - from;
    const int outLength = juce::jmax (1, juce::roundToInt ((double) inLength * ratio));

    signalsmith::stretch::SignalsmithStretch<float> stretcher;
    stretcher.presetDefault (channels, (float) sampleRate);
    stretcher.setTransposeSemitones ((float) semitones);

    const float playbackRate = (float) inLength / (float) outLength;
    const int seekLength = stretcher.outputSeekLength (playbackRate);

    if (inLength < seekLength + 16)
        return false;

    juce::AudioBuffer<float> rendered (channels, outLength);
    rendered.clear();

    std::vector<const float*> in ((size_t) channels), inAfterSeek ((size_t) channels);
    std::vector<float*> out ((size_t) channels), outTail ((size_t) channels);

    // Amorçage : aligne la sortie sur le tout début de l'entrée
    const int firstOutput = juce::jlimit (0, outLength, outLength - (int) ((float) seekLength / playbackRate));

    for (int c = 0; c < channels; ++c)
    {
        in[(size_t) c]          = source.getReadPointer (c, from);
        inAfterSeek[(size_t) c] = source.getReadPointer (c, from) + seekLength;
        out[(size_t) c]         = rendered.getWritePointer (c);
        outTail[(size_t) c]     = rendered.getWritePointer (c) + firstOutput;
    }

    stretcher.outputSeek (in, seekLength);
    stretcher.process (inAfterSeek, inLength - seekLength, out, firstOutput);
    stretcher.flush (outTail, outLength - firstOutput, playbackRate);

    // On ne garde que le passage demandé
    const int cropLength = juce::jmax (1, juce::roundToInt ((double) (end - start) * ratio));
    const int cropStart  = juce::jlimit (0, juce::jmax (0, outLength - cropLength), juce::roundToInt ((double) (start - from) * ratio));
    const int length     = juce::jmin (cropLength, outLength - cropStart);

    if (length < 2)
        return false;

    result.setSize (channels, length);

    for (int c = 0; c < channels; ++c)
        result.copyFrom (c, 0, rendered, c, cropStart, length);

    return true;
}

} // namespace siample
