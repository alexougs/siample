#include "PluginEditor.h"

using namespace siample;

namespace
{
    const int divisionChoices[] = { 2, 4, 8, 16, 32, 64 };

    // Les identifiants d'un menu doivent être positifs : on décale les valeurs musicales
    constexpr int musicalIdOffset = 100;

    constexpr int minimumWidth = 1040;
    constexpr int browserWidth = 232;

    int divisionsToId (int divisions)   { return divisions > 0 ? divisions : musicalIdOffset - divisions; }
    int idToDivisions (int id)          { return id > musicalIdOffset ? musicalIdOffset - id : id; }

    juce::String keyName (int root, bool minor)
    {
        static const char* names[] = { "Do", "Do#", "R\xc3\xa9", "R\xc3\xa9#", "Mi", "Fa", "Fa#", "Sol", "Sol#", "La", "La#", "Si" };
        return fr (names[((root % 12) + 12) % 12]) + (minor ? " min" : " maj");
    }

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

    void styleButton (juce::TextButton& button, const juce::String& text, const juce::String& tooltip)
    {
        button.setButtonText (text);
        button.setTooltip (tooltip);
    }
}

void SiampleEditor::styleCaption (juce::Label& label, const juce::String& text)
{
    label.setText (upper (text), juce::dontSendNotification);
    label.setFont (theme::heading (11.5f).withExtraKerningFactor (0.08f));
    label.setColour (juce::Label::textColourId, theme::textDim);
    label.setMinimumHorizontalScale (0.8f);
    label.setBorderSize (juce::BorderSize<int> (0));
}

void SiampleEditor::styleValueBox (juce::Label& label, const juce::String& tooltip)
{
    label.setFont (theme::heading (14.0f));
    label.setJustificationType (juce::Justification::centred);
    label.setColour (juce::Label::backgroundColourId, theme::panelLight);
    label.setColour (juce::Label::outlineColourId, theme::outline);
    label.setColour (juce::Label::backgroundWhenEditingColourId, theme::panelLight);
    label.setColour (juce::Label::textWhenEditingColourId, theme::text);
    label.setColour (juce::Label::outlineWhenEditingColourId, theme::accent);
    label.setTooltip (tooltip);
}

