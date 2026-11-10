#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_data_structures/juce_data_structures.h>
#include <array>
#include <atomic>

#include "Slice.h"
#include "Voice.h"
#include "TransientDetector.h"

/*  Le cœur de Siample.

    Règle d'accès aux données :
      - le thread audio LIT le sample, les tranches et les réglages de jeu, toujours sous `audioLock` ;
      - l'interface (thread principal) est la seule à les MODIFIER, sous `audioLock` aussi ;
      - l'interface peut donc les lire librement sans verrou ;
      - les tranches étirées sont calculées par un thread de fond, puis installées sous `audioLock`.
*/
class SiampleProcessor final : public juce::AudioProcessor,
                               public juce::ChangeBroadcaster,
                               private juce::Timer
{
public:
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
        int start = 0, end = 0, pitch = 0;
        float fine = 0.0f;
        double ratio = 1.0;

        bool matches (int s, int e, int p, float f, double r) const
        {
            return start == s && end == e && pitch == p
                && std::abs (fine - f) < 0.001f && std::abs (ratio - r) < 1.0e-6;
        }
    };

    SiampleProcessor();
    ~SiampleProcessor() override;

    //==============================================================================
    // Sample
    /** Charge un fichier audio. Renvoie un message d'erreur, ou une chaîne vide si tout va bien. */
    juce::String loadFile (const juce::File& file);

    SampleData::Ptr getSample() const;
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
    const siample::Slice& getSlice (int index) const { return slices[(size_t) index]; }
    int getSliceEnd (int index) const;
    int findSliceAt (int samplePosition) const;

    void setSliceParams (int index, const siample::SliceParams& params);
    void setAllSliceParams (const siample::SliceParams& params);

    int  moveMarker (int index, int newPosition);     // renvoie la position réellement appliquée
    int  addMarker (int position);                    // renvoie l'index de la nouvelle tranche, ou -1
    bool removeMarker (int index);

    int getSelectedSlice() const                 { return selectedSlice.load(); }
    void setSelectedSlice (int index);

    //==============================================================================
    // Découpe automatique
    siample::SliceMode getSliceMode() const      { return sliceMode; }
    float getSensitivity() const                 { return sensitivity; }
    int getDivisions() const                     { return divisions; }

    void setSliceMode (siample::SliceMode newMode);
    void setSensitivity (float newSensitivity);
    void setDivisions (int newDivisions);        // nombre de parts, ou une valeur de siample::grid

    //==============================================================================
    // Tempo
    double getSampleBpm() const                  { return sampleBpm; }
    bool isBpmManual() const                     { return bpmManual; }
    void setSampleBpm (double bpm);              // tempo saisi à la main
    void detectBpm();                            // relance la détection automatique

    double getHostBpm() const                    { return hostBpm.load(); }

    /** Sync : les tranches suivent le tempo du projet, et le pitch ne change plus leur durée. */
    bool isSyncEnabled() const                   { return syncEnabled; }
    void setSyncEnabled (bool shouldSync);

    bool isRendering() const;
    void waitForRendering (int timeoutMs);

    //==============================================================================
    // Jeu
    siample::PlayMode getPlayMode() const        { return playMode; }
    void setPlayMode (siample::PlayMode newMode);

    bool isMono() const                          { return monoMode; }
    void setMono (bool shouldBeMono);

    //==============================================================================
    // Boucle : l'ordre dans lequel les tranches sont rejouées
    bool isPatternFlipped() const                { return patternFlipped; }
    int getPatternSlice (int step) const         { return patternSlices[(size_t) step]; }
    void flipPattern();                          // nouveau réarrangement aléatoire
    void resetPattern();                         // retour à l'ordre d'origine

    bool isPatternPlaying() const                { return patternPlaying.load(); }
    void setPatternPlaying (bool shouldPlay);

    /** Écrit la boucle dans un fichier MIDI temporaire, à glisser dans le DAW. */
    juce::File createPatternMidiFile();

    //==============================================================================
    // Écoute depuis l'interface (clic sur un pad ou sur la forme d'onde)
    void audition (int sliceIndex, bool noteOn);

    /** Un bit par tranche en cours de lecture. */
    juce::uint64 getActiveMask() const           { return activeMask.load (std::memory_order_relaxed); }

    /** Position de lecture dans le sample, ou -1 si rien ne joue. */
    int getPlayheadPosition() const              { return playhead.load (std::memory_order_relaxed); }

    juce::AudioParameterFloat* getMasterGainParameter() const { return masterGain; }
    juce::AudioParameterFloat* getGrainParameter() const      { return grain; }
    juce::AudioFormatManager& getFormatManager() { return formatManager; }

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override              { return true; }

    const juce::String getName() const override  { return JucePlugin_Name; }
    bool acceptsMidi() const override            { return true; }
    bool producesMidi() const override           { return false; }
    bool isMidiEffect() const override           { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override                { return 1; }
    int getCurrentProgram() override             { return 0; }
    void setCurrentProgram (int) override        {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

private:
    class RenderWorker;
    struct RenderJob;

    //==============================================================================
    SampleData::Ptr readFile (const juce::File& file, juce::String& error);
    void installSample (SampleData::Ptr newSample);
    void analyseRegion();
    void reslice();
    void applyPositions (const std::vector<int>& positions);
    void slicesChanged();              // à appeler après toute modification des tranches
    void killAllVoices();
    void scheduleRender();
    void collectGarbage();
    void timerCallback() override;

    // Thread audio
    struct Event { int time; int type; int value; float velocity; };
    void handleEvent (const Event& event);
    void startVoice (int sliceIndex, int key, float velocity, double extraSemitones);
    void releaseKey (int key);
    void renderVoices (juce::AudioBuffer<float>& buffer, int startSample, int numSamples);
    void applyGrain (juce::AudioBuffer<float>& buffer);

    //==============================================================================
    juce::AudioFormatManager formatManager;
    juce::AudioParameterFloat* masterGain = nullptr;
    juce::AudioParameterFloat* grain = nullptr;

    juce::SpinLock audioLock;
    SampleData::Ptr sample;
    std::array<siample::Slice, siample::maxSlices> slices;
    int numSlices   = 0;
    int regionStart = 0;
    int regionEnd   = 0;

    // Réglages de jeu, lus par le thread audio sous audioLock
    siample::PlayMode playMode = siample::PlayMode::pads;
    bool monoMode = false;
    bool syncActive = false;           // sync demandé ET sample chargé
    double stretchRatio = 1.0;         // facteur de durée appliqué aux tranches étirées
    std::array<int, siample::maxSlices> patternSlices {};
    bool patternFlipped = false;

    // Tranches étirées
    std::array<RenderedSlice::Ptr, siample::maxSlices> rendered;
    std::vector<RenderedSlice::Ptr> retired;   // remplacées, gardées en vie tant qu'une voix les lit
    std::unique_ptr<RenderWorker> worker;
    juce::CriticalSection scheduleLock;
    double renderedBpm = 120.0;
    double lastSeenHostBpm = 120.0;

    // État de l'interface, sauvegardé avec le projet
    std::atomic<int> selectedSlice { 0 };
    siample::SliceMode sliceMode = siample::SliceMode::transients;
    float sensitivity = 0.5f;
    int divisions = 16;
    double sampleBpm = 120.0;
    bool bpmManual = false;
    bool syncEnabled = false;
    juce::String missingFilePath;
    juce::ValueTree orphanState;

    std::vector<siample::Onset> candidates;
    bool analysisValid = false;

    // Voix
    static constexpr int numVoices = 32;
    std::array<siample::Voice, numVoices> voices;
    juce::uint64 voiceCounter = 0;
    double hostRate = 44100.0;
    float lastMasterGain = 1.0f;
    std::atomic<juce::uint64> activeMask { 0 };
    std::atomic<int> playhead { -1 };
    std::atomic<double> hostBpm { 120.0 };

    // Lecture interne de la boucle
    std::atomic<bool> patternPlaying { false };
    bool patternWasPlaying = false;
    double patternPosition = 0.0;

    // Grain vintage
    float grainPhase = 0.0f, grainHeldL = 0.0f, grainHeldR = 0.0f;

    // File d'attente des écoutes venant de l'interface
    struct UiEvent { int slice; bool on; };
    juce::SpinLock uiLock;
    std::array<UiEvent, 32> uiEvents {};
    int numUiEvents = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SiampleProcessor)
};
