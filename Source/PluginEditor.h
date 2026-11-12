#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "ui/Theme.h"
#include "ui/WaveformView.h"
#include "ui/PadGrid.h"
#include "ui/SlicePanel.h"
#include "ui/DragExportButton.h"
#include "ui/TimelineView.h"
#include "ui/BrowserPanel.h"

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

    /** Affiche la découpe (false) ou le montage (true). */
    void showMontage (bool shouldShowMontage);
    void showBrowser (bool shouldShow);

   #if SIAMPLE_TESTS
    void timerCallbackForTests()   { timerCallback(); }
   #endif

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;

    void openFileChooser();
    void loadFile (const juce::File& file);
    void showMenu();
    void savePreset();
    void loadPreset();
    void refresh();            // tout remettre à jour d'après le processeur
    void selectionChanged();
    void updateStatus();

    static void styleCaption (juce::Label& label, const juce::String& text);
    static void styleValueBox (juce::Label& label, const juce::String& tooltip);

    SiampleProcessor& proc;
    siample::SiampleLookAndFeel lookAndFeel;
    juce::TooltipWindow tooltipWindow { this, 700 };

    // Ligne du haut : tout ce qui vaut pour le plugin entier
    juce::TextButton sliceViewButton, montageViewButton, undoButton, redoButton, browseButton, menuButton;
    juce::Label projectCaption, projectLabel, repeatCaption, grainCaption, masterCaption;
    juce::TextButton velocityButton;
    juce::ComboBox repeatBox;
    juce::Slider masterSlider, grainSlider;
    std::unique_ptr<juce::SliderParameterAttachment> masterAttachment, grainAttachment;

    // Ligne du sample affiché
    std::array<juce::TextButton, siample::numDecks> deckButtons;
    juce::TextButton loadButton;
    juce::Label fileLabel;
    juce::Label tempoCaption, bpmLabel;
    juce::TextButton halfButton, doubleButton, autoButton, syncButton;
    juce::Label keyCaption, keyLabel, transposeLabel;
    juce::TextButton transposeDownButton, transposeUpButton;
    juce::ComboBox targetKeyBox;

    // Vue Découpe
    juce::Label sliceCaption, optionLabel, countLabel, playCaption;
    juce::TextButton transientsButton, gridButton, manualButton, normaliseButton;
    juce::Slider sensitivitySlider;
    juce::ComboBox divisionsBox;
    juce::TextButton padsButton, keysButton, monoButton;
    siample::WaveformView waveform;

    // Vue Montage
    juce::TextButton listenButton, orderButton, flipButton, clearButton;
    std::array<juce::TextButton, siample::numVariants> variantButtons;
    juce::Label barsCaption, snapCaption, timelineInfoLabel;
    juce::ComboBox barsBox, snapBox;
    siample::DragExportButton timelineAudioDrag, timelineMidiDrag;
    siample::TimelineView timeline;

    juce::Label hintLabel;

    siample::BrowserPanel browser;
    siample::PadGrid pads;
    siample::SlicePanel slicePanel;

    std::unique_ptr<juce::FileChooser> chooser;
    SiampleProcessor::SampleData::Ptr shownSample;
    juce::String errorMessage;
    juce::Rectangle<int> middleCard;
    juce::uint64 lastMask = 0;
    double shownHostBpm = 0.0;
    bool shownRendering = false;
    bool shownPlaying = false;
    bool shownTempoFromHost = false;
    bool montageVisible = false;
    bool browserVisible = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SiampleEditor)
};