//==============================================================================
SiampleEditor::SiampleEditor (SiampleProcessor& p)
    : AudioProcessorEditor (&p), proc (p), waveform (p),
      timelineAudioDrag (fr ("Glisser l'audio"),
                         fr ("Attrape ce bouton et glisse-le dans la playlist du DAW : "
                             "le montage y est déposé en audio, au tempo du projet, tel que tu l'entends.")),
      timelineMidiDrag (fr ("Glisser le MIDI"),
                        fr ("Attrape ce bouton et glisse-le sur la piste Siample du DAW : le montage y est déposé en notes. "
                            "Chaque sample a son canal MIDI (A = 2, B = 3, C = 4, D = 5).")),
      timeline (p), browser (p), pads (p), slicePanel (p)
{
    setLookAndFeel (&lookAndFeel);

    // --- Ligne du haut
    styleButton (sliceViewButton, fr ("Découpe"), fr ("Découper le sample affiché : forme d'onde, points de coupe, tempo"));
    styleButton (montageViewButton, "Montage", fr ("Assembler les tranches de tes samples sur une grille, écouter et exporter"));
    makeSegment ({ &sliceViewButton, &montageViewButton }, 7104);
    sliceViewButton.onClick   = [this] { if (sliceViewButton.getToggleState())   showMontage (false); };
    montageViewButton.onClick = [this] { if (montageViewButton.getToggleState()) showMontage (true); };

    styleButton (undoButton, "Annuler", fr ("Annule la dernière modification"));
    styleButton (redoButton, fr ("Rétablir"), fr ("Refait la modification annulée"));
    undoButton.setConnectedEdges (juce::Button::ConnectedOnRight);
    redoButton.setConnectedEdges (juce::Button::ConnectedOnLeft);
    undoButton.onClick = [this] { proc.undo(); };
    redoButton.onClick = [this] { proc.redo(); };

    styleToggle (browseButton, "Parcourir", fr ("Ouvre le navigateur de samples : ton dossier, avec pré-écoute"));
    browseButton.onClick = [this] { showBrowser (browseButton.getToggleState()); };

    styleButton (menuButton, "Menu", fr ("Presets, export de toutes les tranches, vider le sample"));
    menuButton.onClick = [this] { showMenu(); };

    for (auto* b : { &sliceViewButton, &montageViewButton, &undoButton, &redoButton, &browseButton, &menuButton })
        addAndMakeVisible (*b);

    styleCaption (projectCaption, "Projet");
    projectCaption.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (projectCaption);

    styleValueBox (projectLabel, {});
    projectLabel.onTextChange = [this]
    {
        const double typed = projectLabel.getText().replaceCharacter (',', '.').retainCharacters ("0123456789.").getDoubleValue();

        if (typed >= 30.0 && typed <= 300.0)
            proc.setProjectBpm (typed);

        shownHostBpm = 0.0;   // force la mise à jour
        updateStatus();
    };
    addAndMakeVisible (projectLabel);

    styleToggle (velocityButton, fr ("Vélocité fixe"), fr ("Toutes les notes jouent à fond, quelle que soit la force de frappe (le « full level » des MPC)"));
    velocityButton.onClick = [this] { proc.setFullVelocity (velocityButton.getToggleState()); };
    addAndMakeVisible (velocityButton);

    styleCaption (repeatCaption, fr ("Répét."));
    repeatCaption.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (repeatCaption);

    repeatBox.addItem ("Non", 1);
    repeatBox.addItem ("1/4", 4);
    repeatBox.addItem ("1/8", 8);
    repeatBox.addItem ("1/16", 16);
    repeatBox.addItem ("1/32", 32);
    repeatBox.setTooltip (fr ("Note repeat : tant que tu tiens un pad, la tranche se répète à cette valeur, calée sur le tempo du projet"));
    repeatBox.onChange = [this] { proc.setNoteRepeat (repeatBox.getSelectedId() == 1 ? 0 : repeatBox.getSelectedId()); };
    addAndMakeVisible (repeatBox);

    styleRotary (grainSlider, this, " %", fr ("Grain vintage : le son granuleux des vieux samplers (moins de bits, moins d'aigus)"));
    addAndMakeVisible (grainSlider);
    grainAttachment = std::make_unique<juce::SliderParameterAttachment> (*proc.getGrainParameter(), grainSlider);
    grainSlider.setDoubleClickReturnValue (true, 0.0);

    styleRotary (masterSlider, this, " dB", fr ("Volume général du plugin"));
    addAndMakeVisible (masterSlider);
    masterAttachment = std::make_unique<juce::SliderParameterAttachment> (*proc.getMasterGainParameter(), masterSlider);
    masterSlider.setDoubleClickReturnValue (true, 0.0);

    styleCaption (grainCaption, "Grain");
    styleCaption (masterCaption, "Volume");

    for (auto* l : { &grainCaption, &masterCaption })
    {
        l->setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (*l);
    }

    // --- Ligne du sample
    for (int i = 0; i < numDecks; ++i)
    {
        auto& b = deckButtons[(size_t) i];
        b.setButtonText (deckName (i));
        b.setClickingTogglesState (true);
        b.setRadioGroupId (7105);
        b.setConnectedEdges ((i > 0 ? juce::Button::ConnectedOnLeft : 0) | (i < numDecks - 1 ? juce::Button::ConnectedOnRight : 0));
        b.onClick = [this, i]
        {
            if (deckButtons[(size_t) i].getToggleState() && proc.getActiveDeck() != i)
            {
                proc.setActiveDeck (i);
                errorMessage.clear();
                changeListenerCallback (nullptr);
            }
        };
        addAndMakeVisible (b);
    }

    styleButton (loadButton, "Charger", fr ("Charge un fichier audio dans le sample affiché (ou glisse-le sur la fenêtre)"));
    loadButton.onClick = [this] { openFileChooser(); };
    addAndMakeVisible (loadButton);

    fileLabel.setFont (theme::font (13.0f));
    fileLabel.setMinimumHorizontalScale (0.75f);
    addAndMakeVisible (fileLabel);

    styleCaption (tempoCaption, "Tempo");
    tempoCaption.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (tempoCaption);

    styleValueBox (bpmLabel, fr ("Tempo du sample, détecté automatiquement. Clique pour le corriger à la main."));
    bpmLabel.setEditable (true);
    bpmLabel.onTextChange = [this]
    {
        const double typed = bpmLabel.getText().replaceCharacter (',', '.').retainCharacters ("0123456789.").getDoubleValue();

        if (typed >= 30.0 && typed <= 300.0)
            proc.setSampleBpm (typed);

        refresh();
    };
    addAndMakeVisible (bpmLabel);

    styleButton (halfButton, fr ("÷2"), fr ("Divise le tempo par deux, si la détection a compté deux fois trop vite"));
    halfButton.onClick = [this] { proc.setSampleBpm (proc.getSampleBpm() * 0.5); refresh(); };
    styleButton (doubleButton, fr ("×2"), fr ("Double le tempo, si la détection a compté deux fois trop lentement"));
    doubleButton.onClick = [this] { proc.setSampleBpm (proc.getSampleBpm() * 2.0); refresh(); };
    styleButton (autoButton, "Auto", fr ("Relance la détection automatique du tempo sur la zone de travail"));
    autoButton.onClick = [this] { proc.detectBpm(); refresh(); };
    halfButton.setConnectedEdges (juce::Button::ConnectedOnRight);
    doubleButton.setConnectedEdges (juce::Button::ConnectedOnLeft);

    styleToggle (syncButton, "Sync", fr ("Les tranches de ce sample suivent le tempo du projet, et le pitch ne change plus leur durée. "
                                         "À activer sur chaque sample pour qu'ils tombent ensemble dans le montage."));
    syncButton.onClick = [this] { proc.setSyncEnabled (syncButton.getToggleState()); refresh(); };

    for (auto* b : { &halfButton, &doubleButton, &autoButton, &syncButton })
        addAndMakeVisible (*b);

    styleCaption (keyCaption, fr ("Tonalité"));
    keyCaption.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (keyCaption);

    keyLabel.setFont (theme::font (13.0f, true));
    keyLabel.setJustificationType (juce::Justification::centred);
    keyLabel.setMinimumHorizontalScale (0.75f);
    keyLabel.setTooltip (fr ("Tonalité estimée du sample (après transposition). C'est une estimation : fie-toi à ton oreille."));
    addAndMakeVisible (keyLabel);

    styleButton (transposeDownButton, "-", fr ("Transpose tout le sample d'un demi-ton vers le bas"));
    styleButton (transposeUpButton, "+", fr ("Transpose tout le sample d'un demi-ton vers le haut"));
    transposeDownButton.setConnectedEdges (juce::Button::ConnectedOnRight);
    transposeUpButton.setConnectedEdges (juce::Button::ConnectedOnLeft);
    transposeDownButton.onClick = [this] { proc.deck().setTranspose (proc.deck().getTranspose() - 1); refresh(); };
    transposeUpButton.onClick   = [this] { proc.deck().setTranspose (proc.deck().getTranspose() + 1); refresh(); };
    addAndMakeVisible (transposeDownButton);
    addAndMakeVisible (transposeUpButton);

    transposeLabel.setFont (theme::font (13.0f));
    transposeLabel.setJustificationType (juce::Justification::centred);
    transposeLabel.setColour (juce::Label::backgroundColourId, theme::panelLight);
    transposeLabel.setTooltip (fr ("Transposition de tout le sample, en demi-tons. Avec Sync, la durée ne change pas."));
    addAndMakeVisible (transposeLabel);

    targetKeyBox.setTextWhenNothingSelected (fr ("Vers…"));
    for (int root = 0; root < 12; ++root)
        targetKeyBox.addItem (keyName (root, false).upToFirstOccurrenceOf (" ", false, false), root + 1);
    targetKeyBox.setTooltip (fr ("Choisis la tonalité de ton morceau : le sample est transposé par le plus court chemin"));
    targetKeyBox.onChange = [this]
    {
        const auto key = proc.deck().getDetectedKey();

        if (targetKeyBox.getSelectedId() > 0 && key.isValid())
        {
            int shift = (targetKeyBox.getSelectedId() - 1 - key.root) % 12;

            if (shift > 6)   shift -= 12;
            if (shift < -5)  shift += 12;

            proc.deck().setTranspose (shift);
            refresh();
        }
    };
    addAndMakeVisible (targetKeyBox);

    // --- Vue Découpe
    styleCaption (sliceCaption, fr ("Découpe"));
    addAndMakeVisible (sliceCaption);

    styleButton (transientsButton, "Transitoires", fr ("Coupe automatiquement à chaque attaque (kick, snare, note…)"));
    styleButton (gridButton, "Grille", fr ("Coupe la zone en parts égales, ou en temps d'après le tempo"));
    styleButton (manualButton, "Manuel", fr ("Tu places les points toi-même : double-clic sur la forme d'onde"));
    makeSegment ({ &transientsButton, &gridButton, &manualButton }, 7100);

    juce::TextButton* modeButtons[] = { &transientsButton, &gridButton, &manualButton };

    for (int i = 0; i < 3; ++i)
    {
        auto* b = modeButtons[i];
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

    optionLabel.setFont (theme::font (12.0f));
    optionLabel.setColour (juce::Label::textColourId, theme::textDim);
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

    countLabel.setFont (theme::font (13.0f, true));
    countLabel.setMinimumHorizontalScale (0.8f);
    addAndMakeVisible (countLabel);

    styleButton (normaliseButton, "Niveler", fr ("Met toutes les tranches au même niveau : les chops faibles sortent aussi fort que les autres"));
    normaliseButton.onClick = [this] { proc.deck().normaliseSlices(); refresh(); };
    addAndMakeVisible (normaliseButton);

    styleCaption (playCaption, "Jeu");
    playCaption.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (playCaption);

    styleButton (padsButton, "Pads", fr ("Une note par tranche : la tranche 1 est sur la note ") + noteName (baseNote));
    styleButton (keysButton, "Clavier", fr ("Tout le clavier joue la tranche sélectionnée ; la note ")
                                            + noteName (keyboardRootNote) + fr (" donne sa hauteur d'origine"));
    makeSegment ({ &padsButton, &keysButton }, 7103);
    padsButton.onClick = [this] { if (padsButton.getToggleState()) { proc.setPlayMode (PlayMode::pads); refresh(); } };
    keysButton.onClick = [this] { if (keysButton.getToggleState()) { proc.setPlayMode (PlayMode::keys); refresh(); } };
    addAndMakeVisible (padsButton);
    addAndMakeVisible (keysButton);

    styleToggle (monoButton, "Mono", fr ("Chaque nouvelle note coupe la précédente : les tranches de ce sample ne se chevauchent plus"));
    monoButton.onClick = [this] { proc.setMono (monoButton.getToggleState()); refresh(); };
    addAndMakeVisible (monoButton);

    waveform.onEdit   = [this] { refresh(); };
    waveform.onSelect = [this] { selectionChanged(); };
    addAndMakeVisible (waveform);

    // --- Vue Montage
    styleToggle (listenButton, fr ("Écouter"), fr ("Joue le montage en boucle, au tempo du projet"));
    listenButton.onClick = [this] { proc.setTimelinePlaying (listenButton.getToggleState()); refresh(); };
    addChildComponent (listenButton);

    styleCaption (barsCaption, "Mesures");
    styleCaption (snapCaption, "Grille");

    for (auto* l : { &barsCaption, &snapCaption })
    {
        l->setJustificationType (juce::Justification::centredRight);
        addChildComponent (*l);
    }

    for (int bars : { 1, 2, 4, 8, 16 })
        barsBox.addItem (juce::String (bars), bars);
    barsBox.setTooltip (fr ("Longueur du montage, en mesures de 4 temps"));
    barsBox.onChange = [this] { if (barsBox.getSelectedId() > 0) { proc.setTimelineBars (barsBox.getSelectedId()); refresh(); } };
    addChildComponent (barsBox);

    snapBox.addItem ("Libre", 1);
    snapBox.addItem ("1/4", 4);
    snapBox.addItem ("1/8", 8);
    snapBox.addItem ("1/16", 16);
    snapBox.addItem ("1/32", 32);
    snapBox.setTooltip (fr ("Les blocs s'aimantent à cette grille. Maj enfoncée : déplacement libre."));
    snapBox.onChange = [this] { proc.setSnap (snapBox.getSelectedId() == 1 ? 0 : snapBox.getSelectedId()); refresh(); };
    addChildComponent (snapBox);

    for (int i = 0; i < numVariants; ++i)
    {
        auto& b = variantButtons[(size_t) i];
        b.setButtonText (juce::String (i + 1));
        b.setClickingTogglesState (true);
        b.setRadioGroupId (7106);
        b.setConnectedEdges ((i > 0 ? juce::Button::ConnectedOnLeft : 0) | (i < numVariants - 1 ? juce::Button::ConnectedOnRight : 0));
        b.setTooltip (fr ("Variante ") + juce::String (i + 1) + fr (" du montage : trois versions à comparer. "
                          "Une variante vide reprend la copie de celle que tu quittes."));
        b.onClick = [this, i]
        {
            if (variantButtons[(size_t) i].getToggleState() && proc.getVariant() != i)
            {
                const int previous = proc.getVariant();

                // On part de ce qu'on a déjà plutôt que d'une page blanche
                if (proc.getNumClipsIn (i) == 0 && proc.getNumClipsIn (previous) > 0)
                    proc.copyVariant (previous, i);

                proc.setVariant (i);
                refresh();
            }
        };
        addChildComponent (b);
    }

    styleButton (orderButton, "Dans l'ordre", fr ("Pose la boucle du sample affiché sur sa ligne, dans son ordre d'origine"));
    orderButton.onClick = [this] { proc.fillLane (proc.getActiveDeck(), false); refresh(); };
    styleButton (flipButton, "Flip", fr ("Réarrange au hasard les tranches du sample affiché, en gardant le rythme d'origine. Reclique pour une autre idée."));
    flipButton.onClick = [this] { proc.fillLane (proc.getActiveDeck(), true); refresh(); };
    styleButton (clearButton, "Vider", fr ("Retire tous les blocs de cette variante"));
    clearButton.onClick = [this] { proc.clearClips(); refresh(); };

    for (auto* b : { &orderButton, &flipButton, &clearButton })
        addChildComponent (*b);

    timelineInfoLabel.setFont (theme::font (12.0f));
    timelineInfoLabel.setColour (juce::Label::textColourId, theme::textDim);
    timelineInfoLabel.setJustificationType (juce::Justification::centredRight);
    timelineInfoLabel.setMinimumHorizontalScale (0.75f);
    addChildComponent (timelineInfoLabel);

    timelineAudioDrag.createFile = [this] { return proc.createTimelineAudioFile(); };
    timelineMidiDrag.createFile  = [this] { return proc.createTimelineMidiFile(); };
    addChildComponent (timelineAudioDrag);
    addChildComponent (timelineMidiDrag);

    timeline.onEdit   = [this] { refresh(); };
    timeline.onSelect = [this] { errorMessage.clear(); changeListenerCallback (nullptr); };
    addChildComponent (timeline);

    hintLabel.setFont (theme::font (11.5f));
    hintLabel.setColour (juce::Label::textColourId, theme::textDim);
    hintLabel.setMinimumHorizontalScale (0.7f);
    addAndMakeVisible (hintLabel);

    // --- Bas et navigateur
    browser.onLoad = [this] (const juce::File& file) { loadFile (file); };
    addChildComponent (browser);

    pads.onSelect = [this] { selectionChanged(); };
    addAndMakeVisible (pads);

    slicePanel.onEdit = [this] { waveform.repaint(); timeline.repaint(); undoButton.setEnabled (proc.canUndo()); };
    addAndMakeVisible (slicePanel);

    proc.addChangeListener (this);

    setResizable (true, true);
    setResizeLimits (minimumWidth, 720, 1900, 1400);
    setSize (1120, 800);

    // les composants créés avant l'application du thème reprennent ses couleurs
    sendLookAndFeelChange();

    shownSample = proc.getSample();
    waveform.sampleChanged();
    showMontage (proc.getShownView() == 1);
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
void SiampleEditor::showMontage (bool shouldShowMontage)
{
    montageVisible = shouldShowMontage;
    proc.setShownView (montageVisible ? 1 : 0);
    (montageVisible ? montageViewButton : sliceViewButton).setToggleState (true, juce::dontSendNotification);

    for (auto* c : std::initializer_list<juce::Component*> { &sliceCaption, &transientsButton, &gridButton, &manualButton, &optionLabel,
                                                              &countLabel, &normaliseButton, &playCaption, &padsButton, &keysButton,
                                                              &monoButton, &waveform })
        c->setVisible (! montageVisible);

    for (auto* c : std::initializer_list<juce::Component*> { &listenButton, &barsCaption, &barsBox, &snapCaption, &snapBox, &orderButton,
                                                              &flipButton, &clearButton, &timelineInfoLabel, &timelineAudioDrag,
                                                              &timelineMidiDrag, &timeline })
        c->setVisible (montageVisible);

    for (auto& b : variantButtons)
        b.setVisible (montageVisible);

    resized();
    refresh();
}

void SiampleEditor::showBrowser (bool shouldShow)
{
    if (browserVisible == shouldShow)
        return;

    browserVisible = shouldShow;
    browseButton.setToggleState (shouldShow, juce::dontSendNotification);
    browser.setVisible (shouldShow);

    if (! shouldShow)
        proc.stopPreview();

    // Le navigateur s'ajoute à gauche : la fenêtre s'élargit d'autant, le reste ne bouge pas
    const int extra = browserWidth + 12;
    const int width = getWidth() + (shouldShow ? extra : -extra);
    setResizeLimits (minimumWidth + (shouldShow ? extra : 0), 720, 1900 + extra, 1400);
    setSize (juce::jmax (minimumWidth, width), getHeight());
    resized();
    repaint();
}

//==============================================================================
void SiampleEditor::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);

    // Nom du plugin
    juce::AttributedString name;
    name.setJustification (juce::Justification::centredLeft);
    name.append ("Si", theme::heading (23.0f), theme::accent);
    name.append ("ample", theme::heading (23.0f), theme::text);
    name.draw (g, juce::Rectangle<float> (18.0f, 12.0f, 110.0f, 36.0f));

    theme::drawCard (g, middleCard);
}

