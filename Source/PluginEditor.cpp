#include "PluginEditor.h"

using namespace siample;

namespace
{
    const int divisionChoices[] = { 2, 4, 8, 16, 32, 64 };

    // Les identifiants d'un menu doivent être positifs : on décale les valeurs musicales
    constexpr int musicalIdOffset = 100;

    int divisionsToId (int divisions)   { return divisions > 0 ? divisions : musicalIdOffset - divisions; }
    int idToDivisions (int id)          { return id > musicalIdOffset ? musicalIdOffset - id : id; }

    void styleRotary (juce::Slider& slider, juce::Component* popupParent, const juce::String& suffix, const juce::String& tooltip)
    {
        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider.setPopupDisplayEnabled (true, true, popupParent);
        slider.setTextValueSuffix (suffix);
        slider.setTooltip (tooltip);
    }

    void styleToggle (juce::TextButton& button, const juce::String& text, const juce::String& tooltip)
    {
        button.setButtonText (text);
        button.setTooltip (tooltip);
        button.setClickingTogglesState (true);
    }
}

void SiampleEditor::styleBarLabel (juce::Label& label, const juce::String& text, bool strong)
{
    label.setText (text, juce::dontSendNotification);
    label.setFont (theme::font (strong ? 14.0f : 13.0f, strong));
    label.setColour (juce::Label::textColourId, strong ? theme::text : theme::textDim);
    label.setMinimumHorizontalScale (0.8f);
}

