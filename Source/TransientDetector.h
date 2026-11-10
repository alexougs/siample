#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

/*  Analyse d'un signal mono : détection des attaques (transitoires) et estimation du tempo.
    Ce fichier ne dépend que de la bibliothèque standard, pour pouvoir être testé seul.

    Étapes :
      1. computeOnsetEnvelope : mesure, toutes les ~6 ms, de combien le niveau vient de monter.
      2. findOnsetCandidates  : en tire les attaques plausibles, avec une force entre 0 et 1.
      3. selectOnsets         : garde les attaques assez fortes selon la sensibilité choisie.
      4. estimateTempo        : cherche la pulsation régulière dans la même mesure.
*/
namespace siample
{

struct Onset
{
    int   position = 0;     // en échantillons, relatif au début du signal analysé
    float strength = 0.0f;  // 0 .. 1
};

struct OnsetEnvelope
{
    int hop = 0;                 // écart entre deux mesures, en échantillons
    int win = 0;                 // longueur de la fenêtre de mesure
    double hfWeight = 4.0;
    float maxLevel = -1000.0f;
    float floorDb  = -1000.0f;
    std::vector<float> level;    // niveau en dB
    std::vector<float> flux;     // montée du niveau, plafonnée (0 .. fluxScale) : sert à trouver les attaques
    std::vector<float> accent;   // montée du volume réel : sert à trouver le tempo (les coups forts y dominent)

    static constexpr float cap = 10.0f;
    static constexpr float fluxScale = cap * 1.5f;

