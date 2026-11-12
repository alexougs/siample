#pragma once

#include <juce_dsp/juce_dsp.h>
#include <array>
#include <cmath>
#include <vector>

namespace siample
{

/** Tonalité estimée d'un passage. root : 0 = do … 11 = si, ou -1 si rien de fiable. */
struct DetectedKey
{
    int root = -1;
    bool minor = false;
    float confidence = 0.0f;

    bool isValid() const   { return root >= 0; }
};

/** Estime la tonalité : on mesure la présence de chacune des 12 notes dans le spectre,
    puis on compare ce profil à ceux des 24 tonalités (profils de Krumhansl). */
inline DetectedKey detectKey (const float* x, int length, double sampleRate)
{
    DetectedKey result;

    if (x == nullptr || sampleRate <= 0.0)
        return result;

    // Une minute suffit, et on réduit la fréquence d'échantillonnage : seules les notes nous intéressent
    length = juce::jmin (length, (int) (sampleRate * 60.0));
    const int factor = juce::jmax (1, (int) (sampleRate / 11025.0));
    const double rate = sampleRate / (double) factor;
    const int reduced = length / factor;

    constexpr int order = 12, size = 1 << order, hop = size / 2;

    if (reduced < size)
        return result;

    std::vector<float> low ((size_t) reduced);

    for (int i = 0; i < reduced; ++i)
    {
        float sum = 0.0f;

        for (int j = 0; j < factor; ++j)
            sum += x[i * factor + j];

        low[(size_t) i] = sum / (float) factor;
    }

    juce::dsp::FFT fft (order);
    std::vector<float> frame ((size_t) size * 2), window ((size_t) size);

    for (int i = 0; i < size; ++i)
        window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (size - 1));

    std::array<double, 12> chroma {};
    const int firstBin = juce::jmax (2, (int) (60.0 * size / rate));
    const int lastBin  = juce::jmin (size / 2 - 2, (int) (2100.0 * size / rate));

    for (int start = 0; start + size <= reduced; start += hop)
    {
        std::fill (frame.begin(), frame.end(), 0.0f);

        for (int i = 0; i < size; ++i)
            frame[(size_t) i] = low[(size_t) (start + i)] * window[(size_t) i];

        fft.performFrequencyOnlyForwardTransform (frame.data(), true);

        // On ne retient que les pics du spectre : ce sont eux qui portent les notes
        for (int bin = firstBin; bin <= lastBin; ++bin)
        {
            const float magnitude = frame[(size_t) bin];

            if (magnitude <= frame[(size_t) bin - 1] || magnitude < frame[(size_t) bin + 1])
                continue;

            const double frequency = (double) bin * rate / (double) size;
            const int note = (int) std::lround (12.0 * std::log2 (frequency / 261.6256));
            chroma[(size_t) (((note % 12) + 12) % 12)] += (double) magnitude;
        }
    }

    double total = 0.0;
    for (double c : chroma)
        total += c;

    if (total < 1.0e-6)
        return result;

    static const double major[12] = { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
    static const double minor[12] = { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };

    auto correlate = [&chroma] (const double* profile, int root)
    {
        double meanC = 0.0, meanP = 0.0;

        for (int i = 0; i < 12; ++i)
        {
            meanC += chroma[(size_t) i] / 12.0;
            meanP += profile[i] / 12.0;
        }

        double num = 0.0, denC = 0.0, denP = 0.0;

        for (int i = 0; i < 12; ++i)
        {
            const double c = chroma[(size_t) ((i + root) % 12)] - meanC;
            const double p = profile[i] - meanP;
            num  += c * p;
            denC += c * c;
            denP += p * p;
        }

        return denC > 0.0 && denP > 0.0 ? num / std::sqrt (denC * denP) : 0.0;
    };

    double best = -2.0;

    for (int root = 0; root < 12; ++root)
    {
        for (int mode = 0; mode < 2; ++mode)
        {
            const double score = correlate (mode == 0 ? major : minor, root);

            if (score > best)
            {
                best = score;
                result.root  = root;
                result.minor = mode == 1;
            }
        }
    }

    result.confidence = (float) best;

    if (best < 0.35)
        result.root = -1;

    return result;
}

} // namespace siample