//==============================================================================
SiampleEditor::SiampleEditor (SiampleProcessor& p)
    : AudioProcessorEditor (&p), proc (p), waveform (p), midiDrag (p), pads (p), slicePanel (p)
{
    setLookAndFeel (&lookAndFeel);

    // --- En-tête
    loadButton.setButtonText (fr ("Charger un sample"));
    loadButton.onClick = [this] { openFileChooser(); };
    addAndMakeVisible (loadButton);

    fileLabel.setFont (theme::font (14.0f));
    fileLabel.setMinimumHorizontalScale (0.8f);
    addAndMakeVisible (fileLabel);

    styleRotary (grainSlider, this, " %", fr ("Grain vintage : le son granuleux des vieux samplers (moins de bits, moins d'aigus)"));
    addAndMakeVisible (grainSlider);
    grainAttachment = std::make_unique<juce::SliderParameterAttachment> (*proc.getGrainParameter(), grainSlider);
    grainSlider.setDoubleClickReturnValue (true, 0.0);

    styleRotary (masterSlider, this, " dB", fr ("Volume général du plugin"));
    addAndMakeVisible (masterSlider);
    masterAttachment = std::make_unique<juce::SliderParameterAttachment> (*proc.getMasterGainParameter(), masterSlider);
    masterSlider.setDoubleClickReturnValue (true, 0.0);

    styleBarLabel (grainLabel, "Grain", false);
    styleBarLabel (masterLabel, "Volume", false);

    for (auto* l : { &grainLabel, &masterLabel })
    {
        l->setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (*l);
    }

    // --- Barre de découpe
    styleBarLabel (sliceLabel, fr ("Découpe"), true);
    addAndMakeVisible (sliceLabel);

    transientsButton.setButtonText ("Transitoires");
    transientsButton.setTooltip (fr ("Coupe automatiquement à chaque attaque (kick, snare, note…)"));
    gridButton.setButtonText (fr ("Grille"));
    gridButton.setTooltip (fr ("Coupe la zone en parts égales, ou en temps d'après le tempo"));
    manualButton.setButtonText ("Manuel");
    manualButton.setTooltip (fr ("Tu places les points toi-même : double-clic sur la forme d'onde"));

    juce::TextButton* modeButtons[] = { &transientsButton, &gridButton, &manualButton };

    for (int i = 0; i < 3; ++i)
    {
        auto* b = modeButtons[i];
        b->setClickingTogglesState (true);
        b->setRadioGroupId (7100);
        b->setConnectedEdges ((i > 0 ? juce::Button::ConnectedOnLeft : 0) | (i < 2 ? juce::Button::ConnectedOnRight : 0));
        b->onClick = [this, b, i]
        {
            if (b->getToggleState())
            {
                proc.setSliceMode ((SliceMode) i);
                refresh();
            }
        };
        addAndMakeVisible (*b);
    }

    styleBarLabel (optionLabel, {}, false);
    optionLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (optionLabel);

    sensitivitySlider.setSliderStyle (juce::Slider::LinearHorizontal);
    sensitivitySlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    sensitivitySlider.setRange (0.0, 1.0, 0.01);
    sensitivitySlider.setDoubleClickReturnValue (true, 0.5);
    sensitivitySlider.setTooltip (fr ("Vers la droite : plus de tranches. Vers la gauche : seulement les attaques les plus fortes."));
    sensitivitySlider.onValueChange = [this]
    {
        proc.setSensitivity ((float) sensitivitySlider.getValue());
        refresh();
    };
    addAndMakeVisible (sensitivitySlider);

    divisionsBox.addSectionHeading (fr ("Parts égales"));
    for (int n : divisionChoices)
        divisionsBox.addItem (juce::String (n) + " parts", divisionsToId (n));

    divisionsBox.addSectionHeading (fr ("D'après le tempo"));
    divisionsBox.addItem ("Mesures", divisionsToId (grid::bars));
    divisionsBox.addItem ("Noires (1/4)", divisionsToId (grid::quarters));
    divisionsBox.addItem ("Croches (1/8)", divisionsToId (grid::eighths));
    divisionsBox.addItem ("Doubles (1/16)", divisionsToId (grid::sixteenths));

    divisionsBox.onChange = [this]
    {
        if (divisionsBox.getSelectedId() > 0)
        {
            proc.setDivisions (idToDivisions (divisionsBox.getSelectedId()));
            refresh();
        }
    };
    addAndMakeVisible (divisionsBox);

    styleBarLabel (countLabel, {}, false);
    countLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (countLabel);

    // --- Barre de tempo et de jeu
    styleBarLabel (tempoLabel, "Tempo", true);
    addAndMakeVisible (tempoLabel);

    bpmLabel.setFont (theme::font (14.0f, true));
    bpmLabel.setJustificationType (juce::Justification::centred);
    bpmLabel.setEditable (true);
    bpmLabel.setColour (juce::Label::backgroundColourId, theme::panelLight);
    bpmLabel.setColour (juce::Label::outlineColourId, theme::outline);
    bpmLabel.setColour (juce::Label::backgroundWhenEditingColourId, theme::panelLight);
    bpmLabel.setColour (juce::Label::textWhenEditingColourId, theme::text);
    bpmLabel.setColour (juce::Label::outlineWhenEditingColourId, theme::accent);
    bpmLabel.setTooltip (fr ("Tempo du sample, détecté automatiquement. Clique pour le corriger à la main."));
    bpmLabel.onTextChange = [this]
    {
        const double typed = bpmLabel.getText().replaceCharacter (',', '.').retainCharacters ("0123456789.").getDoubleValue();

        if (typed >= 30.0 && typed <= 300.0)
            proc.setSampleBpm (typed);

        refresh();
    };
    addAndMakeVisible (bpmLabel);

    halfButton.setButtonText (fr ("÷2"));
    halfButton.setTooltip (fr ("Divise le tempo par deux, si la détection a compté deux fois trop vite"));
    halfButton.onClick = [this] { proc.setSampleBpm (proc.getSampleBpm() * 0.5); refresh(); };
    doubleButton.setButtonText (fr ("×2"));
    doubleButton.setTooltip (fr ("Double le tempo, si la détection a compté deux fois trop lentement"));
    doubleButton.onClick = [this] { proc.setSampleBpm (proc.getSampleBpm() * 2.0); refresh(); };
    autoButton.setButtonText ("Auto");
    autoButton.setTooltip (fr ("Relance la détection automatique du tempo sur la zone de travail"));
    autoButton.onClick = [this] { proc.detectBpm(); refresh(); };
    halfButton.setConnectedEdges (juce::Button::ConnectedOnRight);
    doubleButton.setConnectedEdges (juce::Button::ConnectedOnLeft);

    for (auto* b : { &halfButton, &doubleButton, &autoButton })
        addAndMakeVisible (*b);

    styleToggle (syncButton, "Sync", fr ("Les tranches suivent le tempo du projet, et le pitch ne change plus leur durée"));
    syncButton.onClick = [this] { proc.setSyncEnabled (syncButton.getToggleState()); refresh(); };
    addAndMakeVisible (syncButton);

    styleBarLabel (hostLabel, {}, false);
    addAndMakeVisible (hostLabel);

    styleBarLabel (playLabel, "Jeu", true);
    playLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (playLabel);

    styleToggle (padsButton, "Pads", fr ("Une note par tranche : la tranche 1 est sur la note ") + noteName (baseNote));
    styleToggle (keysButton, "Clavier", fr ("Tout le clavier joue la tranche sélectionnée ; la note ")
                                            + noteName (keyboardRootNote) + fr (" donne sa hauteur d'origine"));
    padsButton.setRadioGroupId (7103);
    keysButton.setRadioGroupId (7103);
    padsButton.setConnectedEdges (juce::Button::ConnectedOnRight);
    keysButton.setConnectedEdges (juce::Button::ConnectedOnLeft);
    padsButton.onClick = [this] { if (padsButton.getToggleState()) { proc.setPlayMode (PlayMode::pads); refresh(); } };
    keysButton.onClick = [this] { if (keysButton.getToggleState()) { proc.setPlayMode (PlayMode::keys); refresh(); } };
    addAndMakeVisible (padsButton);
    addAndMakeVisible (keysButton);

    styleToggle (monoButton, "Mono", fr ("Chaque nouvelle note coupe la précédente : les tranches ne se chevauchent plus"));
    monoButton.onClick = [this] { proc.setMono (monoButton.getToggleState()); refresh(); };
    addAndMakeVisible (monoButton);

    // --- Zone principale
    waveform.onEdit   = [this] { refresh(); };
    waveform.onSelect = [this] { selectionChanged(); };
    addAndMakeVisible (waveform);

    hintLabel.setFont (theme::font (12.0f));
    hintLabel.setColour (juce::Label::textColourId, theme::textDim);
    hintLabel.setMinimumHorizontalScale (0.7f);
    addAndMakeVisible (hintLabel);

    // --- Barre de boucle
    styleBarLabel (loopLabel, "Boucle", true);
    addAndMakeVisible (loopLabel);

    styleToggle (listenButton, fr ("Écouter"), fr ("Joue la boucle en continu, dans l'ordre affiché"));
    listenButton.onClick = [this] { proc.setPatternPlaying (listenButton.getToggleState()); refresh(); };
    addAndMakeVisible (listenButton);

    flipButton.setButtonText ("Flip");
    flipButton.setTooltip (fr ("Réarrange les tranches au hasard, en gardant le rythme d'origine. Reclique pour une autre idée."));
    flipButton.onClick = [this] { proc.flipPattern(); refresh(); };
    addAndMakeVisible (flipButton);

    originalButton.setButtonText ("Original");
    originalButton.setTooltip (fr ("Revient à l'ordre d'origine des tranches"));
    originalButton.onClick = [this] { proc.resetPattern(); refresh(); };
    addAndMakeVisible (originalButton);

    addAndMakeVisible (midiDrag);

    styleBarLabel (loopInfoLabel, {}, false);
    addAndMakeVisible (loopInfoLabel);

    // --- Bas
    pads.onSelect = [this] { selectionChanged(); };
    addAndMakeVisible (pads);

    slicePanel.onEdit = [this] { waveform.repaint(); };
    addAndMakeVisible (slicePanel);

    proc.addChangeListener (this);

    setResizable (true, true);
    setResizeLimits (900, 680, 1800, 1300);
    setSize (1020, 770);

    // les composants créés avant l'application du thème reprennent ses couleurs
    sendLookAndFeelChange();

    shownSample = proc.getSample();
    waveform.sampleChanged();
    refresh();
    startTimerHz (30);
}

