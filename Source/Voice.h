#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include "Slice.h"

namespace siample
{

/** Tout ce qu'il faut pour lancer une voix. */
struct VoiceStart
{
    int slice = 0;
    int key = 0;                                     // note MIDI, ou identifiant interne
    const SliceParams* params = nullptr;
    const juce::AudioBuffer<float>* source = nullptr; // sample d'origine, ou tranche étirée
    const void* owner = nullptr;                      // la tranche étirée utilisée, sinon nullptr
    int start = 0, end = 0;                           // passage à lire dans `source`
    int originalStart = 0, originalEnd = 0;           // le même passage dans le sample d'origine
    double rate = 1.0;                                // échantillons source par échantillon de sortie
    float velocity = 1.0f;
    double hostRate = 44100.0;
    juce::uint64 age = 0;
};

/** Une voix lit une tranche. Tout se passe sur le thread audio. */
struct Voice
{
    bool active  = false;
    bool killing = false;
    bool oneShot = true;
    bool reverse = false;

    int slice = -1;
    int key   = -1;
    int choke = 0;
    int start = 0, end = 0;
    int originalStart = 0, originalEnd = 0;
    juce::uint64 age = 0;

    const juce::AudioBuffer<float>* source = nullptr;
    const void* owner = nullptr;

    double pos  = 0.0;
    double rate = 1.0;

    float gainL = 1.0f, gainR = 1.0f;
    float rampIn = 0.0f, rampInc = 1.0f;
    float killGain = 1.0f, killDec = 0.0f;

    // Filtre (0 = aucun, 1 = passe-bas, 2 = passe-haut)
    int filterType = 0;
    float fa1 = 0.0f, fa2 = 0.0f, fa3 = 0.0f, fk = 1.414f;
    float ic1[2] = { 0.0f, 0.0f }, ic2[2] = { 0.0f, 0.0f };

    juce::ADSR adsr;

    void begin (const VoiceStart& s)
    {
        const auto& p = *s.params;

        active  = true;
        killing = false;
        slice   = s.slice;
        key     = s.key;
        choke   = p.choke;
        oneShot = p.oneShot;
        reverse = p.reverse;
        source  = s.source;
        owner   = s.owner;
        start   = s.start;
        end     = s.end;
        originalStart = s.originalStart;
        originalEnd   = s.originalEnd;
        age     = s.age;
        rate    = s.rate;
        pos     = reverse ? (double) (end - 1) : (double) start;

        const float gain  = juce::Decibels::decibelsToGain (p.gainDb) * (0.2f + 0.8f * s.velocity);
        const float angle = (juce::jlimit (-1.0f, 1.0f, p.pan) + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
        gainL = gain * std::cos (angle) * juce::MathConstants<float>::sqrt2;
        gainR = gain * std::sin (angle) * juce::MathConstants<float>::sqrt2;

        // Mini-fondu d'entrée (0,5 ms) contre les clics
        rampIn  = 0.0f;
        rampInc = (float) (1.0 / juce::jmax (1.0, s.hostRate * 0.0005));

        setFilter (p.filter, s.hostRate);

        adsr.setSampleRate (s.hostRate);
        adsr.setParameters ({ p.attack, p.decay, p.sustain, p.release });
        adsr.reset();
        adsr.noteOn();
    }

    /** Coupe la voix avec un fondu très court. */
    void kill (double hostRate)
    {
        if (active && ! killing)
        {
            killing  = true;
            killGain = 1.0f;
            killDec  = (float) (1.0 / juce::jmax (1.0, hostRate * 0.004));
        }
    }

    void noteOff()
    {
        if (active && ! oneShot)
            adsr.noteOff();
    }

    /** Position de lecture ramenée au sample d'origine (pour la tête de lecture). */
    int originalPosition() const
    {
        const double span = (double) juce::jmax (1, end - start);
        const double fraction = juce::jlimit (0.0, 1.0, (pos - (double) start) / span);
        return originalStart + (int) (fraction * (double) (originalEnd - originalStart));
    }

    void render (float* outL, float* outR, int numSamples) noexcept
    {
        const int   last = source->getNumSamples() - 1;
        const float* in0 = source->getReadPointer (0);
        const float* in1 = source->getNumChannels() > 1 ? source->getReadPointer (1) : in0;
        const float  edgeFade = 48.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            const double remaining = reverse ? (pos - (double) start + 1.0) : ((double) end - pos);

            if (remaining <= 0.0)
            {
                active = false;
                return;
            }

            float g = adsr.getNextSample();

            if (! adsr.isActive())
            {
                active = false;
                return;
            }

            // Fondu de sortie quand on arrive au bord de la tranche
            const float outLeft = (float) (remaining / rate);
            if (outLeft < edgeFade)
                g *= outLeft / edgeFade;

            if (rampIn < 1.0f)
            {
                g *= rampIn;
                rampIn += rampInc;
            }

            if (killing)
            {
                g *= killGain;
                killGain -= killDec;

                if (killGain <= 0.0f)
                {
                    active = false;
                    return;
                }
            }

            // Interpolation de Hermite sur 4 points
            const int   i1 = juce::jlimit (0, last, (int) pos);
            const float f  = (float) (pos - (double) (int) pos);
            const int   i0 = juce::jmax (0, i1 - 1);
            const int   i2 = juce::jmin (last, i1 + 1);
            const int   i3 = juce::jmin (last, i1 + 2);

            float left  = hermite (in0[i0], in0[i1], in0[i2], in0[i3], f);
            float right = hermite (in1[i0], in1[i1], in1[i2], in1[i3], f);

            if (filterType != 0)
            {
                left  = filterSample (left, 0);
                right = filterSample (right, 1);
            }

            outL[i] += g * gainL * left;
            outR[i] += g * gainR * right;

            pos += reverse ? -rate : rate;
        }
    }

private:
    void setFilter (float amount, double hostRate)
    {
        ic1[0] = ic1[1] = ic2[0] = ic2[1] = 0.0f;
        amount = juce::jlimit (-1.0f, 1.0f, amount);

        if (std::abs (amount) < 0.005f)
        {
            filterType = 0;
            return;
        }

        // Passe-bas : de 20 kHz à 80 Hz. Passe-haut : de 20 Hz à 8 kHz.
        const double a = (double) std::abs (amount);
        double cutoff = amount < 0.0f ? 20000.0 * std::pow (80.0 / 20000.0, a)
                                      : 20.0 * std::pow (8000.0 / 20.0, a);
        cutoff = juce::jmin (cutoff, hostRate * 0.45);

        filterType = amount < 0.0f ? 1 : 2;

        const float g = (float) std::tan (juce::MathConstants<double>::pi * cutoff / hostRate);
        fk  = 1.414f;
        fa1 = 1.0f / (1.0f + g * (g + fk));
        fa2 = g * fa1;
        fa3 = g * fa2;
    }

    float filterSample (float x, int channel) noexcept
    {
        // Filtre à variable d'état (forme TPT), stable même à fréquence de coupure élevée
        const float v3 = x - ic2[channel];
        const float v1 = fa1 * ic1[channel] + fa2 * v3;
        const float v2 = ic2[channel] + fa2 * ic1[channel] + fa3 * v3;
        ic1[channel] = 2.0f * v1 - ic1[channel];
        ic2[channel] = 2.0f * v2 - ic2[channel];
        return filterType == 1 ? v2 : x - fk * v1 - v2;
    }

    static float hermite (float y0, float y1, float y2, float y3, float f) noexcept
    {
        const float c1 = 0.5f * (y2 - y0);
        const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * f + c2) * f + c1) * f + y1;
    }
};

} // namespace siample
