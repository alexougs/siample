#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_data_structures/juce_data_structures.h>
#include <array>
#include <atomic>

#include "Slice.h"
#include "TransientDetector.h"
#include "KeyDetector.h"

class SiampleProcessor;

namespace siample
{

struct SampleData final : public juce::ReferenceCountedObject
{
    using Ptr = juce::ReferenceCountedObjectPtr<SampleData>;

    juce::AudioBuffer<float> buffer;
    double sampleRate = 44100.0;
    juce::File file;
};

/** Une tranche étirée et transposée à l'avance, prête à être lue. */
struct RenderedSlice final : public juce::ReferenceCountedObject
{
    using Ptr = juce::ReferenceCountedObjectPtr<RenderedSlice>;

    juce::AudioBuffer<float> buffer;
    int lead = 0;                      // audio qui précède la tranche, gardé pour le fondu de boucle
    int start = 0, end = 0, pitch = 0;
    float fine = 0.0f;
    double ratio = 1.0;

    bool matches (int s, int e, int p, float f, double r) const
    {
        return start == s && end == e && pitch == p
            && std::abs (fine - f) < 0.001f && std::abs (ratio - r) < 1.0e-6;
    }
};

/*  Un des quatre samples chargés (A, B, C, D) : le fichier, sa découpe, son tempo, ses réglages.

    Règle d'accès aux données, la même pour tout le plugin :
      - le thread audio LIT le sample, les tranches et les réglages de jeu, toujours sous le verrou audio ;
      - l'interface (thread principal) est la seule à les MODIFIER, sous ce même verrou ;
      - l'interface peut donc les lire librement sans verrou ;
      - les tranches étirées sont calculées par un thread de fond, puis installées sous le verrou.
*/
class Deck final
{
public:
    Deck (SiampleProcessor& processor, int deckIndex);
    ~Deck();

    int getIndex() const                         { return index; }

    //==============================================================================
    // Sample
    /** Charge un fichier audio. Renvoie un message d'erreur, ou une chaîne vide si tout va bien. */
    juce::String loadFile (const juce::File& file);
    void unload();

    SampleData::Ptr getSample() const            { return sample; }
    bool hasSample() const                       { return sample != nullptr; }
    int getSampleLength() const                  { return sample != nullptr ? sample->buffer.getNumSamples() : 0; }
    double getSourceSampleRate() const           { return sample != nullptr ? sample->sampleRate : 44100.0; }
    juce::String getFileName() const             { return sample != nullptr ? sample->file.getFileName() : juce::String(); }
    juce::String getMissingFilePath() const      { return missingFilePath; }

    //==============================================================================
    // Zone de travail (la partie du fichier que l'on découpe)
    int getRegionStart() const                   { return regionStart; }
    int getRegionEnd() const                     { return regionEnd; }
    void setRegion (int newStart, int newEnd);

    //==============================================================================
    // Tranches
    int getNumSlices() const                     { return numSlices; }
    const Slice& getSlice (int sliceIndex) const { return slices[(size_t) sliceIndex]; }
    int getSliceEnd (int sliceIndex) const;
    int findSliceAt (int samplePosition) const;

    void setSliceParams (int sliceIndex, const SliceParams& params);
    void setAllSliceParams (const SliceParams& params);

    int  moveMarker (int sliceIndex, int newPosition);   // renvoie la position réellement appliquée
    int  addMarker (int position);                       // renvoie l'index de la nouvelle tranche, ou -1
    bool removeMarker (int sliceIndex);

    int getSelectedSlice() const                 { return selectedSlice.load(); }
    void setSelectedSlice (int sliceIndex);

    /** Met toutes les tranches au même niveau de crête. */
    void normaliseSlices();

    //==============================================================================
    // Découpe automatique
    SliceMode getSliceMode() const               { return sliceMode; }
    float getSensitivity() const                 { return sensitivity; }
    int getDivisions() const                     { return divisions; }