void SiampleEditor::resized()
{
    auto area = getLocalBounds().reduced (18, 0);
    area.removeFromTop (12);
    area.removeFromBottom (14);

    // --- Ligne du haut
    {
        auto row = area.removeFromTop (36).reduced (0, 2);
        // Fenêtre étroite : les deux potards se passent de leur nom (il reste dans l'info-bulle)
        const bool compact = row.getWidth() < 1070;
        grainCaption.setVisible (! compact);
        masterCaption.setVisible (! compact);

        row.removeFromLeft (106);
        sliceViewButton.setBounds (row.removeFromLeft (76));
        montageViewButton.setBounds (row.removeFromLeft (76));
        row.removeFromLeft (10);
        undoButton.setBounds (row.removeFromLeft (62));
        redoButton.setBounds (row.removeFromLeft (62));
        row.removeFromLeft (10);
        browseButton.setBounds (row.removeFromLeft (76));
        row.removeFromLeft (6);
        menuButton.setBounds (row.removeFromLeft (54));

        masterSlider.setBounds (row.removeFromRight (36).expanded (0, 2));
        masterCaption.setBounds (row.removeFromRight (compact ? 4 : 54));
        grainSlider.setBounds (row.removeFromRight (36).expanded (0, 2));
        grainCaption.setBounds (row.removeFromRight (compact ? 4 : 46));
        row.removeFromRight (8);
        repeatBox.setBounds (row.removeFromRight (68));
        row.removeFromRight (6);
        repeatCaption.setBounds (row.removeFromRight (48));
        row.removeFromRight (8);
        velocityButton.setBounds (row.removeFromRight (94));
        row.removeFromRight (10);
        projectLabel.setBounds (row.removeFromRight (58));
        row.removeFromRight (6);
        projectCaption.setBounds (row.removeFromRight (juce::jmin (58, juce::jmax (0, row.getWidth()))));
    }

    area.removeFromTop (8);

    // --- Ligne du sample
    {
        auto row = area.removeFromTop (32);

        for (auto& b : deckButtons)
            b.setBounds (row.removeFromLeft (36));

        row.removeFromLeft (10);
        loadButton.setBounds (row.removeFromLeft (74));
        row.removeFromLeft (10);

        targetKeyBox.setBounds (row.removeFromRight (76));
        row.removeFromRight (6);
        transposeUpButton.setBounds (row.removeFromRight (26));
        transposeLabel.setBounds (row.removeFromRight (48));
        transposeDownButton.setBounds (row.removeFromRight (26));
        row.removeFromRight (4);
        keyLabel.setBounds (row.removeFromRight (70));
        keyCaption.setBounds (row.removeFromRight (72));
        row.removeFromRight (10);
        syncButton.setBounds (row.removeFromRight (62));
        row.removeFromRight (6);
        autoButton.setBounds (row.removeFromRight (48));
        row.removeFromRight (4);
        doubleButton.setBounds (row.removeFromRight (34));
        halfButton.setBounds (row.removeFromRight (34));
        row.removeFromRight (4);
        bpmLabel.setBounds (row.removeFromRight (62));
        row.removeFromRight (6);
        tempoCaption.setBounds (row.removeFromRight (52));
        row.removeFromRight (8);
        fileLabel.setBounds (row.withSizeKeepingCentre (row.getWidth(), 18));
    }

    area.removeFromTop (10);

    // --- Navigateur (à gauche, sur toute la hauteur restante)
    if (browserVisible)
    {
        browser.setBounds (area.removeFromLeft (browserWidth));
        area.removeFromLeft (12);
    }

    // --- Bas : pads et réglages de la tranche
    const int bottomHeight = juce::jlimit (268, 330, area.getHeight() * 41 / 100);
    auto bottom = area.removeFromBottom (bottomHeight);
    pads.setBounds (bottom.removeFromLeft (juce::jlimit (300, 380, bottom.getWidth() * 36 / 100)));
    bottom.removeFromLeft (12);
    slicePanel.setBounds (bottom);
    area.removeFromBottom (12);

    // --- Carte du milieu : la découpe ou le montage
    middleCard = area;
    auto card = area.reduced (14, 10);
    auto bar = card.removeFromTop (30);
    hintLabel.setBounds (card.removeFromBottom (16));
    card.removeFromBottom (4);
    card.removeFromTop (8);

    waveform.setBounds (card);
    timeline.setBounds (card);

    if (! montageVisible)
    {
        sliceCaption.setBounds (bar.removeFromLeft (70));
        transientsButton.setBounds (bar.removeFromLeft (96));
        gridButton.setBounds (bar.removeFromLeft (58));
        manualButton.setBounds (bar.removeFromLeft (66));
        bar.removeFromLeft (8);

        monoButton.setBounds (bar.removeFromRight (56));
        bar.removeFromRight (8);
        keysButton.setBounds (bar.removeFromRight (64));
        padsButton.setBounds (bar.removeFromRight (52));
        bar.removeFromRight (4);
        playCaption.setBounds (bar.removeFromRight (36));

        optionLabel.setBounds (bar.removeFromLeft (72));
        bar.removeFromLeft (8);
        const auto option = bar.removeFromLeft (juce::jlimit (90, 160, bar.getWidth() - 190));
        sensitivitySlider.setBounds (option);
        divisionsBox.setBounds (option);
        bar.removeFromLeft (12);
        countLabel.setBounds (bar.removeFromLeft (84));
        bar.removeFromLeft (4);
        normaliseButton.setBounds (bar.removeFromLeft (juce::jmin (64, juce::jmax (0, bar.getWidth()))));
    }
    else
    {
        listenButton.setBounds (bar.removeFromLeft (82));
        bar.removeFromLeft (4);
        barsCaption.setBounds (bar.removeFromLeft (66));
        bar.removeFromLeft (6);
        barsBox.setBounds (bar.removeFromLeft (56));
        snapCaption.setBounds (bar.removeFromLeft (56));
        bar.removeFromLeft (6);
        snapBox.setBounds (bar.removeFromLeft (70));
        bar.removeFromLeft (12);

        for (auto& b : variantButtons)
            b.setBounds (bar.removeFromLeft (30));

        bar.removeFromLeft (12);
        orderButton.setBounds (bar.removeFromLeft (94));
        bar.removeFromLeft (4);
        flipButton.setBounds (bar.removeFromLeft (46));
        bar.removeFromLeft (4);
        clearButton.setBounds (bar.removeFromLeft (52));

        timelineMidiDrag.setBounds (bar.removeFromRight (116));
        bar.removeFromRight (6);
        timelineAudioDrag.setBounds (bar.removeFromRight (116));
        bar.removeFromRight (8);
        timelineInfoLabel.setBounds (bar);
    }
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
    const auto start = proc.hasSample() ? proc.getSample()->file.getParentDirectory() : browser.getFolder();

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
void SiampleEditor::showMenu()
{
    juce::PopupMenu menu;
    menu.addItem (1, fr ("Enregistrer un preset…"));
    menu.addItem (2, fr ("Charger un preset…"));
    menu.addSeparator();
    menu.addItem (3, fr ("Exporter toutes les tranches du sample ") + deckName (proc.getActiveDeck()) + " en WAV",
                  proc.hasSample() && proc.getNumSlices() > 0);
    menu.addItem (4, fr ("Vider le sample ") + deckName (proc.getActiveDeck()),
                  proc.hasSample() || proc.getMissingFilePath().isNotEmpty());

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (menuButton),
                        [safe = juce::Component::SafePointer<SiampleEditor> (this)] (int choice)
                        {
                            if (safe == nullptr)
                                return;

                            if (choice == 1)  safe->savePreset();
                            if (choice == 2)  safe->loadPreset();

                            if (choice == 3)
                            {
                                // Les fichiers sont écrits dans un dossier, qu'on ouvre : il n'y a plus qu'à les glisser où l'on veut
                                const auto files = safe->proc.createAllSliceAudioFiles();

                                if (! files.isEmpty())
                                    files.getFirst().revealToUser();
                            }

                            if (choice == 4)
                            {
                                safe->proc.deck().unload();
                                safe->errorMessage.clear();
                                safe->changeListenerCallback (nullptr);
                            }
                        });
}

