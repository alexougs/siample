#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_data_structures/juce_data_structures.h>
#include <array>
#include <atomic>

#include "Slice.h"
#include "Voice.h"
#include "Deck.h"

/*  Le cœur de Siample : quatre samples (voir Deck), les voix qui les jouent, le montage et la sauvegarde.

    Les fonctions « du sample affiché » (loadFile, getNumSlices, setSliceParams…) s'adressent au sample
    dont l'onglet est ouvert ; getDeck (i) donne accès aux autres.
*/
class SiampleProcessor final : public juce::AudioProcessor,
                               public juce::ChangeBroadcaster,
                               private juce::Timer
{
public:
    using SampleData    = siample::SampleData;
    using RenderedSlice = siample::RenderedSlice;

    SiampleProcessor();
    ~SiampleProcessor() override;

    //==============================================================================
    // Les quatre samples
    siample::Deck& getDeck (int index)                 { return *decks[(size_t) juce::jlimit (0, siample::numDecks - 1, index)]; }
    const siample::Deck& getDeck (int index) const     { return *decks[(size_t) juce::jlimit (0, siample::numDecks - 1, index)]; }
    siample::Deck& deck()                              { return getDeck (activeDeck.load()); }
    const siample::Deck& deck() const                  { return getDeck (activeDeck.load()); }

    int getActiveDeck() const                    { return activeDeck.load(); }
    void setActiveDeck (int index);

    //==============================================================================
    // Le sample affiché
    juce::String loadFile (const juce::File& file)       { return deck().loadFile (file); }
    SampleData::Ptr getSample() const                    { return deck().getSample(); }
    bool hasSample() const                               { return deck().hasSample(); }
    int getSampleLength() const                          { return deck().getSampleLength(); }
    double getSourceSampleRate() const                   { return deck().getSourceSampleRate(); }
    juce::String getFileName() const                     { return deck().getFileName(); }
    juce::String getMissingFilePath() const              { return deck().getMissingFilePath(); }

    int getRegionStart() const                           { return deck().getRegionStart(); }
    int getRegionEnd() const                             { return deck().getRegionEnd(); }
    void setRegion (int newStart, int newEnd)            { deck().setRegion (newStart, newEnd); }

    int getNumSlices() const                             { return deck().getNumSlices(); }
    const siample::Slice& getSlice (int index) const     { return deck().getSlice (index); }
    int getSliceEnd (int index) const                    { return deck().getSliceEnd (index); }
    int findSliceAt (int samplePosition) const           { return deck().findSliceAt (samplePosition); }
    void setSliceParams (int index, const siample::SliceParams& params)  { deck().setSliceParams (index, params); }
    void setAllSliceParams (const siample::SliceParams& params)          { deck().setAllSliceParams (params); }
    int  moveMarker (int index, int newPosition)         { return deck().moveMarker (index, newPosition); }
    int  addMarker (int position)                        { return deck().addMarker (position); }
    bool removeMarker (int index)                        { return deck().removeMarker (index); }
    int getSelectedSlice() const                         { return deck().getSelectedSlice(); }
    void setSelectedSlice (int index)                    { deck().setSelectedSlice (index); }

    siample::SliceMode getSliceMode() const              { return deck().getSliceMode(); }
    float getSensitivity() const                         { return deck().getSensitivity(); }
    int getDivisions() const                             { return deck().getDivisions(); }
    void setSliceMode (siample::SliceMode newMode)       { deck().setSliceMode (newMode); }
    void setSensitivity (float newSensitivity)           { deck().setSensitivity (newSensitivity); }
    void setDivisions (int newDivisions)                 { deck().setDivisions (newDivisions); }

    double getSampleBpm() const                          { return deck().getSampleBpm(); }
    bool isBpmManual() const                             { return deck().isBpmManual(); }
    void setSampleBpm (double bpm)                       { deck().setSampleBpm (bpm); }
    void detectBpm()                                     { deck().detectBpm(); }
    bool isSyncEnabled() const                           { return deck().isSyncEnabled(); }
    void setSyncEnabled (bool shouldSync)                { deck().setSyncEnabled (shouldSync); }

    siample::PlayMode getPlayMode() const                { return deck().getPlayMode(); }
    void setPlayMode (siample::PlayMode newMode)         { deck().setPlayMode (newMode); }
    bool isMono() const                                  { return deck().isMono(); }
    void setMono (bool shouldBeMono)                     { deck().setMono (shouldBeMono); }

    double getSliceSeconds (int index) const             { return deck().getSliceSeconds (index); }
    juce::uint64 getActiveMask() const                   { return deck().getActiveMask(); }
    int getPlayheadPosition() const                      { return deck().getPlayheadPosition(); }

    /** Vrai tant qu'un des samples calcule ses tranches étirées. */
    bool isRendering() const;
    void waitForRendering (int timeoutMs);

    //==============================================================================
    // Tempo du projet
    double getHostBpm() const                    { return hostBpm.load(); }

    /** Faux tant que le DAW n'a pas donné de tempo (version autonome) : on peut alors le régler à la main. */
    bool isTempoFromHost() const                 { return tempoFromHost.load(); }
    void setProjectBpm (double bpm);

    //==============================================================================
    // Jeu
    /** Toutes les notes jouent à fond, quelle que soit la force de frappe. */
    bool isFullVelocity() const                  { return fullVelocity.load(); }
    void setFullVelocity (bool shouldBeFull)     { fullVelocity.store (shouldBeFull); }

    /** Répétition tant qu'une note est tenue : 0 = non, sinon 4, 8, 16 ou 32 (noires … triples croches). */
    int getNoteRepeat() const                    { return noteRepeat.load(); }
    void setNoteRepeat (int division);

    //==============================================================================
    // Montage : des blocs (une tranche d'un des samples) posés sur une grille en temps
    int getVariant() const                       { return variant.load(); }
    void setVariant (int index);
    void copyVariant (int from, int to);
    int getNumClipsIn (int variantIndex) const   { return clipCounts[(size_t) variantIndex]; }

    int getNumClips() const                      { return clipCounts[(size_t) variant.load()]; }
    const siample::Clip& getClip (int index) const { return clips[(size_t) variant.load()][(size_t) index]; }

    /** Ajoute un bloc. length < 0 : la durée naturelle de la tranche. Renvoie son index, ou -1. */
    int addClip (int deckIndex, int sliceIndex, double startBeats, double lengthBeats = -1.0);
    /** Déplace un bloc (et le change de ligne si deckIndex >= 0 n'a pas de sens ici : la ligne suit le sample). */
    void moveClip (int index, double newStartBeats);
    void resizeClip (int index, double newLengthBeats);
    void removeClip (int index);
    void clearClips (int deckIndex = -1);        // -1 : tout le montage

    /** Remplit la ligne d'un sample avec sa boucle : dans l'ordre d'origine, ou réarrangée au hasard. */
    void fillLane (int deckIndex, bool shuffled);

    int getTimelineBars() const                  { return timelineBars; }
    void setTimelineBars (int bars);
    double getTimelineBeats() const              { return (double) (timelineBars * siample::beatsPerBar); }

    /** Grille d'aimantation : 0 = libre, sinon 4, 8, 16 ou 32. */
    int getSnap() const                          { return snapDivision; }
    void setSnap (int division);
    double snapBeats (double beats) const;
    double getSnapStep() const                   { return snapDivision > 0 ? 4.0 / (double) snapDivision : 0.0; }

    /** Durée naturelle d'une tranche, en temps du projet. */
    double naturalBeats (int deckIndex, int sliceIndex) const;

    bool isTimelinePlaying() const               { return timelinePlaying.load(); }
    void setTimelinePlaying (bool shouldPlay);
    /** Position de lecture du montage en temps, ou -1. */
    double getTimelinePosition() const           { return timelinePositionUi.load (std::memory_order_relaxed); }

    //==============================================================================
    // Export : des fichiers temporaires, à glisser dans le DAW
    juce::File createSliceAudioFile (int sliceIndex);            // tranche du sample affiché, avec ses réglages
    juce::Array<juce::File> createAllSliceAudioFiles();          // toutes les tranches du sample affiché
    juce::File createTimelineAudioFile();
    juce::File createTimelineMidiFile();

    bool renderSliceAudio (int deckIndex, int sliceIndex, juce::AudioBuffer<float>& result, bool withGrain = true);
    bool renderTimelineAudio (juce::AudioBuffer<float>& result, double& sampleRate);

    //==============================================================================
    // Annuler / rétablir
    bool canUndo() const                         { return ! undoStack.empty(); }
    bool canRedo() const                         { return ! redoStack.empty(); }
    void undo();
    void redo();

    /** À appeler juste AVANT une modification : garde une photo de l'état pour pouvoir y revenir.
        continuous : deux appels rapprochés avec la même étiquette (un potard qu'on tourne, un bloc
        qu'on déplace) ne font qu'une seule étape. */
    void beginEdit (const juce::String& tag, bool continuous = false);

    //==============================================================================
    // Presets : tout l'état du plugin dans un fichier .siample
    bool savePreset (const juce::File& file);
    juce::String loadPreset (const juce::File& file);

    //==============================================================================
    // Pré-écoute d'un fichier (navigateur de samples)
    juce::String startPreview (const juce::File& file);
    void stopPreview();
    bool isPreviewPlaying() const                { return previewPlaying.load (std::memory_order_relaxed); }

    //==============================================================================
    // Écoute depuis l'interface (clic sur un pad ou sur la forme d'onde)
    void audition (int sliceIndex, bool noteOn)  { audition (activeDeck.load(), sliceIndex, noteOn); }
    void audition (int deckIndex, int sliceIndex, bool noteOn);

    /** Ce que l'interface affichait en dernier (0 = découpe, 1 = montage) : retenu avec le projet. */
    int getShownView() const                     { return shownView; }
    void setShownView (int view)                 { shownView = view == 1 ? 1 : 0; }

    juce::AudioParameterFloat* getMasterGainParameter() const { return masterGain; }
    juce::AudioParameterFloat* getGrainParameter() const      { return grain; }
    juce::AudioFormatManager& getFormatManager() { return formatManager; }

    /** Lit un fichier audio en mémoire. maxSeconds > 0 : seulement le début. */
    SampleData::Ptr readFile (const juce::File& file, juce::String& error, double maxSeconds = 0.0);

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
    friend class siample::Deck;

    //==============================================================================
    juce::ValueTree createState() const;
    void applyState (const juce::ValueTree& state, bool reuseLoadedSamples);
    void killAllVoices();
    void killDeckVoices (int deckIndex);          // sous le verrou audio
    void deckSlicesChanged (int deckIndex);       // des tranches ont disparu : le montage suit
    void deckSampleReplaced (int deckIndex);
    void timerCallback() override;
    juce::File writeTempWav (const juce::AudioBuffer<float>& buffer, double sampleRate, const juce::File& file);
    juce::File tempFile (const juce::String& name) const;

    // Thread audio
    struct Event { int time; int type; int value; float velocity; int channel; };
    struct Held { bool active; int key; int deck; int slice; float velocity; double countdown; };

    void handleEvent (const Event& event);
    bool prepareVoice (int deckIndex, int sliceIndex, const RenderedSlice* stretched, double outputRate,
                       double extraSemitones, siample::VoiceStart& start) const;
    void startVoice (int deckIndex, int sliceIndex, int key, float velocity, double extraSemitones,
                     int lane = -1, double life = -1.0);
    void releaseKey (int key, bool force);
    void holdNote (int key, int deckIndex, int sliceIndex, float velocity, int time);
    void unholdNote (int key);
    void renderVoices (juce::AudioBuffer<float>& buffer, int startSample, int numSamples);
    void renderPreview (juce::AudioBuffer<float>& buffer);

    //==============================================================================
    juce::AudioFormatManager formatManager;
    juce::AudioParameterFloat* masterGain = nullptr;
    juce::AudioParameterFloat* grain = nullptr;

    juce::SpinLock audioLock;
    std::array<std::unique_ptr<siample::Deck>, siample::numDecks> decks;
    std::atomic<int> activeDeck { 0 };

    // Montage (modifié par l'interface sous audioLock, lu par le thread audio)
    std::array<std::array<siample::Clip, siample::maxClips>, siample::numVariants> clips;
    std::array<int, siample::numVariants> clipCounts {};
    std::atomic<int> variant { 0 };
    int shownView = 0;
    int timelineBars = 2;
    int snapDivision = 16;
    std::atomic<bool> timelinePlaying { false };
    std::atomic<double> timelinePositionUi { -1.0 };
    bool timelineWasPlaying = false;
    double timelinePosition = 0.0;

    // Jeu
    std::atomic<bool> fullVelocity { false };
    std::atomic<int> noteRepeat { 0 };
    std::array<Held, 16> held {};

    // Voix
    static constexpr int numVoices = 48;
    std::array<siample::Voice, numVoices> voices;
    juce::uint64 voiceCounter = 0;
    double hostRate = 44100.0;
    int currentBlockSize = 0;
    float lastMasterGain = 1.0f;
    std::atomic<double> hostBpm { 120.0 };
    std::atomic<bool> tempoFromHost { false };

    // Grain vintage
    float grainPhase = 0.0f, grainHeldL = 0.0f, grainHeldR = 0.0f;

    // Pré-écoute
    SampleData::Ptr preview;
    double previewPosition = 0.0;
    std::atomic<bool> previewPlaying { false };

    // File d'attente des écoutes venant de l'interface
    struct UiEvent { int deck; int slice; bool on; };
    juce::SpinLock uiLock;
    std::array<UiEvent, 32> uiEvents {};
    int numUiEvents = 0;

    // Annuler / rétablir
    std::vector<juce::ValueTree> undoStack, redoStack;
    juce::String lastEditTag;
    juce::uint32 lastEditTime = 0;
    int editSuspended = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SiampleProcessor)
};