    void setSliceMode (SliceMode newMode);
    void setSensitivity (float newSensitivity);
    void setDivisions (int newDivisions);        // nombre de parts, ou une valeur de siample::grid

    //==============================================================================
    // Tempo et tonalité
    double getSampleBpm() const                  { return sampleBpm; }
    bool isBpmManual() const                     { return bpmManual; }
    void setSampleBpm (double bpm);              // tempo saisi à la main
    void detectBpm();                            // relance la détection automatique

    /** Sync : les tranches suivent le tempo du projet, et le pitch ne change plus leur durée. */
    bool isSyncEnabled() const                   { return syncEnabled; }
    void setSyncEnabled (bool shouldSync);

    DetectedKey getDetectedKey() const           { return detectedKey; }

    /** Transposition de tout le sample, en demi-tons : elle s'ajoute au pitch de chaque tranche. */
    int getTranspose() const                     { return transpose; }
    void setTranspose (int semitones);

    bool isRendering() const;
    void waitForRendering (int timeoutMs);

    //==============================================================================
    // Jeu
    PlayMode getPlayMode() const                 { return playMode; }
    void setPlayMode (PlayMode newMode);

    bool isMono() const                          { return monoMode; }
    void setMono (bool shouldBeMono);

    /** Durée d'une tranche telle qu'on l'entend (pitch et Sync compris). */
    double sliceSourceDuration (int sliceIndex) const;   // en échantillons du sample
    double getSliceSeconds (int sliceIndex) const;

    /** Un bit par tranche en cours de lecture. */
    juce::uint64 getActiveMask() const           { return activeMask.load (std::memory_order_relaxed); }

    /** Position de lecture dans le sample, ou -1 si rien ne joue. */
    int getPlayheadPosition() const              { return playhead.load (std::memory_order_relaxed); }

    //==============================================================================
    juce::ValueTree toTree() const;
    void fromTree (const juce::ValueTree& tree, bool reuseLoadedSample);

private:
    friend class ::SiampleProcessor;
    class RenderWorker;
    struct RenderJob;

    void installSample (SampleData::Ptr newSample);
    void analyseRegion();
    void reslice();
    void applyPositions (const std::vector<int>& positions);
    void slicesChanged();              // à appeler après toute modification des tranches
    void scheduleRender();
    void collectGarbage();
    void hostTempoTick();              // appelé régulièrement : recalcule si le tempo du projet a bougé

    int effectivePitch (int sliceIndex) const    { return slices[(size_t) sliceIndex].params.pitch + transpose; }

    SiampleProcessor& owner;
    const int index;

    SampleData::Ptr sample;
    std::array<Slice, maxSlices> slices;
    int numSlices   = 0;
    int regionStart = 0;
    int regionEnd   = 0;

    // Réglages de jeu, lus par le thread audio sous le verrou
    PlayMode playMode = PlayMode::pads;
    bool monoMode = false;
    bool syncActive = false;           // sync demandé ET sample chargé
    double stretchRatio = 1.0;         // facteur de durée appliqué aux tranches étirées
    int transpose = 0;

    // Tranches étirées
    std::array<RenderedSlice::Ptr, maxSlices> rendered;
    std::vector<RenderedSlice::Ptr> retired;   // remplacées, gardées en vie tant qu'une voix les lit
    std::unique_ptr<RenderWorker> worker;
    juce::CriticalSection scheduleLock;
    double renderedBpm = 120.0;
    double lastSeenHostBpm = 120.0;

    // État sauvegardé avec le projet
    std::atomic<int> selectedSlice { 0 };
    SliceMode sliceMode = SliceMode::transients;
    float sensitivity = 0.5f;
    int divisions = 16;
    double sampleBpm = 120.0;
    bool bpmManual = false;
    bool syncEnabled = false;
    juce::String missingFilePath;
    juce::ValueTree orphanState;

    std::vector<Onset> candidates;
    DetectedKey detectedKey;
    bool analysisValid = false;

    std::atomic<juce::uint64> activeMask { 0 };
    std::atomic<int> playhead { -1 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Deck)
};

} // namespace siample