void SiampleEditor::savePreset()
{
    const auto folder = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Siample");
    folder.createDirectory();
    const auto name = proc.hasSample() ? proc.getSample()->file.getFileNameWithoutExtension() : juce::String ("Preset");

    chooser = std::make_unique<juce::FileChooser> (fr ("Enregistrer le preset"), folder.getChildFile (name + ".siample"), "*.siample");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this] (const juce::FileChooser& fc)
                          {
                              auto file = fc.getResult();

                              if (file == juce::File())
                                  return;

                              if (! proc.savePreset (file.withFileExtension ("siample")))
                              {
                                  errorMessage = fr ("Impossible d'écrire le preset");
                                  refresh();
                              }
                          });
}

void SiampleEditor::loadPreset()
{
    const auto folder = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Siample");

    chooser = std::make_unique<juce::FileChooser> (fr ("Charger un preset"), folder, "*.siample");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              if (fc.getResult().existsAsFile())
                              {
                                  errorMessage = proc.loadPreset (fc.getResult());
                                  changeListenerCallback (nullptr);
                              }
                          });
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

    lastMask = ~proc.getActiveMask();   // force la mise à jour des pads allumés
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
    timeline.setPlayhead (proc.isTimelinePlaying() ? proc.getTimelinePosition() : -1.0);
    browser.timerTick();

    if (std::abs (proc.getHostBpm() - shownHostBpm) > 0.005 || proc.isRendering() != shownRendering
        || proc.isTempoFromHost() != shownTempoFromHost)
        updateStatus();

    if (proc.isTimelinePlaying() != shownPlaying)
        refresh();
}