SiampleEditor::~SiampleEditor()
{
    stopTimer();
    proc.removeChangeListener (this);
    masterAttachment.reset();
    grainAttachment.reset();
    setLookAndFeel (nullptr);
}

//==============================================================================
void SiampleEditor::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);

    // Nom du plugin
    juce::AttributedString name;
    name.setJustification (juce::Justification::centredLeft);
    name.append ("Si", theme::font (24.0f, true), theme::accent);
    name.append ("ample", theme::font (24.0f, true), theme::text);
    name.draw (g, getLocalBounds().reduced (16, 0).removeFromTop (56).removeFromLeft (118).toFloat());
}

void SiampleEditor::resized()
{
    auto area = getLocalBounds().reduced (16, 0);

    // En-tête
    auto header = area.removeFromTop (56);
    header.removeFromLeft (118);
    loadButton.setBounds (header.removeFromLeft (150).withSizeKeepingCentre (150, 30));
    header.removeFromLeft (12);
    masterSlider.setBounds (header.removeFromRight (44).withSizeKeepingCentre (44, 44));
    masterLabel.setBounds (header.removeFromRight (56));
    header.removeFromRight (6);
    grainSlider.setBounds (header.removeFromRight (44).withSizeKeepingCentre (44, 44));
    grainLabel.setBounds (header.removeFromRight (46));
    fileLabel.setBounds (header);

    // Barre de découpe
    auto bar = area.removeFromTop (30);
    sliceLabel.setBounds (bar.removeFromLeft (70));
    transientsButton.setBounds (bar.removeFromLeft (104));
    gridButton.setBounds (bar.removeFromLeft (70));
    manualButton.setBounds (bar.removeFromLeft (76));
    bar.removeFromLeft (10);
    countLabel.setBounds (bar.removeFromRight (110));
    optionLabel.setBounds (bar.removeFromLeft (86));
    bar.removeFromLeft (6);
    const auto optionArea = bar.removeFromLeft (juce::jmin (240, bar.getWidth()));
    sensitivitySlider.setBounds (optionArea);
    divisionsBox.setBounds (optionArea.withWidth (juce::jmin (150, optionArea.getWidth())));

    area.removeFromTop (8);

    // Barre de tempo et de jeu
    bar = area.removeFromTop (30);
    tempoLabel.setBounds (bar.removeFromLeft (70));
    bpmLabel.setBounds (bar.removeFromLeft (70).reduced (0, 1));
    bar.removeFromLeft (6);
    halfButton.setBounds (bar.removeFromLeft (34));
    doubleButton.setBounds (bar.removeFromLeft (34));
    bar.removeFromLeft (6);
    autoButton.setBounds (bar.removeFromLeft (52));
    bar.removeFromLeft (14);
    syncButton.setBounds (bar.removeFromLeft (64));
    bar.removeFromLeft (8);
    monoButton.setBounds (bar.removeFromRight (64));
    bar.removeFromRight (8);
    keysButton.setBounds (bar.removeFromRight (70));
    padsButton.setBounds (bar.removeFromRight (60));
    playLabel.setBounds (bar.removeFromRight (40));
    hostLabel.setBounds (bar);

    area.removeFromTop (10);
    area.removeFromBottom (14);

    // Bas : pads à gauche, réglages à droite
    const int bottomHeight = juce::jlimit (270, 350, area.getHeight() * 46 / 100);
    auto bottom = area.removeFromBottom (bottomHeight);
    const int padsWidth = juce::jmin (bottom.getWidth() * 40 / 100, (bottomHeight - 32) * 5 / 4);
    pads.setBounds (bottom.removeFromLeft (padsWidth));
    bottom.removeFromLeft (14);
    slicePanel.setBounds (bottom);

    // Barre de boucle
    area.removeFromBottom (12);
    bar = area.removeFromBottom (30);
    loopLabel.setBounds (bar.removeFromLeft (70));
    listenButton.setBounds (bar.removeFromLeft (84));
    bar.removeFromLeft (8);
    flipButton.setBounds (bar.removeFromLeft (64));
    bar.removeFromLeft (4);
    originalButton.setBounds (bar.removeFromLeft (80));
    bar.removeFromLeft (12);
    midiDrag.setBounds (bar.removeFromLeft (210));
    bar.removeFromLeft (10);
    loopInfoLabel.setBounds (bar);

    area.removeFromBottom (6);
    hintLabel.setBounds (area.removeFromBottom (20));
    area.removeFromBottom (2);
    waveform.setBounds (area);
}