    bool isValid() const   { return ! flux.empty(); }
};

inline OnsetEnvelope computeOnsetEnvelope (const float* x, int length, double sampleRate)
{
    OnsetEnvelope env;
    env.hop = std::max (32, (int) std::lround (sampleRate * 0.0058));
    env.win = env.hop * 4;

    // Poids des aigus : la différence entre deux échantillons voisins rapetisse quand la fréquence
    // d'échantillonnage monte, on compense pour que le résultat ne dépende pas du format du fichier.
    env.hfWeight = 4.0 * (sampleRate / 44100.0) * (sampleRate / 44100.0);

    if (x == nullptr || length < env.win * 2)
        return env;

    const int numFrames = (length - env.win) / env.hop + 1;
    env.level.resize ((size_t) numFrames);
    std::vector<float> amplitude ((size_t) numFrames);

    for (int k = 0; k < numFrames; ++k)
    {
        const float* p = x + k * env.hop;
        double e = 0.0, hf = 0.0;
        float prev = p[0];

        for (int n = 0; n < env.win; ++n)
        {
            const float v = p[n];
            const float d = v - prev;
            e  += (double) v * (double) v;
            hf += (double) d * (double) d;   // accentue les aigus, là où les attaques sont nettes
            prev = v;
        }

        const float db = 10.0f * std::log10 ((float) ((e + env.hfWeight * hf) / (double) env.win) + 1.0e-12f);
        env.level[(size_t) k] = db;
        env.maxLevel = std::max (env.maxLevel, db);
        amplitude[(size_t) k] = (float) std::sqrt (e / (double) env.win);
    }

    if (env.maxLevel < -90.0f)
    {
        env.level.clear();
        return env;   // silence
    }

    env.accent.resize ((size_t) numFrames);
    for (int k = 0; k < numFrames; ++k)
        env.accent[(size_t) k] = std::max (0.0f, amplitude[(size_t) k] - (k > 0 ? amplitude[(size_t) k - 1] : 0.0f));

    env.floorDb = env.maxLevel - 60.0f;
    auto lev = [&] (int k) { return k < 0 ? env.floorDb : std::max (env.floorDb, env.level[(size_t) k]); };

    // Une montée de plus de 10 dB est une attaque franche, quelle que soit sa taille : on plafonne,
    // sinon un coup qui sort du silence complet écraserait tous les autres.
    env.flux.resize ((size_t) numFrames);

    for (int k = 0; k < numFrames; ++k)
        env.flux[(size_t) k] = std::min (OnsetEnvelope::cap, std::max (0.0f, lev (k) - lev (k - 1)))
                             + 0.5f * std::min (OnsetEnvelope::cap, std::max (0.0f, lev (k) - lev (k - 2)));

    return env;
}

namespace detail
{
    /** Affine la position d'une attaque à l'intérieur de [searchStart, searchEnd). */
    inline int refineOnset (const float* x, int length, int searchStart, int searchEnd, double hfWeight)
    {
        const int block   = 32;
        const int context = 12;   // blocs de contexte regardés avant chaque bloc

        searchStart = std::max (0, searchStart);
        searchEnd   = std::min (length, searchEnd);

        if (searchEnd - searchStart < block * 2)
            return searchStart;

        // On calcule aussi l'énergie un peu avant la zone de recherche, pour avoir un point de comparaison
        const int analysisStart = std::max (0, searchStart - context * block);
        const int firstBlock    = (searchStart - analysisStart) / block;
        const int numBlocks     = (searchEnd - analysisStart) / block;

        std::vector<double> energy ((size_t) numBlocks);
        double total = 0.0;

        for (int b = 0; b < numBlocks; ++b)
        {
            double e = 0.0;
            const float* p = x + analysisStart + b * block;
            float prev = p[0];

            for (int n = 0; n < block; ++n)
            {
                const float d = p[n] - prev;
                e += (double) p[n] * (double) p[n] + hfWeight * (double) d * (double) d;
                prev = p[n];
            }

            energy[(size_t) b] = e;
            total += e;
        }

        const double eps = 1.0e-9 + 0.002 * total / (double) numBlocks;

        // On cherche le bloc où l'énergie dépasse le plus tout ce qui le précède de près.
        // (Le maximum, et non la moyenne : une basse tenue fait osciller l'énergie sans être une attaque.)
        int bestBlock = firstBlock;
        double bestRatio = 0.0;

        for (int b = firstBlock; b < numBlocks; ++b)
        {
            double before = 0.0;

            for (int j = std::max (0, b - context); j < b; ++j)
                before = std::max (before, energy[(size_t) j]);

            const double ratio = (energy[(size_t) b] + eps) / (before + eps);

            if (ratio > bestRatio)
            {
                bestRatio = ratio;
                bestBlock = b;
            }
        }

        int pos = analysisStart + bestBlock * block;

        // Recule jusqu'au passage par zéro le plus proche (au plus 1 bloc) pour limiter les clics
        for (int n = pos; n > std::max (searchStart, pos - block) && n > 0; --n)
        {
            if ((x[n] >= 0.0f) != (x[n - 1] >= 0.0f))
            {
                pos = n;
                break;
            }
        }

        return pos;
    }
}

/** Renvoie les attaques candidates, triées dans l'ordre du temps. */
inline std::vector<Onset> findOnsetCandidates (const float* x, int length, double sampleRate, const OnsetEnvelope& env)
{
    std::vector<Onset> result;

    if (! env.isValid())
        return result;

    const int numFrames = (int) env.flux.size();
    const int hop = env.hop, win = env.win;
    auto lev = [&] (int k) { return k < 0 ? env.floorDb : std::max (env.floorDb, env.level[(size_t) k]); };

    float maxFlux = 0.0f;
    for (float f : env.flux)
        maxFlux = std::max (maxFlux, f);

    if (maxFlux < 1.5f)
        return result;   // rien qui ressemble à une attaque

    const int radius = std::max (1, (int) std::lround (0.03 * sampleRate / (double) hop));
    float maxStrength = 0.0f;

    for (int k = 0; k < numFrames; ++k)
    {
        const float f = env.flux[(size_t) k];

        if (f < 1.0f)
            continue;

        bool isPeak = true;

        for (int j = std::max (0, k - radius); j <= std::min (numFrames - 1, k + radius) && isPeak; ++j)
            if ((j < k && env.flux[(size_t) j] >= f) || (j > k && env.flux[(size_t) j] > f))
                isPeak = false;

        if (! isPeak)
            continue;

        // Niveau atteint juste après l'attaque
        float after = env.floorDb;
        for (int j = k; j <= std::min (numFrames - 1, k + 4); ++j)
            after = std::max (after, lev (j));

        if (after < env.maxLevel - 45.0f)
            continue;   // trop faible pour être utile

        const float loud = std::min (1.0f, std::max (0.0f, (after - (env.maxLevel - 40.0f)) / 40.0f));
        const float strength = (f / OnsetEnvelope::fluxScale) * (0.4f + 0.6f * loud);

        // L'attaque est entrée dans la fenêtre par sa fin
        const int roughEnd = k * hop + win;
        const int pos = k == 0 ? detail::refineOnset (x, length, 0, win, env.hfWeight)
                               : detail::refineOnset (x, length, roughEnd - 2 * hop, roughEnd + hop / 2, env.hfWeight);

        result.push_back ({ pos, strength });
        maxStrength = std::max (maxStrength, strength);
    }

    if (maxStrength > 0.0f)
        for (auto& o : result)
            o.strength /= maxStrength;

    std::sort (result.begin(), result.end(), [] (const Onset& a, const Onset& b) { return a.position < b.position; });
    return result;
}

/** Choisit les attaques à garder.
    @param sensitivity   0 = seulement les plus fortes, 1 = presque toutes
    @param maxCount      nombre maximum de positions renvoyées
    @param minGap        écart minimum entre deux positions, en échantillons
    @returns les positions triées dans l'ordre du temps
*/
inline std::vector<int> selectOnsets (const std::vector<Onset>& candidates, float sensitivity, int maxCount, int minGap)
{
    sensitivity = std::min (1.0f, std::max (0.0f, sensitivity));
    const float inv = 1.0f - sensitivity;
    const float threshold = 0.02f + 0.75f * std::pow (inv, 2.6f);

    std::vector<Onset> kept;
    for (const auto& o : candidates)
        if (o.strength >= threshold)
            kept.push_back (o);

    std::sort (kept.begin(), kept.end(), [] (const Onset& a, const Onset& b) { return a.strength > b.strength; });

    std::vector<int> positions;

    for (const auto& o : kept)
    {
        if ((int) positions.size() >= maxCount)
            break;

        bool tooClose = false;
        for (int p : positions)
            if (std::abs (p - o.position) < minGap)
            {
                tooClose = true;
                break;
            }

        if (! tooClose)
            positions.push_back (o.position);
    }

    std::sort (positions.begin(), positions.end());
    return positions;
}

/** Estime le tempo, en battements par minute.

    Deux indices sont combinés :
      - la pulsation régulière des attaques (autocorrélation), fiable sur plusieurs secondes ;
      - l'hypothèse « boucle » : si le passage dure un nombre rond de temps (4, 8, 16…),
        le tempo exact se déduit de sa durée.

    @returns le tempo entre 60 et 200 BPM, ou 0 si rien d'exploitable
*/
inline double estimateTempo (const OnsetEnvelope& env, int length, double sampleRate)
{
    if (! env.isValid() || length <= 0 || sampleRate <= 0.0)
        return 0.0;

    const double duration = (double) length / sampleRate;
    const double framesPerSecond = sampleRate / (double) env.hop;
    const int numFrames = (int) env.accent.size();

    const double minBpm = 60.0, maxBpm = 200.0;
    double pulseBpm = 0.0;

    if (duration >= 3.0)
    {
        double mean = 0.0;
        for (float f : env.accent)
            mean += f;
        mean /= (double) numFrames;

        auto correlation = [&] (int lag)
        {
            if (lag <= 0 || lag >= numFrames - 8)
                return 0.0;

            double sum = 0.0;
            for (int i = 0; i + lag < numFrames; ++i)
                sum += ((double) env.accent[(size_t) i] - mean) * ((double) env.accent[(size_t) (i + lag)] - mean);

            return sum / (double) (numFrames - lag);
        };

        const int lagMin = std::max (2, (int) std::floor (framesPerSecond * 60.0 / maxBpm));
        const int lagMax = std::min (numFrames / 2, (int) std::ceil (framesPerSecond * 60.0 / minBpm));

        double bestScore = 0.0;
        int bestLag = 0;

        for (int lag = lagMin; lag <= lagMax; ++lag)
        {
            // Une vraie pulsation se répète aussi tous les 2 et 4 temps
            double score = correlation (lag) + 0.5 * correlation (lag * 2) + 0.25 * correlation (lag * 4);

            // Préférence douce pour les tempos courants (autour de 110 BPM)
            const double bpm = framesPerSecond * 60.0 / (double) lag;
            const double octaves = std::log2 (bpm / 110.0);
            score *= std::exp (-0.5 * octaves * octaves / (0.8 * 0.8));

            if (score > bestScore)
            {
                bestScore = score;
                bestLag = lag;
            }
        }

        if (bestLag > 0)
        {
            // Affinage : on regarde sur 4 temps, où un petit écart se voit quatre fois mieux
            double refinedLag = (double) bestLag;
            const int longLag = bestLag * 4;

            if (longLag + 3 < numFrames / 2)
            {
                int bestLong = longLag;
                double bestValue = correlation (longLag);

                for (int l = longLag - 3; l <= longLag + 3; ++l)
                {
                    const double value = correlation (l);
                    if (value > bestValue)
                    {
                        bestValue = value;
                        bestLong = l;
                    }
                }

                const double a = correlation (bestLong - 1), b = bestValue, c = correlation (bestLong + 1);
                const double denominator = a - 2.0 * b + c;
                const double offset = std::abs (denominator) > 1.0e-12 ? 0.5 * (a - c) / denominator : 0.0;
                refinedLag = ((double) bestLong + std::min (1.0, std::max (-1.0, offset))) / 4.0;
            }

            pulseBpm = framesPerSecond * 60.0 / refinedLag;
        }
    }

    // Hypothèse « boucle » : nombre rond de temps dans la durée
    const int beatCounts[] = { 4, 8, 16, 32, 64, 2, 1, 128, 12, 24, 6, 3, 48, 96 };

    if (pulseBpm > 0.0)
    {
        double best = 0.0, bestError = 0.03;   // 3 % d'écart au plus

        for (int beats : beatCounts)
        {
            const double bpm = 60.0 * (double) beats / duration;
            const double error = std::abs (bpm - pulseBpm) / pulseBpm;

            if (bpm >= minBpm && bpm <= maxBpm && error < bestError)
            {
                bestError = error;
                best = bpm;
            }
        }

        return best > 0.0 ? best : pulseBpm;
    }

    // Passage court : on suppose une boucle de 1, 2, 4, 8… temps, dans la plage la plus courante
    for (double low : { 75.0, 60.0 })
        for (int beats : { 4, 8, 16, 2, 32, 1, 64 })
        {
            const double bpm = 60.0 * (double) beats / duration;
            if (bpm >= low && bpm < (low > 70.0 ? 160.0 : maxBpm))
                return bpm;
        }

    return 0.0;
}

} // namespace siample
