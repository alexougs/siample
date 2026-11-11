#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "ui/Theme.h"
#include "ui/WaveformView.h"
#include "ui/PadGrid.h"
#include "ui/SlicePanel.h"
#include "ui/DragExportButton.h"
#include "ui/SequenceLane.h"

class SiampleEditor final : public juce::AudioProcessorEditor,
                            public juce::FileDragAndDropTarget,
                            public juce::DragAndDropContainer,
                            private juce::ChangeListener,
                            private juce::Timer
{
public:
    explicit SiampleEditor (SiampleProcessor&);
    ~SiampleEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray& files, int x, int y) override;
    void fileDragExit (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;

    void openFileChooser();
    void loadFile (const juce::File& file);
    void refresh();            // tout remettre à jour d'après le processeur
    void selectionChanged();
    void updateHostLabel();

    static void styleBarLabel (juce::Label& label, const juce::String& text, bool strong);

    SiampleProcessor& proc;
    siample::SiampleLookAndFeel lookAndFeel;
    juce::TooltipWindow tooltipWindow { this, 700 };

    // En-tête
    juce::TextButton loadButton;
    juce::Label fileLabel;
    juce::Slider masterSlider, grainSlider;
    juce::Label masterLabel, grainLabel;
    std::unique_ptr<juce::SliderParameterAttachment> masterAttachment, grainAttachment;

    // Barre de découpe
    juce::Label sliceLabel;
    juce::TextButton transientsButton, gridButton, manualButton;
    juce::Label optionLabel;
    juce::Slider sensitivitySlider;
    juce::ComboBox divisionsBox;
    juce::Label countLabel;

    // Barre de tempo et de jeu
    juce::Label tempoLabel, bpmLabel, hostLabel, playLabel;
    juce::TextButton halfButton, doubleButton, autoButton, syncButton;
    juce::TextButton padsButton, keysButton, monoButton;

    siample::WaveformView waveform;
    juce::Label hintLabel;

    // Barre de boucle
    juce::Label loopLabel, loopInfoLabel;
    juce::TextButton listenButton, flipButton, originalButton;
    siample::DragExportButton midiDrag;

    // Piste : des tranches enchaînées
    juce::Label sequenceLabel, sequenceInfoLabel;
    juce::TextButton sequenceListenButton, sequenceAddButton, sequenceClearButton;
    siample::DragExportButton sequenceAudioDrag, sequenceMidiDrag;
    siample::SequenceLane sequenceLane;

    siample::PadGrid pads;
    siample::SlicePanel slicePanel;

    std::unique_ptr<juce::FileChooser> chooser;
    SiampleProcessor::SampleData::Ptr shownSample;
    juce::String errorMessage;
    juce::uint64 lastMask = 0;
    double shownHostBpm = 0.0;
    bool shownRendering = false;
    bool shownPlaying = false;
    bool shownSequencePlaying = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SiampleEditor)
};