//==============================================================================
bool SiampleEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
        if (proc.getFormatManager().findFormatForFileExtension (juce::File (f).getFileExtension()) != nullptr)
            return true;

    return false;
}

void SiampleEditor::fileDragEnter (const juce::StringArray&, int, int)   { waveform.setDropHighlight (true); }
void SiampleEditor::fileDragExit (const juce::StringArray&)              { waveform.setDropHighlight (false); }

void SiampleEditor::filesDropped (const juce::StringArray& files, int, int)
{
    waveform.setDropHighlight (false);

    for (const auto& f : files)
    {
        const juce::File file (f);

        if (proc.getFormatManager().findFormatForFileExtension (file.getFileExtension()) != nullptr)
        {
            loadFile (file);
            break;
        }
    }
}

void SiampleEditor::openFileChooser()
{
    const auto start = proc.hasSample() ? proc.getSample()->file.getParentDirectory()
                                        : juce::File::getSpecialLocation (juce::File::userMusicDirectory);

    chooser = std::make_unique<juce::FileChooser> (fr ("Choisis un sample"), start,
                                                   proc.getFormatManager().getWildcardForAllFormats());

    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto file = fc.getResult();

                              if (file.existsAsFile())
                                  loadFile (file);
                          });
}

void SiampleEditor::loadFile (const juce::File& file)
{
    errorMessage = proc.loadFile (file);

    if (errorMessage.isNotEmpty())
        errorMessage = file.getFileName() + " : " + errorMessage;

    // en cas de succès, le processeur prévient via changeListenerCallback ; on rafraîchit quand même tout de suite
    changeListenerCallback (nullptr);
}