void SiampleEditor::selectionChanged()
{
    pads.showSlice (proc.getSelectedSlice());
    slicePanel.refresh();
    waveform.repaint();
}

void SiampleEditor::updateStatus()
{
    shownHostBpm = proc.getHostBpm();
    shownRendering = proc.isRendering();
    shownTempoFromHost = proc.isTempoFromHost();

    if (! projectLabel.isBeingEdited())
        projectLabel.setText (juce::String (shownHostBpm, 1), juce::dontSendNotification);

    projectLabel.setEditable (! shownTempoFromHost);
    projectLabel.setTooltip (shownTempoFromHost ? fr ("Tempo du projet, donné par le DAW")
                                                : fr ("Tempo du projet. Le DAW ne l'a pas encore donné : clique pour le régler à la main."));

    syncButton.setButtonText (proc.isSyncEnabled() && proc.hasSample() && proc.deck().isRendering() ? fr ("Sync…") : juce::String ("Sync"));

    // Les durées affichées dépendent du tempo du projet
    timeline.repaint();
}

void SiampleEditor::refresh()
{
    const bool hasSample = proc.hasSample();
    const auto mode = proc.getSliceMode();
    const int count = proc.getNumSlices();
    const int active = proc.getActiveDeck();
    auto& deck = proc.deck();

    undoButton.setEnabled (proc.canUndo());
    redoButton.setEnabled (proc.canRedo());
    velocityButton.setToggleState (proc.isFullVelocity(), juce::dontSendNotification);
    repeatBox.setSelectedId (proc.getNoteRepeat() == 0 ? 1 : proc.getNoteRepeat(), juce::dontSendNotification);

    // Onglets des samples
    for (int i = 0; i < numDecks; ++i)
    {
        auto& b = deckButtons[(size_t) i];
        const auto& d = proc.getDeck (i);
        b.setToggleState (i == active, juce::dontSendNotification);
        b.setColour (juce::TextButton::textColourOffId, d.hasSample() ? theme::text : theme::textDim.withAlpha (0.6f));
        b.setTooltip (d.hasSample() ? fr ("Sample ") + deckName (i) + " : " + d.getFileName()
                                    : fr ("Sample ") + deckName (i) + fr (" : vide. Clique pour l'afficher, puis charge un fichier."));
    }

    // Nom du fichier ou message d'erreur
    if (errorMessage.isNotEmpty())
    {
        fileLabel.setText (errorMessage, juce::dontSendNotification);
        fileLabel.setColour (juce::Label::textColourId, theme::error);
    }
    else if (hasSample)
    {
        const double seconds = (double) proc.getSampleLength() / proc.getSourceSampleRate();
        fileLabel.setText (proc.getFileName() + fr ("  ·  ") + juce::String (seconds, 1) + " s", juce::dontSendNotification);
        fileLabel.setColour (juce::Label::textColourId, theme::text);
    }
    else if (proc.getMissingFilePath().isNotEmpty())
    {
        fileLabel.setText (fr ("Fichier introuvable : ") + juce::File (proc.getMissingFilePath()).getFileName(), juce::dontSendNotification);
        fileLabel.setColour (juce::Label::textColourId, theme::error);
    }
    else
    {
        fileLabel.setText (fr ("Sample ") + deckName (active) + fr (" vide"), juce::dontSendNotification);
        fileLabel.setColour (juce::Label::textColourId, theme::textDim);
    }

    waveform.setEmptyMessage (proc.getMissingFilePath().isNotEmpty()
                                  ? fr ("Le fichier audio de ce projet est introuvable :\n") + proc.getMissingFilePath()
                                        + fr ("\nRecharge-le pour retrouver ta découpe.")
                                  : juce::String());

    // Tempo et tonalité
    bpmLabel.setText (hasSample ? juce::String (proc.getSampleBpm(), 1) : juce::String ("-"), juce::dontSendNotification);
    bpmLabel.setColour (juce::Label::textColourId, proc.isBpmManual() ? theme::accent : theme::text);
    syncButton.setToggleState (proc.isSyncEnabled(), juce::dontSendNotification);

    const auto key = deck.getDetectedKey();
    const int transpose = deck.getTranspose();
    keyLabel.setText (hasSample && key.isValid() ? keyName (key.root + transpose, key.minor) : juce::String ("-"), juce::dontSendNotification);
    keyLabel.setColour (juce::Label::textColourId, transpose != 0 ? theme::accent : theme::text);
    transposeLabel.setText ((transpose > 0 ? "+" : "") + juce::String (transpose) + " st", juce::dontSendNotification);
    targetKeyBox.setSelectedId (hasSample && key.isValid() && transpose != 0 ? ((key.root + transpose) % 12 + 12) % 12 + 1 : 0,
                                juce::dontSendNotification);

    for (auto* c : std::initializer_list<juce::Component*> { &bpmLabel, &halfButton, &doubleButton, &autoButton, &syncButton,
                                                              &transposeDownButton, &transposeUpButton, &transposeLabel })
        c->setEnabled (hasSample);

    targetKeyBox.setEnabled (hasSample && key.isValid());
    autoButton.setEnabled (hasSample && proc.isBpmManual());

    // Découpe
    (mode == SliceMode::transients ? transientsButton : mode == SliceMode::grid ? gridButton : manualButton)
        .setToggleState (true, juce::dontSendNotification);

    sensitivitySlider.setValue (proc.getSensitivity(), juce::dontSendNotification);
    divisionsBox.setSelectedId (divisionsToId (proc.getDivisions()), juce::dontSendNotification);

    if (! montageVisible)
    {
        sensitivitySlider.setVisible (mode == SliceMode::transients);
        divisionsBox.setVisible (mode == SliceMode::grid);
        optionLabel.setText (mode == SliceMode::transients ? fr ("Sensibilité") : mode == SliceMode::grid ? juce::String ("Pas") : juce::String(),
                             juce::dontSendNotification);
    }
    else
    {
        sensitivitySlider.setVisible (false);
        divisionsBox.setVisible (false);
    }

    countLabel.setText (hasSample ? juce::String (count) + (count > 1 ? " tranches" : " tranche") : juce::String(), juce::dontSendNotification);
    (proc.getPlayMode() == PlayMode::keys ? keysButton : padsButton).setToggleState (true, juce::dontSendNotification);
    monoButton.setToggleState (proc.isMono(), juce::dontSendNotification);

    for (auto* c : std::initializer_list<juce::Component*> { &transientsButton, &gridButton, &manualButton, &sensitivitySlider, &divisionsBox,
                                                              &normaliseButton, &padsButton, &keysButton, &monoButton })
        c->setEnabled (hasSample);

    // Montage
    const int clips = proc.getNumClips();
    shownPlaying = proc.isTimelinePlaying();
    listenButton.setToggleState (shownPlaying, juce::dontSendNotification);
    listenButton.setButtonText (shownPlaying ? juce::String ("Stop") : fr ("Écouter"));
    barsBox.setSelectedId (proc.getTimelineBars(), juce::dontSendNotification);
    snapBox.setSelectedId (proc.getSnap() == 0 ? 1 : proc.getSnap(), juce::dontSendNotification);

    for (int i = 0; i < numVariants; ++i)
        variantButtons[(size_t) i].setToggleState (i == proc.getVariant(), juce::dontSendNotification);

    orderButton.setEnabled (hasSample && count > 0);
    flipButton.setEnabled (hasSample && count > 1);
    clearButton.setEnabled (clips > 0);
    listenButton.setEnabled (clips > 0 || shownPlaying);
    timelineAudioDrag.setEnabled (clips > 0);
    timelineMidiDrag.setEnabled (clips > 0);

    {
        const double seconds = proc.getTimelineBeats() * 60.0 / proc.getHostBpm();
        timelineInfoLabel.setText (clips == 0 ? juce::String()
                                              : juce::String (clips) + (clips > 1 ? " blocs" : " bloc") + fr ("  ·  ")
                                                    + juce::String (seconds, 1) + " s",
                                   juce::dontSendNotification);
    }

    hintLabel.setText (montageVisible
                           ? fr ("Glisser un pad : poser un bloc  ·  Glisser un bloc : le déplacer  ·  Bord droit : sa durée  ·  "
                                 "Alt + glisser : dupliquer  ·  Clic droit : retirer  ·  Lettre d'une ligne : afficher ce sample")
                           : hasSample ? fr ("Clic : écouter  ·  Double-clic : ajouter un point  ·  Glisser un point : le déplacer  ·  "
                                             "Clic droit sur un point : le supprimer  ·  Molette : zoom  ·  Bandeau du haut : zone à découper")
                                       : juce::String(),
                       juce::dontSendNotification);

    updateStatus();
    pads.showSlice (proc.getSelectedSlice());
    slicePanel.refresh();
    waveform.repaint();
    timeline.clipsChanged();
}