//==============================================================================
void SiampleEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    const auto current = proc.getSample();

    if (current != shownSample)
    {
        shownSample = current;
        waveform.sampleChanged();
    }

    refresh();
}

void SiampleEditor::timerCallback()
{
    const auto mask = proc.getActiveMask();

    if (mask != lastMask)
    {
        lastMask = mask;
        pads.setActiveMask (mask);
        waveform.setActiveMask (mask);
    }

    waveform.setPlayhead (mask != 0 ? proc.getPlayheadPosition() : -1);

    const double host = proc.getHostBpm();
    const bool rendering = proc.isRendering();

    if (std::abs (host - shownHostBpm) > 0.005 || rendering != shownRendering)
        updateHostLabel();

    if (proc.isPatternPlaying() != shownPlaying)
        refresh();
}

void SiampleEditor::selectionChanged()
{
    pads.showSlice (proc.getSelectedSlice());
    slicePanel.refresh();
    waveform.repaint();
}

void SiampleEditor::updateHostLabel()
{
    shownHostBpm = proc.getHostBpm();
    shownRendering = proc.isRendering();

    juce::String text = "Projet : " + juce::String (shownHostBpm, 1) + " BPM";

    if (proc.isSyncEnabled() && proc.hasSample())
        text << fr ("  ·  ") << (shownRendering ? fr ("calcul en cours…") : fr ("calé"));

    hostLabel.setText (text, juce::dontSendNotification);
}

void SiampleEditor::refresh()
{
    const bool hasSample = proc.hasSample();
    const auto mode = proc.getSliceMode();

    // Nom du fichier ou message d'erreur
    if (errorMessage.isNotEmpty())
    {
        fileLabel.setText (errorMessage, juce::dontSendNotification);
        fileLabel.setColour (juce::Label::textColourId, theme::error);
    }
    else if (hasSample)
    {
        const double seconds = (double) proc.getSampleLength() / proc.getSourceSampleRate();
        const int minutes = (int) (seconds / 60.0);
        const auto duration = minutes > 0 ? juce::String (minutes) + " min " + juce::String ((int) seconds % 60) + " s"
                                          : juce::String (seconds, 1) + " s";
        fileLabel.setText (proc.getFileName() + fr ("  ·  ") + duration, juce::dontSendNotification);
        fileLabel.setColour (juce::Label::textColourId, theme::text);
    }
    else if (proc.getMissingFilePath().isNotEmpty())
    {
        fileLabel.setText (fr ("Fichier introuvable : ") + juce::File (proc.getMissingFilePath()).getFileName(), juce::dontSendNotification);
        fileLabel.setColour (juce::Label::textColourId, theme::error);
    }
    else
    {
        fileLabel.setText (fr ("Aucun sample chargé"), juce::dontSendNotification);
        fileLabel.setColour (juce::Label::textColourId, theme::textDim);
    }

    waveform.setEmptyMessage (proc.getMissingFilePath().isNotEmpty()
                                  ? fr ("Le sample de ce projet est introuvable :\n") + proc.getMissingFilePath()
                                        + fr ("\nRecharge-le pour retrouver ta découpe.")
                                  : juce::String());

    // Barre de découpe
    transientsButton.setToggleState (mode == SliceMode::transients, juce::dontSendNotification);
    gridButton.setToggleState (mode == SliceMode::grid, juce::dontSendNotification);
    manualButton.setToggleState (mode == SliceMode::manual, juce::dontSendNotification);

    sensitivitySlider.setVisible (mode == SliceMode::transients);
    divisionsBox.setVisible (mode == SliceMode::grid);
    optionLabel.setVisible (mode != SliceMode::manual);
    optionLabel.setText (mode == SliceMode::transients ? fr ("Sensibilité") : fr ("Pas"), juce::dontSendNotification);

    sensitivitySlider.setValue (proc.getSensitivity(), juce::dontSendNotification);
    divisionsBox.setSelectedId (divisionsToId (proc.getDivisions()), juce::dontSendNotification);

    if (divisionsBox.getSelectedId() == 0)
        divisionsBox.setSelectedId (16, juce::dontSendNotification);

    const int count = proc.getNumSlices();
    countLabel.setText (hasSample ? juce::String (count) + (count > 1 ? " tranches" : " tranche") : juce::String(),
                        juce::dontSendNotification);

    // Barre de tempo et de jeu
    bpmLabel.setText (hasSample ? juce::String (proc.getSampleBpm(), 1) : juce::String ("-"), juce::dontSendNotification);
    bpmLabel.setColour (juce::Label::textColourId, proc.isBpmManual() ? theme::accent : theme::text);
    autoButton.setEnabled (hasSample && proc.isBpmManual());
    syncButton.setToggleState (proc.isSyncEnabled(), juce::dontSendNotification);
    padsButton.setToggleState (proc.getPlayMode() == PlayMode::pads, juce::dontSendNotification);
    keysButton.setToggleState (proc.getPlayMode() == PlayMode::keys, juce::dontSendNotification);
    monoButton.setToggleState (proc.isMono(), juce::dontSendNotification);
    updateHostLabel();

    // Barre de boucle
    shownPlaying = proc.isPatternPlaying();
    listenButton.setToggleState (shownPlaying, juce::dontSendNotification);
    listenButton.setButtonText (shownPlaying ? juce::String ("Stop") : fr ("Écouter"));
    originalButton.setEnabled (hasSample && proc.isPatternFlipped());
    flipButton.setEnabled (hasSample && count > 1);

    if (! hasSample)
        loopInfoLabel.setText ({}, juce::dontSendNotification);
    else if (proc.isPatternFlipped())
        loopInfoLabel.setText (proc.isMono() ? fr ("Ordre réarrangé")
                                             : fr ("Ordre réarrangé. Active Mono pour que les tranches ne se chevauchent pas."),
                               juce::dontSendNotification);
    else
        loopInfoLabel.setText (fr ("Ordre d'origine"), juce::dontSendNotification);

    for (auto* c : std::initializer_list<juce::Component*> { &transientsButton, &gridButton, &manualButton, &sensitivitySlider,
                                                              &divisionsBox, &bpmLabel, &halfButton, &doubleButton, &syncButton,
                                                              &listenButton, &midiDrag })
        c->setEnabled (hasSample);

    // Aide contextuelle
    if (! hasSample)
    {
        hintLabel.setText ({}, juce::dontSendNotification);
    }
    else
    {
        const double seconds = (double) proc.getSampleLength() / proc.getSourceSampleRate();
        const bool fullRegion = proc.getRegionStart() == 0 && proc.getRegionEnd() == proc.getSampleLength();

        hintLabel.setText (seconds > 30.0 && fullRegion
                               ? fr ("Fichier long : glisse dans le bandeau turquoise du haut pour choisir la zone à découper. Molette : zoom.")
                               : fr ("Clic : écouter  ·  Double-clic : ajouter un point  ·  Glisser un point : le déplacer  ·  Clic droit sur un point : le supprimer  ·  Molette : zoom  ·  Bandeau du haut : zone à découper"),
                           juce::dontSendNotification);
    }

    pads.showSlice (proc.getSelectedSlice());
    slicePanel.refresh();
    waveform.repaint();
    midiDrag.repaint();
}
